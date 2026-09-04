# liquid

**A version control system, build system, package manager, artifact service and
runtime — in one program.**

The name is the thesis: *software changes form*. Source becomes objects, objects
become libraries, libraries become packages, packages become images, images become
running processes. liquid moves a project along that chain in either direction,
computing only the minimum delta at each step, distributing the work across many
machines, with a cryptographic chain of custody throughout.

It aims to subsume the role of git, GitHub, Hugging Face, Spack, Nix, `docker build`,
CMake, Autotools, ccache/sccache, remote build execution, an artifact registry, a code
search engine, a PaaS, Dropbox, Clerk and Vault.

**[lake.live](docs/features/18-lake-service.md)** is the hosted expression of all of
it — liquid is the tool, lake is where it collects. A mix of **GitHub, Google Drive
and Railway** over one content-addressed store, with identity, secrets and PKI.

> **Status: the Phase 1 store exit criterion is met; the chunk store, bounded-memory file streaming, garbage collection with pack compaction, deterministic crash simulator, first working repository, trust-domain data-key rotation with revision readability, TAAP/1 identities with rotation plus signed drips and changes, local change capture, inline secret scanning, verified redaction, and structured JSON/`.env` encryption are implemented.** Code is a C++26 **libliquid**
> (`liquid::liquid`, static by default, shared with `BUILD_SHARED_LIBS`) plus the `liquid` binary
> as a thin client of it; liquify and lake link the same library. The public error and digest APIs
> are implemented, including BLAKE3, SHA-256, and keyed BLAKE3 identifiers. The public waiting operations are lazy
> Boost.Capy tasks, backed by a private Corosio runtime. The local store implements open, put,
> bounded-memory `put_file` and `get_to_file`, get, range read, contains, trust-domain encrypted objects with passphrase or raw-key-file custody,
> versioned data-key rotation and revocation, repository readability reports,
> leased roots, epoch garbage collection, and pack compaction. Capture subscriptions, default-blocking
> secret scanning, `scan_result/1`, and repository redaction are implemented; remote operations remain
> `unsupported` until their owning issues land. VFS full materialisation works on every platform;
> lazy mounts use libfuse3 on Linux and optional macFUSE on macOS. Repository creation, working-tree reads
> and writes, recorded moves
> and removals, change cuts, references, snapshots, composition, conflict values, undo, advisory
> locks, status, and one-drip streaming file writes are implemented. TAAP/1 identities with rotation and signed drips and changes, the internal
> CRDT metadata document, canonical CBOR codec, and drip journal are implemented behind libliquid.
> The `liquid` binary is the documented thin client of the Phase 1 repository, capture, VFS, store,
> and identity APIs. `liquid gc --explain` reports why retained chunks are live. Repository path
> rules encrypt selected JSON and `.env` leaf values while leaving their structure visible, with
> explicit plaintext reads and key-only historical diff. Whole-file encryption remains the fallback,
> and `find_package(liquid)` works from an install.

## Where this sits

liquid is an **Orama** project (`oramasearch/liquid`, private). It is one module of
[**`oramasearch/liquid-bridge`**](https://github.com/oramasearch/liquid-bridge), which
also holds [our k0s](https://github.com/oramasearch/k0s), the
[lake binary](https://github.com/oramasearch/lake) and the
[Orama operator](https://github.com/oramasearch/orama-operator) — and the
**[action plan](https://github.com/oramasearch/liquid-bridge/blob/main/docs/action-plan.md)**. It is the intended
go-to-market vehicle for **Metis**, the Enterprise Intelligence OS Orama is pivoting to
build, whose central claim is that intelligence is bounded by *data availability*.

liquid manufactures the two lower bands of that availability — Reach and Fidelity — as
a side effect of being a tool teams adopt for other reasons, while **Zetta** turns
available data into running instruments.

See **[docs/features/23-metis-and-zetta.md](docs/features/23-metis-and-zetta.md)** for
the mapping, and for the open decision about the overlap between liquid's storage layer
and Zetta's.

## Documentation

- [`docs/library/libliquid.md`](docs/library/libliquid.md) — the library: targets, public API rules, how to consume and verify it.
- [`docs/library/api-map.md`](docs/library/api-map.md) — the implemented and declared public headers, ABI rules, and dependency provenance.
- [`docs/library/hash-agility.md`](docs/library/hash-agility.md) — digest and signature algorithm tags, mixed BLAKE3/SHA-256 stores, rehashing, and present limits.
- [`docs/library/runtime.md`](docs/library/runtime.md) — coroutine tasks, executors, blocking work, and runtime safety rules.
- [`docs/library/chunk-index-benchmark.md`](docs/library/chunk-index-benchmark.md) — reproducible RocksDB/LMDB chunk-index measurements.
- [`docs/library/store.md`](docs/library/store.md) — local store layout, packs, integrity, index, tuning, and rebuild.
- [`docs/library/gc.md`](docs/library/gc.md) — leased roots, epoch marking, reference extraction, and crash-safe sweep ordering.
- [`docs/library/encryption.md`](docs/library/encryption.md) — trust-domain encrypted store objects, key custody, and failure behavior.
- [`docs/library/leaf-encryption.md`](docs/library/leaf-encryption.md) — JSON and `.env` leaf encryption, path rules, explicit decryption, and structural key diff.
- [`docs/library/simulation.md`](docs/library/simulation.md) — seeded crash recovery simulation, fault points, invariants, and replay.
- [`docs/library/identity.md`](docs/library/identity.md) — TAAP/1 identities, local custody, signatures, and verification.
- [`docs/library/vfs.md`](docs/library/vfs.md) — lazy chunk hydration, FUSE mounts, full materialisation, and platform support.
- [`docs/library/capture.md`](docs/library/capture.md) — watcher-backed local change capture, cursor delivery, coalescing, recovery, and telemetry.
- [`docs/library/secret-scanning.md`](docs/library/secret-scanning.md) — inline detectors, repository exceptions, signed scan attestations, verified redaction, and measured cost.
| | |
|---|---|
| **[docs/research/](docs/research/)** | What the world currently does, what works, what is broken, and what people actually complain about. 27 topics, each with a curated analysis and — for 24 of them — the web-grounded sources it was drawn from. |
| **[docs/features/](docs/features/)** | What liquid does. 25 feature areas, each traceable to the research. |
| **[docs/build/](docs/build/)** | Local and GitHub Actions build verification. |
| **[docs/standards/](docs/standards/)** | External standards liquid depends on — currently [TAAP/1](docs/standards/taap-1-spec-draft.md), vendored until it has its own repository. |

Start with **[docs/research/99-synthesis/](docs/research/99-synthesis/)** for the
argument in one place, then **[docs/features/](docs/features/)** for the
specifications.

## Names

**liquid** is the tool. **[lake](docs/features/18-lake-service.md)** is where it
collects — the hosted service at `lake.live`. **[liquify](docs/features/21-liquify-ide.md)**
is the IDE.

A **drip** is one captured write to disk — the atom of history, automatic and
continuous, and **signed by the party that made it**, human or agent. A **change** is a
named, reviewable aggregation of drips. Drips are the record; changes are the narrative.

## Design axioms

1. **One content-addressed chunk store.** Source, binaries, packages, images, search
   indexes and encrypted data are all chunks in one store. There is no "large file
   mode" because there is no distinction to make.
2. **One dependency graph of pure actions.** An action is a pure function from an
   input Merkle tree to outputs, addressed by digest.
3. **Dependencies are observed, never declared.** The sandbox records what an action
   actually read.
4. **Outputs are content-addressed.** A rebuild producing identical bytes stops the
   cascade. This is what kills "build the world".
5. **Nothing is materialised until it is read.**
6. **Reproducibility is enforced, not aspirational.** It is load-bearing for caching,
   substitution and provenance.
7. **Provenance is a byproduct.** Every action already records everything an
   attestation needs.
8. **Everything works offline**, degraded but correct.
9. **Every write to disk is a versioned change.** Drip, drip, drip.
10. **Every change is attributable.** [TAAP/1](docs/standards/taap-1-spec-draft.md) is
    the security model: every claim liquid makes about a user, an agent, a document, a
    build or an artifact is a signed attestation, and history is an append-only,
    tamper-evident log with a witness quorum — optionally anchored to a public chain. On
    by default.

## What it must do

- Version anything, including multi-terabyte binary assets, from day one.
- **Compose repositories** — nest, split, and merge two unrelated codebases with
  history intact. Git cannot do this; it is liquid's headline feature.
- Diff *inside* zip, HDF5, Parquet, SQLite and OOXML — not just their bytes.
- Build incrementally as you type, with a build cache and compiler cache built in.
- Resolve dependencies at Spack's level of expressiveness.
- Produce `.deb`, `.rpm`, `.apk`, OCI images and Helm charts from one build.
- Serve builds, with a transparency log and signed provenance.
- Store encrypted secrets without destroying deduplication.
- Search full text and code, including inside published artifacts.
- Deploy to a minimal Kubernetes distribution — a private, upstream-synced copy of
  [k0s](https://github.com/oramasearch/k0s) modified for drips — with a Railway-style
  experience.
- Present a Dropbox/Drive-style interface for non-developers, local and web.
- Review changes, not branches — so review survives a rebase and stacking is native.
- Host models and datasets with training provenance — the chunk store Hugging Face
  already migrated to, plus the attestation chain a model hub cannot offer.
- Give you an editor with no language server, because the editor and the build are
  the same incremental graph.
- Attribute everything — users, agents, documents, builds, artifacts — with signed
  [TAAP/1](docs/standards/taap-1-spec-draft.md) attestations, in an append-only log with
  independent witnesses, optionally anchored to a public chain for timestamping that does
  not require trusting us.
- Provide identity, a runtime secrets service, and PKI — one CA serving signing,
  mTLS, TLS and SSH, because keyless signing already *is* certificate issuance.
- Support every language, platform and operating system — with the tier of support
  stated honestly rather than implied.

## Library and binary

**libliquid is the product; the binary is a client.** Everything under `include/liquid/` is
the public API; `src/main.cpp` only parses arguments and calls it. liquify links the same
library (as a submodule, `add_subdirectory`), and lake will (across the licence boundary, as a
library, never the other way round).

| Target | What |
|---|---|
| `liquid::liquid` (`libliquid`) | the library: `libliquid.a`, or `libliquid.so`/`.dylib` with `-DBUILD_SHARED_LIBS=ON`; symbols hidden by default, `LIQUID_EXPORT` marks the API |
| `liquid-cli` → `liquid` | the command-line binary; `-DLIQUID_BUILD_CLI=OFF` for library-only consumers |
| `liquid-tests` | GoogleTest; on by default only when liquid is the top-level project |

### Build

`LIQUID_BUILD_BENCH=ON` adds the optional `bench/chunk-index/chunk-index-bench` target and
fetches its RocksDB and LMDB dependencies. It defaults to OFF, so the default configure does not
fetch or build benchmark code.

GitHub Actions builds and tests clean checkouts on Linux, macOS, and Windows; see
[`docs/build/ci.md`](docs/build/ci.md) for the toolchains and coverage.

```sh
git clone --recurse-submodules git@github.com:oramasearch/liquid.git
cd liquid
cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build
mkdir example && printf 'first\n' > example/readme.txt
(cd example && ../build/liquid init . && ../build/liquid status --json && ../build/liquid cut "import")
cmake --install build --prefix /opt/liquid        # bin/liquid, lib/libliquid.a, include/liquid/, lib/cmake/liquid/
```

Consume from CMake either way:

```cmake
# as a submodule (what liquify does)
set(LIQUID_BUILD_CLI OFF)
add_subdirectory(modules/liquid EXCLUDE_FROM_ALL)
target_link_libraries(app PRIVATE liquid::liquid)

# or from an install
find_package(liquid CONFIG REQUIRED)
target_link_libraries(app PRIVATE liquid::liquid)
```

## Implementation

Modern **C++26**, cross-platform.

- **Datastore** — [`oramasearch/automerge-cpp`](https://github.com/oramasearch/automerge-cpp),
  a from-scratch C++23 CRDT implementation, for the metadata and namespace layer.
- **Search** — [`oramasearch/oramacore-cpp`](https://github.com/oramasearch/oramacore-cpp)
  (BM25F) for ranked full-text, plus a positional trigram index for code substring
  and regex search.

See **[docs/features/17-implementation.md](docs/features/17-implementation.md)**.
