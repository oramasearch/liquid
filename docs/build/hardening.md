# Hardening, sanitizers and fuzzing

liquid reads bytes from peers, servers, stores and files other people wrote: sync frames, object
records, drips, identity documents, HTTP/1 and HTTP/2, Git objects, protobuf, manifests and lock
records. This page says how the build defends against bad bytes and how to look for them: the
hardening flags every build carries, the sanitizer configuration, and the libFuzzer harnesses.
It came from the October 2026 security review, finding L-25 (F10, F30), tracked as liquid
[#407](https://github.com/oramasearch/liquid/issues/407).

**No CI runs any of this.** The org rule is no CI. The runs are local, or on the Linux BuildKit lane
below, and their results are recorded here by whoever runs them.

The flags live in [`cmake/Hardening.cmake`](../../cmake/Hardening.cmake), included right after
`project()` and before the first dependency, so they reach libliquid, the CLI, the tests and every
dependency built from source in the tree: Capy, Corosio, Boost.HTTP, zstd, BLAKE3, Monocypher,
automerge-cpp and oramacore-cpp.

## Hardening (`LIQUID_HARDEN`, on by default)

| Protection | Where |
|---|---|
| `-fstack-protector-strong` | Every C and C++ target. |
| `-fstack-clash-protection` | Where the compiler accepts it for the target: Linux x86-64 and AArch64; not Darwin. |
| `-fcf-protection=full` (CET) | x86-64 only. |
| `-D_FORTIFY_SOURCE=3` | `Release`, `RelWithDebInfo` and `MinSizeRel`, the configurations that optimise, which fortification needs. Left out of sanitizer builds, where AddressSanitizer does the checking. |
| `_GLIBCXX_ASSERTIONS`, `_LIBCPP_HARDENING_MODE_FAST` | Bounds and precondition checks in libstdc++ and libc++; each library ignores the other's macro. |
| PIE | Position-independent code and executables everywhere (`CMAKE_POSITION_INDEPENDENT_CODE`, `check_pie_supported`). Apple's linker makes every arm64 executable PIE regardless. |
| Full RELRO, non-executable stack | Linux: `-z relro -z now -z noexecstack`. |

A flag the compiler would ignore for the target is left out at configure time (checked with
`-Werror`), so no build warns about an unused argument. Configure prints what was applied:

```
-- liquid: hardening -fstack-protector-strong, -D_FORTIFY_SOURCE=3 (optimised configurations), _GLIBCXX_ASSERTIONS, _LIBCPP_HARDENING_MODE_FAST, PIE
```

`-DLIQUID_HARDEN=OFF` removes all of it, for comparing performance.

To check a Linux binary, read its ELF headers:

```sh
readelf -hW build/liquid | grep Type        # DYN: position-independent
readelf -lW build/liquid | grep GNU_RELRO   # present
readelf -dW build/liquid | grep BIND_NOW    # FLAGS BIND_NOW: full RELRO
nm build/liquid | grep -c __stack_chk_fail  # non-zero
```

## Sanitizers (`LIQUID_SANITIZE`)

```sh
cmake -S . -B build-asan -G Ninja "-DLIQUID_SANITIZE=address;undefined"
cmake --build build-asan -j 6
ctest --test-dir build-asan --output-on-failure -j 6
```

Every C and C++ target is compiled and linked with the named sanitizers, plus
`-fno-sanitize-recover=all` (the first finding stops the process with a non-zero exit, so CTest
fails) and `-fno-omit-frame-pointer`. This covers what the earlier `CMAKE_CXX_FLAGS` approach
missed: `src/hash/sha256.c`, `src/sandbox/read_guard.c`, `src/sandbox/wrap_hook.c`, BLAKE3,
Monocypher and zstd.

Three programs cannot carry a sanitizer runtime, and set the target property
`LIQUID_NO_SANITIZER_RUNTIME` (`liquid_no_sanitizer_runtime()` in the CMake file):

- `liquid-wrap-hook`, which is preloaded into uninstrumented build tools;
- `liquid-read-guard` and the test fixture `liquid-build-fixture`, which are statically linked on
  Linux, and the AddressSanitizer runtime does not support static linking.

They keep UndefinedBehaviorSanitizer in trap mode (`-fsanitize-trap=undefined`), which needs no
runtime: undefined behaviour stops them with `SIGILL` instead of a report.

Any sanitizer list the compiler accepts works, for example `thread` on its own. Thread and memory
sanitizer runs are not recorded yet (see *What is not done* below).

## Fuzzing (`LIQUID_FUZZ`)

```sh
cmake -S . -B build-fuzz -G Ninja -DLIQUID_FUZZ=ON -DLIQUID_BUILD_TESTS=OFF \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-fuzz --target liquid-fuzz -j 6
build-fuzz/fuzz/liquid-fuzz-cbor build-fuzz/fuzz/corpus/cbor -max_total_time=600
```

`LIQUID_FUZZ` needs Clang and a static libliquid (the harnesses call internal decoders). It
implies `LIQUID_SANITIZE=address;undefined` unless another list is given, and adds libFuzzer
coverage instrumentation to every target. The target `liquid-fuzz` builds every harness and runs
`liquid-fuzz-seeds`, which writes the seed corpus to `build-fuzz/fuzz/corpus/<harness>/`: the
repository's fixtures (the RFC 8949 vectors, the record fixtures, the `.liquid` package files of
the import tests) and records made with liquid's own encoders, including a real two-document
identity chain made in a throwaway `LIQUID_HOME`.

Each harness is `build-fuzz/fuzz/liquid-fuzz-<name>`:

| Harness | Parsers |
|---|---|
| `sync` | The sync wire framing: metadata document, object list, pack frames. |
| `objects` | Object records: blob, tree, change, conflict, repository, attestation, action, result, action index, build trace, SBOM, ABI, execution schedule. |
| `drip` | The drip journal record and the signed drip body history verification reads. |
| `cbor` | Canonical CBOR, with a round-trip property. |
| `identity` | Identity documents (with signature verification), signatures, chains across rotations, scan attestations. |
| `http1` | The Boost.HTTP request parser as liquid's servers configure it, fed in pieces; the client's response parser and chunked decoding. |
| `http2` | HTTP/2 frames, HPACK blocks over one dynamic table, Huffman strings, gRPC framing and status. |
| `configuration` | The manifest: `.liquid` parse, check, format and lower, with its `locks` rules; stored target, source and install-intent records. |
| `resolve` | Lock records: package specs, package records, concretised specs. |
| `git` | Git loose-object headers with the identifier check, commit and tree payloads, pack deltas. |
| `protobuf` | The protobuf wire reader and the REAPI v2 messages a remote execution peer sends. |

The input layout (a selector byte, two-byte length prefixes) is described in
[`fuzz/fuzz.hpp`](../../fuzz/fuzz.hpp). Useful libFuzzer flags: `-max_total_time=<s>`,
`-jobs=<n> -workers=<n>` for several processes, `-rss_limit_mb=4096`, and `-artifact_prefix=<dir>/`
to keep crash inputs out of the working directory. A crash input replays with
`build-fuzz/fuzz/liquid-fuzz-<name> <file>`.

On Darwin two things differ, both handled by the build: Apple's ld-1053 refuses objects that carry
both libFuzzer's PC table and UndefinedBehaviorSanitizer data ("invalid r_symbolnum=1"), so the
coverage there is edge counters and comparison tracing without the table; and libFuzzer's runtime
shares the system libc++ with the harness, which makes AddressSanitizer's container-overflow check
report libFuzzer's own corpus loading, so [`fuzz/options.cpp`](../../fuzz/options.cpp) turns that
one check off on Darwin. Linux keeps both.

## On the Linux BuildKit lane

[`tools/hardening/`](../../tools/hardening/README.md) runs the same configurations on linux/amd64
(Ubuntu 24.04, Clang 19) through `buildctl`, with no Docker on the development machine. One run at
a time: the BuildKit pod is shared with the other liquid and lake lanes.

```sh
# The whole suite under ASan and UBSan:
MODE=ctest BUILD_TYPE=Debug CMAKE_OPTIONS="-DLIQUID_SANITIZE=address;undefined" \
  tools/hardening/run.sh . asan
# A hardened release build, the suite, then the ELF protections of liquid and its helpers:
MODE=ctest tools/hardening/run.sh . release
# Every harness for ten minutes (or FUZZ_HARNESSES="cbor sync"):
MODE=fuzz FUZZ_SECONDS=600 tools/hardening/run.sh . fuzz
```

The ordinary release proof is liquid-bridge's Linux lane
(`codex-work/build/liquid-linux-test/run-liquid-linux-tests.sh <checkout> <label>`), which builds
`Release` with the default options, so it builds and tests the hardened binary.

## Recorded runs

2026-10-21, macOS arm64 (Apple M-series), Homebrew Clang 22.1.8, `RelWithDebInfo`, one core per
harness. A first 60-second smoke run found five defects, fixed in the same change: CBOR nesting
stack overflow ([#457](https://github.com/oramasearch/liquid/issues/457)), Git delta allocation
([#458](https://github.com/oramasearch/liquid/issues/458)), Git tree out-of-bounds pointer
([#459](https://github.com/oramasearch/liquid/issues/459)), the HTTP/1 response parser throwing
([#460](https://github.com/oramasearch/liquid/issues/460)) and nested `include` in a manifest
([#461](https://github.com/oramasearch/liquid/issues/461)). After the fixes, each harness ran 20
minutes and then 30 more minutes from the grown corpus:

| Harness | Executions (30-minute run) | Features | Corpus | Result |
|---|---|---|---|---|
| cbor | 33.5 M | 3611 | 450 | clean |
| configuration | 6.7 M | 20081 | 2578 | clean (the 20-minute run found #461) |
| drip | 4.3 M | 2719 | 385 | clean |
| git | 93.3 M | 2009 | 424 | clean |
| http1 | 48.5 M | 4831 | 1022 | out-of-memory input saved, not triaged yet |
| http2 | 0.85 M | 2872 | 434 | clean |
| identity | 15.4 M | 5860 | 455 | clean |
| objects | 21.0 M | 3320 | 395 | clean |
| protobuf | 61.4 M | 18827 | 2949 | clean |
| resolve | 3.9 M | 6524 | 893 | clean |
| sync | 10.6 M | 4072 | 673 | clean |

The sanitizer suite (`-DLIQUID_SANITIZE=address;undefined`, macOS): 750 of 753 passed. Two tests
fail because of an out-of-bounds read in the tree-sitter-swift scanner
([#462](https://github.com/oramasearch/liquid/issues/462)), and one is a timing-sensitive socket
test that passes when run again.

## What is not done

- 24 CPU-hour runs per harness: [#464](https://github.com/oramasearch/liquid/issues/464).
- Harnesses for the remaining parsers: [#463](https://github.com/oramasearch/liquid/issues/463).
- Thread sanitizer, `-Wall` and clang-tidy: [#465](https://github.com/oramasearch/liquid/issues/465).
- The Linux sanitizer and fuzz runs through `tools/hardening/` have not been run yet.
