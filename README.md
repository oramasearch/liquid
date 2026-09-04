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

> **Status: the Phase 1 store exit criterion is met; the chunk store, bounded-memory file streaming, garbage collection with pack compaction, deterministic crash simulator, first working repository, local Git import/export, trust-domain data-key rotation with revision readability, TAAP/1 identities with rotation plus signed drips and changes, local change capture, inline secret scanning, verified redaction, and structured JSON/`.env` encryption are implemented.** Code is a C++26 **libliquid**
> (`liquid::liquid`, static by default, shared with `BUILD_SHARED_LIBS`) plus the `liquid` binary
> as a thin client of it; liquify and lake link the same library. The public error, digest, identity
> document codec, and signature decoder APIs are implemented, including BLAKE3, SHA-256, and keyed
> BLAKE3 identifiers. The public waiting operations are lazy
> Boost.Capy tasks, backed by a private Corosio runtime. The local store implements open, put,
> bounded-memory `put_file` and `get_to_file`, get, range read, contains, trust-domain encrypted objects with passphrase or raw-key-file custody,
> versioned data-key rotation and revocation, repository readability reports,
> leased roots, epoch garbage collection, and pack compaction. Multiple repositories can use one
> caller-owned store with repository-scoped checkpoints, journals, and retention leases. Capture subscriptions, default-blocking
> secret scanning, the TAAP attestation profile registry with `scan_result/1`, `reproducibility/1`,
> structured `slsa_provenance/1`, signed `sbom/1`, signed `delta/1`, and repository redaction are implemented. Repository
> push and pull, signed metadata exchange, missing-object negotiation, the public sync server, and
> the HTTP/1.1 client and `serve sync` adapter are implemented. VFS full materialisation works on every platform;
> lazy mounts use libfuse3 on Linux and optional macFUSE on macOS. Repository creation, working-tree reads
> and writes, recorded moves
> and removals, change cuts, references, snapshots, composition, conflict values, undo, advisory
> locks, status, public tree/change diff, paged result, attestation, trace, and SBOM enumeration,
> and one-drip streaming file writes are implemented. TAAP/1 identities with rotation and signed drips and changes, the internal
> CRDT metadata document, canonical CBOR codec, and drip journal are implemented behind libliquid.
> Directory CRDT checkpoints are compact and shallow-loaded, remote changes are observed through
> patches, and every automerge apply, merge, and sync-receive failure is checked.
> The `liquid` binary is the documented thin client of the Phase 1 repository, capture, VFS, store,
> and identity APIs plus the Phase 2 build wrapper. `liquid gc --explain` reports why retained chunks are live. Repository path
> rules encrypt selected JSON and `.env` leaf values while leaving their structure visible, with
> explicit plaintext reads and key-only historical diff. Whole-file encryption remains the fallback,
> and `find_package(liquid)` works from an install. `liquid import git` preserves reachable local Git
> history and its identity mapping; `liquid export git` writes a standalone Git repository and reports
> compatibility losses.

> **Phase 4 now includes the persisted demand-driven query graph.** Registered coroutine query
> kinds use canonical keys and values, record input and nested-query dependencies in read order,
> validate red-green on demand, cut off unchanged fingerprints, propagate durability, cancel
> cooperatively, and expose recorded inputs and values for clean comparison. Direct build execution
> and every wrapped tool action are `action_result` graph nodes.

> **Phase 2 now includes `liquid wrap`, local sandboxed action execution, the persistent disk cache,
> sampled reproducibility verification, signed SLSA provenance, record-derived cache diagnostics,
> dependency-aware build observability, build-input policy, a cross-project configure fact base,
> cost-aware retention, and graph-derived SPDX and CycloneDX SBOMs for every output.**
> `liquid::build` stores action and
> result records, materialises a toolchain, observes reads through the VFS mount, and executes the
> action in Linux namespaces or a macOS sandbox with a fixed environment. `liquid wrap` runs an
> unchanged CMake, Autotools, Make, or compilation-database project in one observed mount and turns
> output-producing tool processes into cached actions. Before execution it looks up signed results
> by candidate key, validates every observed manifest path against the current input tree, and
> reports the first differing input on a miss. The on-disk action index rebuilds from its leased
> store root. `liquid verify` re-executes cached actions in fresh sandboxes, compares exit status
> and every output digest, signs a `reproducibility/1` attestation, and permanently quarantines a
> divergent result. `wrap` schedules a one-in-20 local sample on the shared runtime by default.
> `liquid provenance` verifies the result-bound TAAP attestation and `--dsse` projects it as an
> in-toto Statement. `liquid sbom` verifies and prints SPDX 2.3 or CycloneDX 1.5; foreign binaries
> use distinctly marked local composition analysis. `liquid why` explains output, action, and result
> misses with digest, argv, environment,
> toolchain, sandbox, line-range, and drip details; `wrap --explain` and `cache stats` report disk and
> remote hit rates and output bytes avoided versus rebuilt. The memory cache tier remains separate work;
> macOS needs macFUSE and Windows returns its documented unsupported result.
> Each result records non-overlapping queued, sandbox, exec, hash, and store phases. Every wrap
> retains a dependency trace, prints wall and critical-path time plus achieved parallelism and phase
> totals, and supports `liquid build trace --chrome` and `liquid why-slow <target>`. Issue #18 adds
> a transport-independent remote executor alongside #17's remote blob interface, ships source and
> toolchain trees from the store, and chooses local, remote, or racing execution from recorded timing, absent bytes,
> queue depth, and cache locality. Four or more short actions share one supported remote batch; a
> race cancels its loser and publishes only its winner. Results, `liquid why`, retained traces, and
> Chrome events preserve the decision and every model input. The dependency-free protobuf codec,
> REAPI v2.3 messages, and native tree/action/result conversions are implemented. The Corosio
> REAPI client provides CAS, ByteStream, ActionCache, and Execution RPCs; `wrap` and `build --profile`
> use its validated remote cache tier, and `liquid serve reapi` exposes liquid's CAS and ActionCache.
> Production workers and measured transfer timing remain Phase 3 work.
> Repository `config.policy` rules can require registered-profile attestations from approved builders
> for source and toolchain inputs before execution; `liquid policy check` evaluates a cached target
> without building, and the policy digest isolates cache candidates.
> Autotools cache variables and common CMake check results are facts keyed by compiler content,
> explicit sysroot content, and target triple; `liquid facts` inspects or exports them. `wrap --target`
> refuses to execute an uncached probe for a foreign target.
> Typed retention rules keep matching refs and cost-weighted results through computed reachability;
> a physical size cap evicts the lowest execution-cost-per-byte results, and `liquid retain explain`
> names the responsible rule and lease. The memory cache tier remains separate work; macOS needs
> macFUSE and Windows returns its documented unsupported result.

> **Phase 3 build-without-bytes is implemented behind a transport-neutral remote blob source.**
> Remote result records retain source and instance locations without fetching outputs. Store reads,
> VFS hydration, and materialisation fetch verified logical chunks only on demand, while `liquid
> status` reports current-tree bytes that remain remote. Actions carry an opt-in remote-eligibility
> flag, default off; on Linux, eligible actions refuse and name reads outside their input and
> toolchain trees before dispatch. Cost-based scheduling, the REAPI record codec, the Corosio client,
> and the validated read-only remote cache tier are implemented. NativeLink and BuildBarn match the
> local 100% second-run fixture hit rate, and stock Bazel reads and writes `liquid serve reapi`.

> **Phase 3 now includes explicit local replay.** `liquid replay <action|result>` reconstructs a
> recorded action from its input tree, toolchain, environment, and sandbox, then compares exit
> status, stdout, stderr, and declared outputs byte for byte. Matching recorded failures are
> successful reproductions, and differences report the first unequal byte. Replay does not publish
> its local result or change quarantine state unless `--record` requests an ordinary verification
> sample. Registered remote inputs and expected outputs hydrate through their recorded
> `remote_blob_location` and the repository's configured `remote_blob_source`; tests cover both its
> on-disk double and the REAPI client.

> **Phase 3 delta delivery is implemented.** `liquid delta create` stores a zstd reference patch,
> content-addressed delta record, and signed `delta/1` attestation. Apply and fetch verify source,
> patch-blob, and final target digests. Fetch finds the local signed target catalog and uses the
> store's registered remote blobs when the patch or target remains remote, otherwise falling back
> to content-defined chunks. The executable-aware algorithm id is reserved because its measured
> Mach-O candidate was larger than the generic patch.

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
- [`docs/library/http2.md`](docs/library/http2.md) — private Corosio HTTP/2 and gRPC framing, h2c and TLS ALPN interoperability.
- [`docs/library/sync.md`](docs/library/sync.md) — signed repository metadata and pack-framed object exchange over HTTP/1.1.
- [`docs/library/chunk-index-benchmark.md`](docs/library/chunk-index-benchmark.md) — reproducible RocksDB/LMDB chunk-index measurements.
- [`docs/library/store.md`](docs/library/store.md) — local store layout, packs, integrity, index, tuning, and rebuild.
- [`docs/library/gc.md`](docs/library/gc.md) — leased roots, retention rules, cost-aware result eviction, epoch marking, reference extraction, and crash-safe sweep ordering.
- [`docs/library/encryption.md`](docs/library/encryption.md) — trust-domain encrypted store objects, key custody, and failure behavior.
- [`docs/library/leaf-encryption.md`](docs/library/leaf-encryption.md) — JSON and `.env` leaf encryption, path rules, explicit decryption, and structural key diff.
- [`docs/library/simulation.md`](docs/library/simulation.md) — seeded crash recovery simulation, fault points, invariants, and replay.
- [`docs/library/identity.md`](docs/library/identity.md) — TAAP/1 identities, local custody, signatures, and verification.
- [`docs/library/vfs.md`](docs/library/vfs.md) — lazy chunk hydration, FUSE mounts, full materialisation, and platform support.
- [`docs/library/git-interop.md`](docs/library/git-interop.md) — local Git import/export, identity mapping, and compatibility reporting.
- [`docs/library/capture.md`](docs/library/capture.md) — watcher-backed local change capture, cursor delivery, coalescing, recovery, and telemetry.
- [`docs/library/secret-scanning.md`](docs/library/secret-scanning.md) — inline detectors, repository exceptions, signed scan attestations, verified redaction, and measured cost.
- [`docs/library/attestations.md`](docs/library/attestations.md) — registered TAAP schemas, profile-keyed verification, freshness, and selective disclosure.
- [`docs/library/build-engine.md`](docs/library/build-engine.md) — action execution, two-stage disk-cache lookup, signed results, index bytes, and collector coupling.
- [`docs/library/distribution.md`](docs/library/distribution.md) — remote blob records, lazy output hydration, REAPI interoperability, explicit local replay, attested delta delivery, and the remaining Phase 3 design.
- [`docs/library/reapi-codec.md`](docs/library/reapi-codec.md) — the dependency-free protobuf codec, REAPI v2.3 message structs, native record conversion, digest selection, and pinned fixtures.
- [`docs/library/reapi.md`](docs/library/reapi.md) — the REAPI client, remote cache tier, CAS/ActionCache server, configuration, CLI, API map, and peer proof.
- [`docs/library/provenance.md`](docs/library/provenance.md) — automatic signed SLSA provenance, its exact record mapping, verification, and DSSE export.
- [`docs/library/policy.md`](docs/library/policy.md) — build-input attestation rules, approved-builder quorums, failure behavior, CLI inspection, and policy-bound cache keys.
- [`docs/library/sbom.md`](docs/library/sbom.md) — graph-derived SPDX/CycloneDX SBOMs, signed records, binary inference, and verification.

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

The first shippable build product wraps an existing project without adding liquid configuration:

```sh
liquid wrap cmake -S . -B build -G Ninja
liquid wrap --jobs 8 --explain cmake --build build
liquid wrap --target aarch64-unknown-linux-gnu ./configure
```

The second command reuses valid compile and link results before their tools start. Linux requires
libfuse3; macOS requires macFUSE. See [`docs/cli/README.md`](docs/cli/README.md) for options and
platform availability.

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
cmake --install build --prefix /opt/liquid        # bin/liquid, lib/libliquid.a, libexec/liquid/, include/liquid/, lib/cmake/liquid/
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

Submodule consumers may define `Boost::capy` and `Boost::corosio` first; liquid reuses those targets
instead of adding its vendored checkouts again. The exact tested revisions and minimum-pin rule are
documented in [`docs/library/libliquid.md`](docs/library/libliquid.md). The CTest suite builds a
separate lake-shaped consumer project with Capy, Corosio, and nlohmann/json already present.

## Implementation

Modern **C++26**, cross-platform.

- **Datastore** — [`oramasearch/automerge-cpp`](https://github.com/oramasearch/automerge-cpp),
  a from-scratch C++23 CRDT implementation, for the metadata and namespace layer.
- **Search** — [`oramasearch/oramacore-cpp`](https://github.com/oramasearch/oramacore-cpp)
  (BM25F) for ranked full-text, plus a positional trigram index for code substring
  and regex search.

See **[docs/features/17-implementation.md](docs/features/17-implementation.md)**.
