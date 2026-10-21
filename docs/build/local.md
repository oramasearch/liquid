# Building liquid locally

This page is the workstation build: what to install, how to configure, build, test, run, and
install, and which options exist. Continuous integration is [`ci.md`](ci.md); the liquid-on-liquid
build is [`../library/self-host.md`](../library/self-host.md); consuming the library from another
CMake project is [`../library/libliquid.md`](../library/libliquid.md).

## Prerequisites

- **A modern Clang.** `CMakeLists.txt` selects a compiler before `project()`: Homebrew LLVM
  (`/opt/homebrew/opt/llvm/bin/clang++`) on macOS, the newest of `clang++-17` through
  `clang++-22` on Linux, and `clang-cl` on Windows. Apple's Clang and Ubuntu's GCC 13 are too old
  for the Capy and Corosio coroutine stack. Set `CMAKE_CXX_COMPILER` explicitly or enable
  `LIQUID_USE_SYSTEM_COMPILER` to choose another compiler; configuration stops with instructions
  when none is found.
- **CMake 3.28 or newer and Ninja.**
- **OpenSSL and zlib**, found with `find_package`.
- **libfuse3 on Linux.** The `fuse3` pkg-config package is required at configure time. On macOS,
  macFUSE is optional: without it, lazy mounts, `wrap`, `verify`, `replay`, and the
  sandbox-dependent tests report their documented unsupported or skipped result. Windows returns
  the documented unsupported result for those commands.
- **PostgreSQL server and client tools**, optional, for the data-branch scenarios; the tests
  report the missing prerequisite when neither a server nor `LIQUID_TEST_POSTGRES_URL` exists.
- **Autoconf and Automake**, optional, for the Autotools wrap fixtures and guides.
- **Network access during configuration.** FetchContent fetches the pinned Boost core libraries,
  BLAKE3, Monocypher, zstd, GoogleTest, and the digest-pinned tree-sitter runtime and grammars
  (`LIQUID_GRAMMAR_MIRROR` names a mirror; see [`../library/grammars.md`](../library/grammars.md)).
  Every one is pinned to its content (L-28, liquid#408): a tarball by `URL_HASH`, a git source by
  the commit its tag named. The `dependencies-pinned` test fails on a tag, branch or unhashed URL.
  The private submodules need read access to `oramasearch/corosio`, `oramasearch/automerge-cpp`,
  `oramasearch/http`, and `oramasearch/oramacore-cpp`.

## Configure, build, and test

```sh
git clone --recurse-submodules git@github.com:oramasearch/liquid.git
cd liquid
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

The default configure builds the static library `build/libliquid.a`, the command-line binary
`build/liquid`, the bundled frontend modules, and the test suite. It does not fetch or build
benchmark code.

A first repository, driven from the build tree:

```sh
mkdir example && printf 'first\n' > example/readme.txt
(cd example && ../build/liquid init . && ../build/liquid status --json && ../build/liquid cut "import")
```

The executable guides in [`../guides/`](../guides/README.md) continue from there.

## Install

```sh
cmake --install build --prefix /opt/liquid
```

The prefix receives `bin/liquid`, `lib/libliquid.a`, `libexec/liquid/` (the wrap hook, the Linux
remote-action read guard, and the bundled frontend modules), `include/liquid/`, and
`lib/cmake/liquid/` so that `find_package(liquid CONFIG)` works. The complete layout, including
the exported Capy headers and the private Boost component targets, is in
[`../library/libliquid.md`](../library/libliquid.md).

## Options

| Option | Default | Effect |
|---|---|---|
| `BUILD_SHARED_LIBS` | `OFF` | Build `libliquid.so` / `.dylib` instead of `libliquid.a`. |
| `LIQUID_BUILD_CLI` | `ON` | Build the `liquid` binary; `OFF` for library-only consumers. |
| `LIQUID_BUILD_TESTS` | `ON` when liquid is the top-level project | Build the GoogleTest suite and register CTest cases. |
| `LIQUID_BUILD_FRONTENDS` | `ON` | Build the bundled tree-sitter and Clang frontend modules. |
| `LIQUID_BUILD_BENCH` | `OFF` | Add the optional `bench/chunk-index/chunk-index-bench` target and fetch its RocksDB and LMDB dependencies ([`../library/chunk-index-benchmark.md`](../library/chunk-index-benchmark.md)). |
| `LIQUID_SELF_HOST` | `OFF` | Configure the network-free `liquid wrap` build step ([`../library/self-host.md`](../library/self-host.md)). |
| `LIQUID_USE_SYSTEM_COMPILER` | unset | Skip the automatic Clang selection. |
| `LIQUID_HARDEN` | `ON` | Stack protector, stack clash and control-flow protection where the target supports them, standard library assertions, PIE, full RELRO on Linux, and `_FORTIFY_SOURCE=3` in optimised configurations ([`hardening.md`](hardening.md)). |
| `LIQUID_SANITIZE` | empty | Sanitizers for every C and C++ target, dependencies included, for example `address;undefined` ([`hardening.md`](hardening.md)). |
| `LIQUID_FUZZ` | `OFF` | Build the libFuzzer harnesses in `fuzz/` and their seed corpus; implies `address;undefined` ([`hardening.md`](hardening.md)). |

## Documentation checks

The suite carries the documentation tests, which need only the `liquid-cli` and
`liquid-reference-docs` targets and Python 3:

```sh
cmake --build build --target liquid-cli liquid-reference-docs
ctest --test-dir build --output-on-failure -R 'docs|reference|help'
```

`docs-links` resolves every internal link under `docs/` and in the root `README.md`;
`cli-help-docs` compares `--help` with the committed reference in both directions;
`cli-reference-docs` regenerates the reference from the built binary and fails when the committed
tree differs; `cli-docs-json` checks the stable JSON projection. `guides-run` executes the marked
shell blocks in `docs/guides/` on Linux. GitHub Actions builds and tests clean checkouts on Linux,
macOS, and Windows and renders the documentation site; see [`ci.md`](ci.md).


## Linking libliquid without duplicating archives

`libliquid` is a static library, so CMake carries its `PRIVATE` dependencies — Corosio, Boost.HTTP,
oramacore-cpp, the syntax frontend — to consumers for the final link. A consumer that also *names*
those targets puts the same archive on the link line twice, which Apple's linker reports as
`ld: warning: ignoring duplicate libraries`.

When a consumer needs those headers but not a second copy of the archive, ask for the usage
requirements alone:

```cmake
target_link_libraries(my-target PRIVATE
  liquid::liquid
  $<COMPILE_ONLY:Boost::corosio>)
```

The same applies transitively: `liquid-tests` links `liquid_forge_double`, which links
`liquid::liquid` publicly, so it must not name `liquid::liquid` again.

Third-party dependencies fetched with `FetchContent` are marked `SYSTEM`, and googletest's targets
additionally get `INTERFACE_SYSTEM_INCLUDE_DIRECTORIES` and `-Wno-character-conversion`, because a
sub-project compiling its own sources is not covered by system include marking. A clean configure and
build print no warnings; if yours does, that is a regression worth fixing rather than scrolling past.
