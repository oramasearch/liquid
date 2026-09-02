# Implementation: C++26

> Evidence: [`sources.md`](sources.md)
>
> liquid is written in modern C++26. This document is about what that actually
> means in 2026 — which is less than the label suggests.

## The blunt conclusion

**Very little of C++26 is portable production-ready in 2026.** The research is
unambiguous: do not make reflection, contracts, `std::execution` or the new
concurrency containers part of liquid's stable public architecture yet.

That is not an argument against targeting C++26 — `-std=c++26` gives access to
genuine improvements and positions the codebase for the next few years. It is an
argument for being disciplined about *which* C++26 features appear in load-bearing
interfaces.

Note also that compiler and standard library are separate axes: Clang does not
imply libc++, GCC does not preclude another standard library.

## C++26 feature reality check

| Feature | Status | Verdict for liquid |
|---|---|---|
| **Static reflection (P2996)** | GCC 16 experimental (`-freflection`); **no upstream Clang or MSVC** | Excellent research target, not a cross-platform dependency. Keep code-generation or manual traits as the fallback. |
| **Contracts (P2900)** | GCC 16 experimental only | **Do not rely on them for validation or security.** Use explicit checks. |
| **`std::execution` / senders-receivers (P2300)** | **No standard-library implementation anywhere** — not libstdc++, not libc++, not MSVC STL | Use NVIDIA `stdexec` (GCC 12+, Clang 16+, MSVC 14.43+) **behind liquid's own async abstraction**. The project itself labels it experimental with unstable APIs. |
| **`#embed`** | GCC 15+; Clang 19+ mainly via C23; **not MSVC** | Keep a build-time resource generator as the canonical path. |
| **Erroneous behaviour (P2795)** | GCC 16 experimental | Not a portable safety mechanism. Use `-ftrivial-auto-var-init=zero`, warnings, and sanitizers. |
| **`std::simd`** | GCC 16 partial experimental `<simd>` only | Use intrinsics, Highway or xsimd behind runtime-dispatched kernels. |
| **`std::inplace_vector`** | libstdc++ 16 only | Wrapper over `boost::container::static_vector` or an internal type. |
| **`std::hive`, hazard pointers (P2530), RCU (P2545), `std::linalg`** | **Unavailable everywhere** | Use `plf::colony` / established reclamation libraries behind private abstractions. |
| **`constexpr` expansions** (placement new, exceptions, structured bindings) | Patchy and mostly GCC-only | Feature-test individually; never assume as a blanket. |

**The practical target is therefore: C++20/23 as the working language, `-std=c++26`
enabled, and C++26 features adopted individually behind feature tests as they
become portable.**

## The C++20/23 workhorses

These are what liquid will actually be built from, and they are mature:

- **Concepts** — constrain templates, get comprehensible errors.
- **Ranges and views** — but watch compile times.
- **Coroutines** — the right model for liquid's I/O-heavy, highly concurrent work
  (chunk fetch, RPC, action scheduling). Note they are a low-level facility; a
  scheduler/executor layer is required, which is what `stdexec` provides.
- **`std::expected`** — the right error model for a tool with many recoverable
  failure modes. Preferable to exceptions across liquid's core boundaries.
- **`std::format` / `std::print`** — finally.
- **`std::span`, `std::mdspan`, `std::flat_map`** — good, unremarkable.
- **`std::filesystem`** — usable, with known pitfalls around symlinks, permissions
  and Windows path semantics. Given liquid's virtual filesystem ambitions
  ([16](../16-monorepo-scale-and-authoring-loop/)), much of this will be a custom
  layer anyway.

**Modules are the interesting question.** `import std`, module partitions, CMake
support, and the P1689 dependency-scanning format with `clang-scan-deps` are the
pieces. Modules would materially help liquid's own build times and are directly
relevant to liquid's incremental-compilation story
([08](../08-incremental-compilation/)) — liquid must *understand* modules to build
other people's C++ well. But module builds remain the least mature part of the
toolchain, and betting liquid's own bootstrap on them is a risk.

## Library choices

The pieces liquid needs, and the defensible 2026 choices:

| Need | Choice | Rationale |
|---|---|---|
| **Hashing** | **BLAKE3** primary; hardware-accelerated SHA-256 where interop demands it | BLAKE3 is fast and parallel — critical when hashing terabytes of chunks. SHA-256 for OCI/REAPI compatibility ([11](../11-container-images/), [07](../07-distributed-execution/)). Keep the algorithm agile ([13](../13-supply-chain-security/)). |
| **Compression** | **zstd** primary, lz4 for hot paths | zstd gives `--patch-from`, `--rsyncable` and long-range matching — all three used elsewhere in liquid ([02](../02-large-files-and-binary-assets/), [03](../03-structural-diff-and-formats/)). |
| **Chunking** | **FastCDC** | 3–12× faster than Rabin at equal or better dedup; ~2,000–2,700 MB/s. |
| **Async I/O** | io_uring (Linux), kqueue (BSD/macOS), IOCP (Windows), behind one abstraction | Do not expose the backend. `stdexec` or a hand-rolled scheduler over coroutines. |
| **Thread pool / work stealing** | Taskflow or Intel TBB | The action scheduler is the hot path. |
| **Serialisation / RPC** | Protobuf + gRPC where REAPI/OCI compatibility requires it; Cap'n Proto or FlatBuffers for liquid's internal formats | REAPI *is* protobuf, so that dependency is unavoidable. Zero-copy formats for the on-disk index. |
| **Embedded KV store** | RocksDB or LMDB | The chunk index will hold billions of entries ([12](../12-artifact-distribution/)). LMDB for read-heavy mmap simplicity; RocksDB for write-heavy LSM. Measure before committing. |
| **TLS** | OpenSSL 3.x or BoringSSL | |
| **Signing** | libsodium / Ed25519, with an eye on ML-DSA | Post-quantum migration is coming; keep the signature scheme pluggable. |
| **Parsing source** | tree-sitter | Drives structural diff ([03](../03-structural-diff-and-formats/)), symbol extraction ([19](../19-full-text-search/)) and incremental analysis ([08](../08-incremental-compilation/)). |

## Build system and toolchain

liquid must build itself, and this is not a neutral choice — it is a statement
about the project's thesis.

- **CMake + Ninja** is the pragmatic answer: best modules support, best IDE and
  platform coverage, what contributors expect.
- **Meson** is cleaner but has less C++ modules maturity.
- **Bazel/Buck2** would align philosophically but add enormous adoption friction for
  contributors.

The obvious trajectory is **CMake + Ninja to bootstrap, then self-host on liquid as
soon as it can build C++ well.** liquid building liquid is the most convincing
possible demonstration, and it forces the team to feel every rough edge first.

For dependencies, **vcpkg or Conan** — but note the irony: liquid exists partly
because dependency management is broken, so vendoring critical dependencies and
building them through liquid itself should be the medium-term goal.

## Risks

- **Compile times.** Ranges, heavy templates and header-only libraries will make
  liquid's own build slow. Dogfooding fixes this, but only after liquid works.
- **ABI stability.** liquid will expose a C ABI or a stable protocol boundary for
  plugins (format codecs, language frontends) rather than a C++ ABI. Decide this
  early; retrofitting is painful.
- **Compiler bugs at the C++26 frontier.** Experimental features have experimental
  bugs. Confine them to non-critical paths.
- **Three-platform support is a constant tax.** Linux, macOS and Windows differ in
  filesystem semantics, process control, sandboxing and file watching — all areas
  liquid depends on heavily.

## Implications for liquid

1. **Target `-std=c++26`, write C++20/23.** Feature-test every C++26 facility
   individually; keep fallbacks. Nothing experimental in a public interface.

2. **Abstract every unstable dependency.** `stdexec`, SIMD, reflection substitutes
   and reclamation schemes all sit behind liquid's own interfaces so they can be
   swapped as the standard library catches up.

3. **`std::expected` at every core boundary; exceptions only at the outer edge.**
   A build tool has many expected failures; they should be values, not control flow.

4. **Coroutines plus a real scheduler for concurrency.** The workload — thousands of
   concurrent chunk fetches, RPCs and actions — is exactly what this model is for.

5. **BLAKE3 and FastCDC are the two performance-critical primitives.** liquid hashes
   and chunks everything; these are on every hot path in the system.

6. **Design the plugin ABI now.** Format codecs, language frontends and package
   backends are all extension points ([03](../03-structural-diff-and-formats/),
   [10](../10-os-packaging/)). A stable non-C++ boundary avoids an ABI trap later.

7. **Self-host as early as possible.** CMake + Ninja to bootstrap; then build liquid
   with liquid. It is the proof, the test suite and the forcing function all at
   once.
