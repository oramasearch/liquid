# Implementation

> Research: [18-implementation-cpp26](../research/18-implementation-cpp26/)

liquid is written in **modern C++26**, cross-platform from the start: Linux, macOS,
Windows, and BSD where the dependencies allow.

## Committed foundations

Two components are decided and are not open questions.

### Datastore — [`homer6/automerge-cpp`](https://github.com/homer6/automerge-cpp)

A from-scratch C++23 implementation of **Automerge**, a CRDT library — not a wrapper
around the Rust original. CI covers Linux, macOS, Windows and FreeBSD, which matches
liquid's portability commitment.

What it provides that liquid needs directly:

| Capability | Why liquid needs it |
|---|---|
| **Conflict-free merge** (RGA for lists/text) | Concurrent edits from different machines merge deterministically without a coordinator. |
| **Strong identity types** — `ActorId`, `ObjId`, `ChangeHash`, `OpId` | Change identity ([02](02-version-control.md)) and stable object identity across moves — the thing whose absence forced Dropbox's sync-engine rewrite. |
| **Sync protocol** (bloom-filter based, peer-to-peer) | liquid's replication protocol, for both developer push/pull and consumer sync ([15](15-consumer-sync-and-web.md)). |
| **Time travel** — `get_at()`, `text_at()` | History and version browsing as a query rather than a separate mechanism. |
| **Cursors** — stable positions surviving edits and merges | Tracking a location through history; anchoring annotations. |
| **Patches** — `transact_with_patches()` | Incremental change notification, which drives incremental indexing ([12](12-search.md)) and reactive rebuilds ([04](04-build-engine.md)). |
| **Columnar binary encoding** (RLE, delta, boolean; DEFLATE over 256 bytes), upstream-compatible | Compact on-disk history and interoperability with the wider Automerge ecosystem. |
| **Marks** — range annotations anchored by identity | Review comments, provenance annotations, ownership regions. |

**Where it is used, and where it is not.** This is the important architectural
boundary:

- **Automerge holds the metadata layer** — the namespace, trees, paths, names,
  refs, branches, change history, permissions, and small structured data. It is the
  concurrently-edited document.
- **The chunk store holds the bytes** ([01](01-object-model-and-storage.md)).
  Automerge documents reference content by hash; they never contain it.

This is exactly Dropbox's metadata-service / block-service split
([research](../research/21-consumer-sync-and-web/)), and Seafile's commit /
FS-object / block split. It is a well-precedented design, and it is what keeps a
CRDT viable: Automerge is excellent for a structured document that many people edit
and terrible as a container for terabytes of binary data.

**Known risks, stated honestly:**

- **History growth.** CRDT documents accumulate operation history. A repository with
  millions of paths and years of changes needs a compaction and archival story that
  does not exist yet. This is the single biggest open risk in the datastore choice.
- **Automatic merge is not always semantically correct for code.** A CRDT will merge
  two concurrent edits without complaint and can interleave lines into something
  syntactically valid and semantically wrong. liquid therefore keeps structural merge
  ([03](03-diff-and-merge.md)) on top, and must be able to *surface* a change for
  human review even when the CRDT resolved it automatically. "No conflict" must not
  silently mean "no attention required."
- **Text CRDT interleaving anomalies** are a known property of RGA under concurrent
  insertion at the same position.

### Search — [`oramasearch/oramacore-cpp`](https://github.com/oramasearch/oramacore-cpp)

The C++23 (→ C++26) port of OramaCore's engine, structured as a library
(`orama::oramacore-cpp`), coroutine-native via **Boost.Capy**, using
`Result<T> = std::expected<T, Error>` with no exceptions across the public API.

**First index type is BM25F full-text**, described upstream as covering roughly 90%
of real queries. Other index types — prefix/FST, geo, filters, vector — follow the
same pattern.

**Current status:** the BM25F scoring core, tokenizer, and in-memory postings are implemented and
tested upstream. Liquid #22 pins that revision and persists its own content-addressed term records,
using the upstream tokenizer and scorer without exposing upstream types. Coroutine-shaped
repository search is implemented at liquid's boundary; the oramacore repository remains private.

**What it covers, and the gap it does not.** BM25F is *ranked full-text over
tokens*. That is the right engine for prose, documentation, and the Drive-style
consumer content ([15](15-consumer-sync-and-web.md)). It is **not** a substring or
regex engine, and token-based analysis mishandles exactly what matters in source
code: punctuation, partial identifiers, `snake_case`/`camelCase` boundaries and
operators ([research](../research/19-full-text-search/)).

So liquid's search is **two engines behind one query planner**:

1. **oramacore-cpp / BM25F** — ranked full-text and relevance.
2. **A positional trigram index** — exact substring and regex over code, in the
   Zoekt/Blackbird tradition.

Plus brute-force scan for corpora changing faster than any index can follow. See
[12](12-search.md).

The conventions oramacore-cpp already uses — `std::expected` at the boundary, no
exceptions across the API, coroutines, CMake + Ninja, Catch2 — are the conventions
liquid adopts throughout, so integration is idiomatic rather than adapted.

## Language target

**Target `-std=c++26`; write C++20/23.** Very little of C++26 is portable in 2026:

- **No standard-library `std::execution`** (P2300) anywhere — not libstdc++, not
  libc++, not MSVC STL. liquid uses **Boost.Capy** (coroutines) and **Corosio**
  (networking), as Zetta and oramacore-cpp already do — one async stack across the org.
  stdexec is not used.
- **Static reflection** (P2996) is GCC 16 experimental only; no upstream Clang or
  MSVC.
- **Contracts** (P2900) are GCC 16 experimental — not a security or validation
  mechanism.
- **`std::hive`, hazard pointers, RCU, `std::linalg`** are unavailable everywhere.
- **`#embed`** works on GCC 15+ and Clang 19+ but not MSVC.

Feature-test every C++26 facility individually and keep fallbacks. Nothing
experimental appears in a public interface.

The workhorses are C++20/23: concepts, ranges, coroutines, `std::expected`,
`std::format`/`print`, `std::span`, `std::mdspan`, `std::flat_map`.

**Modules** would materially help liquid's own build times and are directly relevant
to building other people's C++ well ([04](04-build-engine.md)) — but module builds
are the least mature part of the 2026 toolchain, so liquid's bootstrap does not
depend on them.

## Component inventory

| Need | Choice |
|---|---|
| **Datastore / metadata CRDT** | **automerge-cpp** (committed) — the same code Zetta already vendors as a submodule |
| **Ranked full-text search** | **oramacore-cpp** / BM25F (committed) |
| **Code substring/regex search** | Positional trigram index (to build) |
| **Hashing** | BLAKE3 primary; hardware SHA-256 for OCI/REAPI interop; algorithm agile |
| **Compression** | zstd (`--patch-from`, `--rsyncable`, long-range), lz4 for hot paths |
| **Content-defined chunking** | FastCDC |
| **Async I/O** | io_uring / kqueue / IOCP behind one abstraction |
| **Coroutines and networking** | **Boost.Capy** for coroutines, **Corosio** for networking — decided. The same stack Zetta and oramacore-cpp use; Zetta vendors both. Requires **Clang 17+**. |
| **Thread pool** | Taskflow or TBB |
| **RPC / serialisation** | Protobuf + gRPC where REAPI and OCI demand it; Cap'n Proto or FlatBuffers internally |
| **Chunk index** | **liquid's own**, built from scratch (a fixed-width LSM, rebuildable from the packs); the RocksDB/LMDB benchmark (`../library/chunk-index-benchmark.md`) is the bar |
| **TLS** | OpenSSL 3.x or BoringSSL |
| **Signing** | libsodium / Ed25519, pluggable for post-quantum ML-DSA |
| **Source parsing** | tree-sitter |
| **Build (bootstrap)** | CMake ≥ 3.28 + Ninja, FetchContent, Catch2 — matching oramacore-cpp |

## Conventions

- `std::expected` at every core boundary; **no exceptions across public APIs**.
- Coroutines plus a real scheduler for concurrency.
- A **stable non-C++ plugin ABI** for format codecs
  ([03](03-diff-and-merge.md)), language frontends and package backends
  ([08](08-packaging-and-images.md)) — decided now, because retrofitting it is
  painful.
- Deterministic, seeded simulation for the sync and reconciliation state machines,
  following Nucleus's example. For a system holding other people's files, this
  matters more than throughput.

## Build order

1. **CMake + Ninja to bootstrap.**
2. **Self-host on liquid** as soon as it can build C++ well. liquid building liquid
   is the proof, the test suite and the forcing function at once.

## Open questions

- **CRDT history compaction.** The largest unresolved risk. What does archival of old
  operation history look like, and what is lost?
- ~~RocksDB vs LMDB~~ **Resolved: neither** (Steve, 2026-09-03): liquid owns its index.
- ~~Boost.Capy vs stdexec~~ **Resolved:** Capy for coroutines, Corosio for networking,
  matching Zetta and oramacore-cpp.
- **Where the Automerge/chunk-store boundary sits exactly** for very large trees:
  at what point does a directory become its own document rather than a subtree of
  its parent's?
