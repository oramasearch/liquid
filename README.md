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

> **Status: the Phase 1 store exit criterion is met; the chunk store, bounded-memory file streaming, garbage collection with pack compaction, deterministic crash simulator, first working repository, local Git import/export, trust-domain data-key rotation with revision readability, TAAP/1 identities with rotation plus signed drips and changes, local change capture, inline secret scanning, verified redaction, structured JSON/`.env` encryption, Lake sealed-secret envelopes, and the macOS/Linux checkout daemon are implemented.** Code is a C++26 **libliquid**
> **Status: the Phase 1 store exit criterion is met; the chunk store, bounded-memory file streaming, garbage collection with pack compaction, deterministic crash simulator, first working repository, local Git import/export, pluggable KEK custody with trust-domain data-key rotation and revision readability, TAAP/1 identities with rotation plus signed drips and changes, local change capture, inline secret scanning, verified redaction, structured JSON/`.env` encryption, and the macOS/Linux checkout daemon are implemented.** Code is a C++26 **libliquid**
> **Status: the Phase 1 store exit criterion is met; the chunk store, bounded-memory file streaming, garbage collection with pack compaction, deterministic crash simulator, first working repository, local Git import/export, trust-domain data-key rotation with revision readability, TAAP/1 identities with rotation plus signed drips and changes, local change capture, agent write flow control, inline secret scanning, verified redaction, structured JSON/`.env` encryption, and the macOS/Linux checkout daemon are implemented.** Code is a C++26 **libliquid**
> (`liquid::liquid`, static by default, shared with `BUILD_SHARED_LIBS`) plus the `liquid` binary
> as a thin client of it; liquify and lake link the same library. The public error, digest, identity
> document codec, and signature decoder APIs are implemented, including BLAKE3, SHA-256, and keyed
> BLAKE3 identifiers. Store owners can await a whole-content write-through callback before the
> first local pack write and evict remote-registered local copies without losing their remote
> location. Repository-local BM25F, substring, and regex search over the working tree or
> named refs is implemented with measured cost planning, current mapped scans, stable explanations,
> and atomic per-drip publication. The public waiting operations are lazy
> Boost.Capy tasks, backed by a private Corosio runtime. The local store implements open, put,
> bounded-memory `put_file` and `get_to_file`, get, range read, contains, trust-domain encrypted objects with passphrase, raw-key-file, age/X25519, cloud KMS, lake transit, or Shamir custody,
> versioned data-key rotation and revocation, repository readability reports,
> leased roots, epoch garbage collection, and pack compaction. Multiple repositories can use one
> caller-owned store with repository-scoped checkpoints, journals, and retention leases. Capture subscriptions, default-blocking
> secret scanning, the TAAP attestation profile registry with `scan_result/1`, `reproducibility/1`,
> structured `slsa_provenance/1`, signed `sbom/1`, signed `delta/1`, `image_provenance/1`, and repository redaction are implemented. Repository
> push and pull, signed metadata exchange, missing-object negotiation, the public sync server with
> change and graph-derived per-reference authorization, mixed and atomic push outcomes, and the
> HTTP/1.1 client and `serve sync` adapter are implemented. VFS full materialisation works on every platform;
> `liquid serve admission` provides the fail-closed Kubernetes webhook, TLS and control listeners,
> policy reload, local or node-store evidence lookup, metrics, REST, OpenAPI, and MCP. Complete
> transparency inclusion verification remains #83 and therefore log mode currently refuses.
> HTTP/1.1 client and `serve sync` adapter are implemented. Issue #195 adds the persisted
> three-tree drive client, named concurrent conflict values, interrupted-transfer resume,
> per-reference refusal reporting, stable-node path history and restore, `liquid drive`, and the
> daemon drive target. VFS full materialisation works on every platform;
> lazy mounts use libfuse3 on Linux and optional macFUSE on macOS. Repository creation, working-tree reads
> and writes, recorded moves
> and removals, change cuts, references, snapshots, historical document state, composition,
> conflict values, operation- and session-scoped local-intent undo, advisory
> locks, status, public tree/change diff, edit-surviving text anchors, paged result, attestation, trace, and SBOM enumeration,
> one-drip streaming file writes and tree imports, and caller-stated write bases with
> stale-reasoning refusal, disjoint re-validation, and forced re-planning records are implemented.
> TAAP/1 identities with rotation and signed drips and changes, the internal
> CRDT metadata document, canonical CBOR codec, and drip journal are implemented behind libliquid.
> Directory CRDT checkpoints prefer compact bytes with checked full-history fallback and reopen as
> sync-capable documents; remote changes are observed through patches, and every automerge apply,
> merge, and sync-receive failure is checked.
> The `liquid` binary is the documented thin client of the Phase 1 repository, capture, VFS, store,
> and identity APIs plus the Phase 2 build wrapper. `liquid gc --explain` reports why retained chunks are live. Repository path
> rules encrypt selected JSON and `.env` leaf values while leaving their structure visible, with
> explicit plaintext reads and key-only historical diff. Whole-file encryption remains the fallback,
> and `find_package(liquid)` works from an install. `liquid import git` preserves reachable local Git
> history and its identity mapping; `liquid export git` writes a standalone Git repository and reports
> compatibility losses. Strict DER, CMS, and X.509 parsing plus verification-only RSA and ECDSA
> validate RFC 3161 checkpoint timestamps and RFC 4998 renewal chains against explicitly pinned
> TSA trust anchors at each token's historical generation time.

> **Phase 4 now includes the persisted demand-driven query graph, continuous clean validation, and per-definition C++ builds.** Registered coroutine query
> kinds use canonical keys and values, record input and nested-query dependencies in read order,
> validate red-green on demand, cut off unchanged fingerprints, propagate durability, cancel
> cooperatively, and expose recorded inputs and values for clean comparison. Idle one-in-twenty
> validation uses a new graph instance, persists audit records, reports the query and inputs on
> divergence, and evicts the offending value. `liquid verify --graph --all` performs a full pass.
> Direct build execution
> and every wrapped tool action are `action_result` graph nodes. A versioned C frontend ABI loads
> replaceable shared or static language modules outside libliquid. The bundled tree-sitter fallback
> fetches 49 exact-commit grammars under archive and per-file SHA-256 checks, registers their
> extensions, and discovers units from pinned tag queries where upstream supplies them. The Clang
> C++ module emits definition actions and assembles their
> outputs into a relocatable translation-unit object. A function-body edit therefore executes only
> that definition's new action; recorded header reads invalidate the definitions that use them.
> Liquid #66 adds content-addressed per-translation-unit symbol records from a query over recorded
> compile actions, a labelled tree-sitter fallback, graph retrieval, repository access, and
> `liquid symbols <path>`.
> Liquid #32 adds evidence-based hot-update classification for tiers 0–4, signed patch and restart
> records, the versioned `liquid-hot` cooperative C runtime for macOS arm64 and Linux x86_64,
> sandboxed restart hooks, reconstructible base-plus-patch status, and a drip-driven daemon target.
> Tier 2 verifies the #27 descriptors referenced by both signed results and otherwise returns the
> tier-3 restart outcome.
> `liquid symbols <path>`; syntactic records and `liquid version --json` carry the grammar-lock
> digest. The distribution decision and inventory are in
> [`docs/library/grammars.md`](docs/library/grammars.md).
> Public build handles now stream queued, started, hit, done, failed, critical-path, and finished
> events through the library and daemon. CMake test registrations become discoverable cached
> actions, with `liquid build --events` providing JSON Lines and `liquid test` reporting results.
> Configured repository builds now expose complete public action plans with dependency edges and
> validated hit state. `repository::build()` and the daemon start the whole selected build in one
> call, `repository::root_digest()` exposes the current tree record, and `liquid build --plan`
> prints the daemon plan schema as JSON Lines.
> Liquid #205 adds canonical structural-edit records and repository/daemon tasks for C and C++
> rename, extract, move, and concrete-tree edits. Previews contain minimal text changes and
> refusals; apply verifies file digests, writes one drip per file, and records one attributed
> operation for whole-edit undo. The bundled tree-sitter frontend now exposes concrete syntax, and
> `liquid edit` previews or applies through the checkout daemon.

> **Phase 2 now includes `liquid wrap`, local sandboxed action execution, the persistent disk cache,
> sampled reproducibility verification, signed SLSA provenance, record-derived cache diagnostics,
> dependency-aware build observability, build-input policy, a cross-project configure fact base,
> cost-aware retention, content-addressed upstream outputs, graph-derived SPDX and CycloneDX SBOMs
> for every output, and stored ABI descriptors plus signed compatibility comparisons for every built
> shared library.**
> `liquid::build` stores action and
> result records, materialises a toolchain, observes reads through the VFS mount, and executes the
> action in Linux namespaces or a macOS sandbox with a fixed environment. `liquid wrap` runs an
> unchanged CMake, Autotools, Make, or compilation-database project in one observed mount and turns
> output-producing tool processes into cached actions. Before execution it looks up signed results
> by candidate key, validates every observed manifest path against the current input tree, and
> reports the first differing input on a miss. The on-disk action index rebuilds from its leased
> store root. `liquid verify` re-executes cached actions in fresh sandboxes, compares exit status
> and every output digest, signs a `reproducibility/1` attestation, and permanently quarantines a
> divergent result. `wrap` schedules one-in-20 graph and action samples on the shared runtime by
> default.
> New signed results carry canonical output-tree digests. `wrap --input <name>=<result|tree>` and
> direct named inputs mount upstream bytes read-only below `deps/<name>/`; an identical normalised
> upstream tree stops the downstream cascade. Directory modes, `ar` headers, and Python bytecode
> are normalised before storage, with the rule set bound into the sandbox digest.
> `liquid provenance` verifies the result-bound TAAP attestation and `--dsse` projects it as an
> in-toto Statement. `liquid sbom` verifies and prints SPDX 2.3 or CycloneDX 1.5; foreign binaries
> use distinctly marked local composition analysis. `liquid why` explains output, action, and result
> misses with digest, argv, environment,
> toolchain, sandbox, line-range, and drip details; `wrap --explain` and `cache stats` report disk and
> remote hit rates and output bytes avoided versus rebuilt. The action-result memory cache tier remains separate work;
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
> Liquid's action-digest scheduler and TAAP-signed worker fleet are implemented, including one
> bounded requeue after worker loss, size-class placement from #18's recorded decision, REAPI
> Execution and ActionCache, and deployable health, metrics, REST, OpenAPI, and MCP surfaces.
> Measured transfer timing remains Phase 3 work.
> The per-node runtime chunk store is implemented with metadata-only image receipt,
> reference-counted read-only VFS mounts, shared-chunk reuse counters, logical byte-budget
> eviction, a local snapshotter listener, TCP health/metrics/REST/OpenAPI/MCP control, and the
> `liquid node-store` CLI. The containerd proxy snapshotter remains in k0s #14.
> Repository `config.policy` rules can require registered-profile attestations from approved builders
> for source and toolchain inputs before execution; `liquid policy check` evaluates a cached target
> without building, and the policy digest isolates cache candidates.
> Autotools cache variables and common CMake check results are facts keyed by compiler content,
> explicit sysroot content, and target triple; `liquid facts` inspects or exports them. `wrap --target`
> refuses to execute an uncached probe for a foreign target.
> Typed retention rules keep matching refs and cost-weighted results through computed reachability;
> a physical size cap evicts the lowest execution-cost-per-byte results, and `liquid retain explain`
> names the responsible rule and lease. The action-result memory cache tier remains separate work; macOS needs
> macFUSE and Windows returns its documented unsupported result.

> **Build: liquid builds liquid.** Linux CI configures FetchContent dependencies outside the
> sandbox, builds the static library and CLI under `liquid wrap` twice, requires a 100% disk hit rate
> on the second run, and compares both outputs byte for byte with the CMake bootstrap. Static and
> shared libliquid builds remain covered.

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

> **Phase 4 daemon lifecycle and fan-out are implemented on macOS and Linux.** The CLI starts one
> locked process per checkout on first daemon-backed use and attaches over framed canonical CBOR.
> The daemon owns capture, maps each drip to the changed graph inputs, processes build, search,
> collaborator, and lake hooks in order with durable per-target cursors, reports lag through
> `liquid status`, exits when idle, and persists its graph on shutdown. Resident graph values use a
> byte ceiling and least-recently-demanded eviction while keys and fingerprints remain. Windows
> returns a documented unsupported result until Corosio provides named-pipe transport. The daemon
> uses issue #20's public graph interface for persistence, invalidation, and resident-value bounds.
> It also performs #23 clean validation during idle slices and reports validation backlog and the
> last divergence. Issue #85 persists actor, directory-document, and repository token buckets;
> rejects over-budget writes before a CRDT transaction opens; applies the same author limits to
> pushes; and reports persistence lag, consumer lag, watermarks, and rejection counts.

> **Phase 4 local search and cost planning are implemented.** `liquid search`,
> `repository::search`, and `repository::plan_search` select BM25F, verified positional trigrams,
> or a parallel mapped scan. `liquid search --explain` reports measured inputs, estimates, actual
> cost, and rejected alternatives. The content-addressed index lives under `.liquid/index/`. The daemon
> publishes changed paths and tombstones after each drip with a durable independent cursor; an
> atomic manifest replacement makes the whole change visible at once. Identical blobs are indexed
> once even when several paths refer to them. Named refs use snapshot-specific index slots.

> **Phase 5 now includes the format registry and its first five codecs.** ZIP, tar, HDF5, Parquet,
> and SQLite are decoded without format-vendor libraries through a versioned C module ABI. Format
> trees drive opt-in repository and CLI container diffs, archive entries use `!/` paths in search,
> and Tier 1 and Tier 2 entries are retained as deduplicated blobs beside the verbatim container.
> Uncompressed tar has a deterministic writer, while hard or unsupported format surfaces retain
> honest tiers and opaque reasons.
> **Phase 5 dependency resolution is implemented.** Canonical
> `package/1` and fully bound `concretised_spec/1` records carry versions, variants, virtual and
> concrete dependencies, compiler and target constraints, conflicts, and patches. The public
> Spack-style parser reports exact error columns, while directory and repository universes use the
> documented `packages/` tree. Liquid's own CDCL engine chooses virtual providers, binds compilers
> as dependencies, applies target-family fallback, optimises reuse before freshness and defaults,
> and returns minimal attributed conflict cores. `liquid resolve` prints the graph, stable JSON, or
> package-selection explanations.

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
- [`docs/library/sync.md`](docs/library/sync.md) — signed repository metadata, peer flow limits, per-reference push outcomes, and pack-framed object exchange over HTTP/1.1.
- [`docs/library/transparency.md`](docs/library/transparency.md) — RFC 3161 timestamp and RFC 4998 evidence verification design and trust policy.
- [`docs/library/sync.md`](docs/library/sync.md) — signed repository metadata, per-reference push outcomes, and pack-framed object exchange over HTTP/1.1.
- [`docs/library/sealed-secrets.md`](docs/library/sealed-secrets.md) — the Lake sealed-secret record, key agreement, authenticated scope, custody boundary, and fixture.
- [`docs/library/drive.md`](docs/library/drive.md) — persisted three-tree folder synchronization, conflicts, transfer resume, path history, and daemon watching.
- [`docs/library/chunk-index-benchmark.md`](docs/library/chunk-index-benchmark.md) — reproducible RocksDB/LMDB chunk-index measurements.
- [`docs/library/store.md`](docs/library/store.md) — local store layout, packs, integrity, index, tuning, and rebuild.
- [`docs/library/gc.md`](docs/library/gc.md) — leased roots, retention rules, cost-aware result eviction, epoch marking, reference extraction, and crash-safe sweep ordering.
- [`docs/library/encryption.md`](docs/library/encryption.md) — trust-domain encrypted store objects, key custody, and failure behavior.
- [`docs/library/leaf-encryption.md`](docs/library/leaf-encryption.md) — JSON and `.env` leaf encryption, path rules, explicit decryption, and structural key diff.
- [`docs/library/simulation.md`](docs/library/simulation.md) — seeded crash recovery simulation, fault points, invariants, and replay.
- [`docs/library/identity.md`](docs/library/identity.md) — TAAP/1 identities, local custody, signatures, and verification.
- [`docs/library/vfs.md`](docs/library/vfs.md) — lazy chunk hydration, FUSE mounts, full materialisation, and platform support.
- [`docs/library/git-interop.md`](docs/library/git-interop.md) — local Git import/export, identity mapping, and compatibility reporting.
- [`docs/library/capture.md`](docs/library/capture.md) — watcher-backed local change capture, write buckets, cursor delivery, coalescing, recovery, and telemetry.
- [`docs/library/secret-scanning.md`](docs/library/secret-scanning.md) — inline detectors, repository exceptions, signed scan attestations, verified redaction, and measured cost.
- [`docs/library/attestations.md`](docs/library/attestations.md) — registered TAAP schemas, profile-keyed verification, freshness, and selective disclosure.
- [`docs/library/admission.md`](docs/library/admission.md) — image provenance, admission policy, evidence and log verification, TLS, decisions, and deployment boundaries.
- [`docs/api/admission.md`](docs/api/admission.md) — webhook, control, metrics, REST, OpenAPI, and MCP routes.
- [`docs/library/build-engine.md`](docs/library/build-engine.md) — action execution, public build streams and ordering, JSON projection, two-stage disk-cache lookup, signed results, index bytes, and collector coupling.
- [`docs/library/incremental-engine.md`](docs/library/incremental-engine.md) — the query graph design and implemented clean validation, tests as actions, daemon lifecycle and build forwarding, cursor fan-out, endpoint, status, and graph value eviction.
- [`docs/library/search.md`](docs/library/search.md) — BM25F, positional-trigram, and mapped-scan executors; archive entries; measured cost planning; atomic publication; explanations; APIs; and crossover measurements.
- [`docs/library/formats.md`](docs/library/formats.md) — the format registry, C ABI, launch codecs, resource limits, nested diff and search paths, and storage deduplication.
- [`docs/library/edit.md`](docs/library/edit.md) — structural edit records, rename/extract/move/tree semantics, daemon and CLI use, exact apply, attribution, undo, limits, and measurement.
- [`docs/library/search.md`](docs/library/search.md) — BM25F, positional-trigram, and mapped-scan executors; measured cost planning; atomic publication; explanations; APIs; and crossover measurements.
- [`docs/library/distribution.md`](docs/library/distribution.md) — remote blob records, lazy output hydration, REAPI interoperability, liquid's scheduler and workers, explicit local replay, attested delta delivery, and the remaining Phase 3 design.
- [`docs/library/workers.md`](docs/library/workers.md) — scheduler and worker protocol, bounded requeue, signed results, operations, control endpoints, and deployment shape.
- [`docs/library/reapi-codec.md`](docs/library/reapi-codec.md) — the dependency-free protobuf codec, REAPI v2.3 message structs, native record conversion, digest selection, and pinned fixtures.
- [`docs/library/reapi.md`](docs/library/reapi.md) — the REAPI client, remote cache tier, CAS/ActionCache server, configuration, CLI, API map, and peer proof.
- [`docs/library/self-host.md`](docs/library/self-host.md) — how liquid configures with CMake, builds itself under `liquid wrap`, and compares bootstrap outputs.
- [`docs/library/provenance.md`](docs/library/provenance.md) — automatic signed SLSA provenance, its exact record mapping, verification, and DSSE export.
- [`docs/library/policy.md`](docs/library/policy.md) — build-input attestation rules, approved-builder quorums, failure behavior, CLI inspection, and policy-bound cache keys.
- [`docs/cli/build-events.md`](docs/cli/build-events.md) — the stable build-event JSON-lines schema and ordering.
- [`docs/cli/test.md`](docs/cli/test.md) — test discovery, filtering, cached execution, and output.
- [`docs/library/sbom.md`](docs/library/sbom.md) — graph-derived SPDX/CycloneDX SBOMs, signed records, binary inference, and verification.
- [`docs/library/abi.md`](docs/library/abi.md) — binary ABI extraction, stored descriptors, compatibility reports, attestations, and CLI use.
- [`docs/library/resolution.md`](docs/library/resolution.md) — package and concretised records, spec parsing, the private CDCL engine boundary, explanations, and solver measurements.

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
documented in [`docs/library/libliquid.md`](docs/library/libliquid.md). The CTest suite installs the
already-built package into a test-local prefix and builds a separate `find_package` consumer without
rebuilding liquid or its dependencies.

## Implementation

Modern **C++26**, cross-platform.

- **Datastore** — [`oramasearch/automerge-cpp`](https://github.com/oramasearch/automerge-cpp),
  a from-scratch C++23 CRDT implementation, for the metadata and namespace layer.
- **Search** — [`oramasearch/oramacore-cpp`](https://github.com/oramasearch/oramacore-cpp)
  (BM25F) for ranked full-text, plus a positional trigram index for code substring
  and regex search.

See **[docs/features/17-implementation.md](docs/features/17-implementation.md)**.
