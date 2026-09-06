# liquid documentation

The documentation separates evidence, specification, external standards, and implementation
notes so that each claim has a clear role. Start with
[`research/99-synthesis/`](research/99-synthesis/) for the argument in one place, then
[`features/`](features/) for the specifications, then [`library/status.md`](library/status.md)
for what is implemented today.

| Area | What it contains |
|---|---|
| [`research/`](research/) | What the world currently does, what works, what is broken, and what people actually complain about. 27 topics, each with a curated analysis and, for 24 of them, the web-grounded sources it was drawn from. |
| [`features/`](features/) | What liquid does: the premise, the names, the design axioms, what it must do, and 25 feature areas, each traceable to the research. |
| [`standards/`](standards/) | External standards liquid depends on; currently [TAAP/1](standards/taap-1-spec-draft.md), vendored until it has its own repository. |
| `library/` | Implementation, design, and benchmark notes for libliquid contributors, indexed below. |
| [`cli/`](cli/README.md) | The command-line client, its output contracts, and focused command pages, indexed below. |
| [`reference/`](reference/) | Generated command, flag, example, availability, and exit-status reference. |
| [`guides/`](guides/) | Executable user workflows for a first repository, existing builds, and encryption. |
| [`api/`](api/README.md) | Network capability maps for liquid's server components, indexed below. |
| [`build/`](build/README.md) | Local and continuous-integration build verification, indexed below. |

## Library

### Status, shape, and public surface

| Page | What it contains |
|---|---|
| [`library/status.md`](library/status.md) | What libliquid and the `liquid` binary implement today, by phase, and the known limits in one place. |
| [`library/libliquid.md`](library/libliquid.md) | libliquid targets, public API, consumption, and verification. |
| [`library/object-model.md`](library/object-model.md) | The records in local or caller-owned shared stores, public tree/change diff, stale-reasoning guards, structural-edit operations, edit-surviving text anchors, compact/shallow CRDT checkpoints, historical document state, measured open cost, and how composition, conflicts, operation/session undo, and locks work. Design for liquid #2, #3, #41, #86, #136, #165, #166, #174, and #205. |
| [`library/locks.md`](library/locks.md) | Exclusive checkout rules and records, offline reconciliation, connected lake authority, CLI use, and daemon delivery. |
| [`library/object-model.md`](library/object-model.md) | The records in local or caller-owned shared stores, public tree/change diff, stale-reasoning guards, edit-surviving text anchors, checked compact-or-full CRDT checkpoints, historical document state, measured open cost, and how composition, conflicts, operation/session undo, and locks work. Design for liquid #2, #3, #41, #86, #136, #165, #166, and #174. |
| [`library/build-engine.md`](library/build-engine.md) | Phase 2's design plus #18's remote scheduling record, #166's paged public record enumeration, #193's public build streams, and #200's configured repository plans and one-call builds: action and result records, observation by the mount with per-platform enforcement, cache lookup, event ordering and JSON projection, retained timing and scheduling traces, content-keyed configure facts, and how `liquid wrap` splits an existing build into actions. |
| [`library/transparency.md`](library/transparency.md) | Lake checkpoint, tile, inclusion, consistency, identity-scan, RFC 3161 timestamp, and RFC 4998 evidence verification; trust bootstrap, CLI, failures, and fixture regeneration. |
| [`library/data-branching.md`](library/data-branching.md) | Per-session PostgreSQL application-data branches, lifecycle and promotion guard, template-copy baseline, local reflink fast path, and sandboxed client-tool execution. |
| [`library/object-model.md`](library/object-model.md) | The records in local or caller-owned shared stores, public tree/change diff, stale-reasoning guards, edit-surviving text anchors, compact/shallow CRDT checkpoints, historical document state, measured open cost, and how composition, conflicts, operation/session undo, and locks work. Design for liquid #2, #3, #41, #86, #136, #165, #166, and #174. |
| [`library/api-map.md`](library/api-map.md) | The implemented and declared public headers of libliquid, including flow configuration and status, the rules every one obeys, dependency provenance, what is deliberately internal, and the ABI policy. |
| [`library/runtime.md`](library/runtime.md) | The Capy task API, Corosio context, executor model, blocking-work adapter, and coroutine safety rules. |
| [`library/hash-agility.md`](library/hash-agility.md) | Digest and signature algorithm tags, mixed BLAKE3/SHA-256 stores, rehashing, and present limits. |
| [`library/http2.md`](library/http2.md) | Private Corosio HTTP/2 framing, HPACK, gRPC envelopes, and h2c/TLS ALPN interoperability proof. |

### Store, repository, and identity

| Page | What it contains |
|---|---|
| [`library/object-model.md`](library/object-model.md) | The records in local or caller-owned shared stores, public tree/change diff, stale-reasoning guards, structural-edit operations, edit-surviving text anchors, checked compact-or-full CRDT checkpoints, historical document state, measured open cost, and how composition, conflicts, operation/session undo, and locks work. Design for liquid #2, #3, #41, #86, #136, #165, #166, #174, and #205. |
| [`library/multiplayer.md`](library/multiplayer.md) | Private sessions, leases, presence, epochs, stable-node per-line attribution, daemon transport, plugin delivery, CLI output, and the 10,000-line measurement. |
| [`library/repository-import-primitives.md`](library/repository-import-primitives.md) | Importer-facing repository recording, metadata, historical-object, mode, and nesting primitives. |
| [`library/store.md`](library/store.md) | Content-defined local and caller-owned shared stores, pre-pack write-through, remote-registered local eviction, on-disk formats, integrity checks, index, tuning, and rebuild. |
| [`library/chunk-index-benchmark.md`](library/chunk-index-benchmark.md) | Reproducible RocksDB/LMDB chunk-index workload and recorded measurements. |
| [`library/gc.md`](library/gc.md) | Repository-scoped leased roots, typed retention rules, cost-aware result eviction, epoch bitmaps, reference extraction, crash-safe sweep order, and collection verification. |
| [`library/simulation.md`](library/simulation.md) | Deterministic seeded store simulation, fault injection, recovery invariants, and replay. |
| [`library/encryption.md`](library/encryption.md) | Trust-domain encrypted store objects, versioned data-key rotation and revocation, readability reports, key custody, on-disk envelopes, and failure behavior. |
| [`library/leaf-encryption.md`](library/leaf-encryption.md) | JSON and `.env` leaf encryption, repository path rules, structural key diff, and whole-file fallback. |
| [`library/sealed-secrets.md`](library/sealed-secrets.md) | The X25519 and XChaCha20-Poly1305 Lake envelope, canonical CBOR, exact scope binding, Go mapping, key custody, and interoperability fixture. |
| [`library/identity.md`](library/identity.md) | TAAP/1 identity documents, public canonical-CBOR codecs, rotation, signatures, and verification. |
| [`library/attestations.md`](library/attestations.md) | TAAP profile registry, typed schemas, freshness, status evidence, selective disclosure, and verification. |
| [`library/secret-scanning.md`](library/secret-scanning.md) | Inline secret detection, repository exceptions, signed scan attestations, redaction, and measured cost. |
| [`library/capture.md`](library/capture.md) | Change capture, write flow control, watcher recovery, coalescing, cursor delivery, and telemetry. |
| [`library/vfs.md`](library/vfs.md) | Lazy and policy-driven chunk hydration, drive xattrs, FUSE mounts, the request queue, full materialisation, and platform coverage. |
| [`library/git-interop.md`](library/git-interop.md) | Git import/export mapping, compatibility limits, and verification. |
| [`library/drive.md`](library/drive.md) | Issues #195, #235, and #258's persisted three-tree reconciliation, per-repository CRDT actors, path device states, pinning, hydration and offline policy, Linux desktop badges, conflicts, transfer resume, history, daemon watcher, and bounded scan state. |
| [`library/sync.md`](library/sync.md) | Repository metadata, CRDT, ref, and pack-entry exchange through the public server and HTTP/1.1 client, including peer buckets, retry signals, change authorization, per-ref outcomes, and atomic pushes. |
| [`library/forge-client.md`](library/forge-client.md) | Lake review records, coroutine calls, anchor advancement, polling events, error mapping, and the installable forge double. |
| [`library/transparency.md`](library/transparency.md) | Lake checkpoint, tile, inclusion, consistency, identity-scan, RFC 3161 timestamp, and RFC 4998 evidence verification; trust bootstrap and policy, CLI, failures, implementation notes, and fixture regeneration. |
| [`library/tuf.md`](library/tuf.md) | TUF root bootstrap, ordered metadata verification, rollback state, target acceptance gate, and fixture provenance. |
| [`library/admission.md`](library/admission.md) | Image provenance statements, atomically reloaded named policy sets, fail-closed evaluation, evidence sources, transparency behavior, TLS serving, structured decisions, and bounded replay. |
| [`library/deploy.md`](library/deploy.md) | Operator control-plane deployment client, digest-pinned interim images, rollback, status, and the provisional OpenAPI contract. |

### Build engine and distribution

| Page | What it contains |
|---|---|
| [`library/build-engine.md`](library/build-engine.md) | Phase 2's design plus #18's remote scheduling record, #26's content-addressed output trees and normalisation, #166's paged public record enumeration, #193's public build streams, and #200's configured repository plans and one-call builds: action and result records, named dependency inputs, observation by the mount with per-platform enforcement, cache lookup, event ordering and JSON projection, retained timing and scheduling traces, content-keyed configure facts, and how `liquid wrap` splits an existing build into actions. |
| [`library/distribution.md`](library/distribution.md) | Phase 3 design and implemented remote blob hydration, local replay, signed delta delivery, #18 scheduling, #16 REAPI interoperability, and #63's scheduler and workers. |
| [`library/workers.md`](library/workers.md) | Scheduler and worker protocol, bounded requeue, TAAP result identity, configuration, control endpoints, and deployment shape. |
| [`api/README.md`](api/README.md) | Scheduler and worker network capability maps. |
| [`library/node-store.md`](library/node-store.md) | Per-node shared chunk storage, metadata-only image mounts, listener boundaries, accounting, budgets, and eviction. |
| [`library/incremental-engine.md`](library/incremental-engine.md) | Phase 4's implemented demand-driven query graph, measured restart and continuous clean validation, daemon lifecycle, flow watermarks, in-process consumer lag, build-stream and structural-edit forwarding, hot-update and drive targets, cached tests as actions, cursor fan-out, graph value eviction, per-definition frontend granularity, local search index, and cost planner; also designs symbols and liquify on the graph. |
| [`library/frontends.md`](library/frontends.md) | The versioned frontend and liquify plugin C descriptors, module loading, tree-sitter fallback, Clang C++ definition actions, graph invalidation, assembly, and own-tree measurement. |
| [`library/edit.md`](library/edit.md) | Structural rename, extract, move, and concrete-tree edits; canonical records, preview/apply guarantees, attribution, grouped undo, CLI use, limits, and own-tree measurement. |
| [`library/grammars.md`](library/grammars.md) | The digest-pinned tree-sitter runtime and 49-grammar lock, trust decision, build controls, inventory, update procedure, and measurements. |
| [`library/search.md`](library/search.md) | Issues #22, #67, and #29's BM25F, positional-trigram, and mapped-scan executors; archive entries; cost model; atomic publication; explanations; public APIs; CLI; and crossover measurements. |
| [`library/formats.md`](library/formats.md) | Issue #29's format registry and C ABI, fidelity tiers, launch codec subsets, limits, nested path notation, diff/search/storage integration, and deduplication measurement. |
| [`library/projection.md`](library/projection.md) | Issues #28 and #78: Tier A packages, OCI and Helm artifacts, owned OSTree object encoding, bootc encapsulation, deterministic layers, attestations, and peer proof. |
| [`library/support-tiers.md`](library/support-tiers.md) | Issue #71's registry-derived language, format, and platform support matrix and strongest-frontend selection. |
| [`library/projection.md`](library/projection.md) | Issue #28's Tier A package, OCI image, and Helm projection; derived dependencies, deterministic compression, layer selection, attestations, and peer proof. |
| [`library/symbols.md`](library/symbols.md) | Content-addressed per-translation-unit symbol records, query behavior, fallback labelling, public repository access, and CLI use. |
| [`library/hot-update.md`](library/hot-update.md) | Tier 0–4 evidence rules, signed patch and restart records, the cooperative `liquid-hot` C ABI, daemon delivery, and reconstructible process composition. |
| [`library/reapi-codec.md`](library/reapi-codec.md) | Dependency-free protobuf wire encoding, the REAPI v2.3 structs, native record conversion, hash selection, and pinned reference fixtures. |
| [`library/reapi.md`](library/reapi.md) | REAPI client and server RPCs, the remote cache tier, configuration and CLI, public API map, and NativeLink, BuildBarn, and Bazel proof. |
| [`library/self-host.md`](library/self-host.md) | The liquid-on-liquid build, fixed-root CMake configuration, cache assertions, and bootstrap output comparison. |
| [`library/provenance.md`](library/provenance.md) | Automatic result provenance: canonical TAAP storage, the exact SLSA v1 mapping, verification, and in-toto/DSSE export. |
| [`library/policy.md`](library/policy.md) | Build-input policy: path and toolchain scopes, verified attestation quorums, refusal and warning behavior, and cache isolation. |
| [`library/sbom.md`](library/sbom.md) | Graph-derived SPDX and CycloneDX SBOMs, signed records, distinct foreign-binary inference, and verification. |
| [`library/abi.md`](library/abi.md) | ELF, Mach-O, and PE ABI descriptors, compatibility rules, automatic signed comparisons, public access, and CLI use. |
| [`library/relocation.md`](library/relocation.md) | Position-independent install-tree rules, build-system conventions, binary and metadata enforcement, public checking, and recorded waivers. |
| [`library/distribution.md`](library/distribution.md) | Phase 3 design and implemented remote blob hydration, local replay, signed delta delivery, #18 scheduling, #16 REAPI interoperability, and #63's scheduler and workers. |
| [`library/workers.md`](library/workers.md) | Scheduler and worker protocol, bounded requeue, TAAP result identity, configuration, control endpoints, and deployment shape. |
| [`library/node-store.md`](library/node-store.md) | Per-node shared chunk storage, metadata-only image mounts, listener boundaries, accounting, budgets, and eviction. |
| [`library/reapi-codec.md`](library/reapi-codec.md) | Dependency-free protobuf wire encoding, the REAPI v2.3 structs, native record conversion, hash selection, and pinned reference fixtures. |
| [`library/reapi.md`](library/reapi.md) | REAPI client and server RPCs, the remote cache tier, configuration and CLI, public API map, and NativeLink, BuildBarn, and Bazel proof. |
| [`library/data-branching.md`](library/data-branching.md) | Per-session PostgreSQL application-data branches, lifecycle and promotion guard, template-copy baseline, local reflink fast path, and sandboxed client-tool execution. |

### Incremental engine, frontends, and search

| Page | What it contains |
|---|---|
| [`library/incremental-engine.md`](library/incremental-engine.md) | Phase 4's implemented demand-driven query graph, measured restart and continuous clean validation, daemon lifecycle, flow watermarks, in-process consumer lag, build-stream and structural-edit forwarding, hot-update and drive targets, cached tests as actions, cursor fan-out, graph value eviction, per-definition frontend granularity, local search index, and cost planner; also designs symbols and liquify on the graph. |
| [`library/frontends.md`](library/frontends.md) | The versioned frontend and liquify plugin C descriptors, multiplayer event callbacks, module loading, tree-sitter fallback, Clang C++ definition actions, graph invalidation, assembly, and own-tree measurement. |
| [`library/grammars.md`](library/grammars.md) | The digest-pinned tree-sitter runtime and 49-grammar lock, trust decision, build controls, inventory, update procedure, and measurements. |
| [`library/symbols.md`](library/symbols.md) | Content-addressed per-translation-unit symbol records, query behavior, fallback labelling, public repository access, and CLI use. |
| [`library/edit.md`](library/edit.md) | Structural rename, extract, move, and concrete-tree edits; canonical records, preview/apply guarantees, attribution, grouped undo, CLI use, limits, and own-tree measurement. |
| [`library/hot-update.md`](library/hot-update.md) | Tier 0–4 evidence rules, signed patch and restart records, the cooperative `liquid-hot` C ABI, daemon delivery, and reconstructible process composition. |
| [`library/search.md`](library/search.md) | Issues #22, #67, and #29's BM25F, positional-trigram, and mapped-scan executors; archive entries; cost model; atomic publication; explanations; public APIs; CLI; and crossover measurements. |

### Formats, projection, resolution, and configuration

| Page | What it contains |
|---|---|
| [`library/formats.md`](library/formats.md) | Issue #29's format registry and C ABI, fidelity tiers, launch codec subsets, limits, nested path notation, diff/search/storage integration, and deduplication measurement. |
| [`library/projection.md`](library/projection.md) | Issues #28 and #77's native packages, Homebrew, PKGBUILD, OCI, Helm, AppImage, Flatpak, and Snap projection; deterministic writers, runtime pins, attestations, and peer proof. |
| [`library/formats.md`](library/formats.md) | Issues #29 and #70's format registry and C ABI, fidelity tiers, codec subsets, limits, nested path notation, diff/search/storage integration, and deduplication measurement. |
| [`library/projection.md`](library/projection.md) | Issue #28's Tier A package, OCI image, and Helm projection; derived dependencies, deterministic compression, layer selection, attestations, and peer proof. |
| [`library/resolution.md`](library/resolution.md) | Package and concretised records, spec grammar, universe layout, solver model, explanations, optimisation, and scale measurements. |
| [`library/configuration-language.md`](library/configuration-language.md) | The typed `.liquid` grammar, exact diagnostics, package/target/source lowering, repository-local includes, verified source fetch, generators, and trace origins. |
| [`library/import-spack.md`](library/import-spack.md) | Pinned Spack export, package-field mapping, import reports and losses, the 500-package universe, and the Linux build subset. |
| [`build/sandboxed-executor.md`](build/sandboxed-executor.md) | Issue #10's local executor: fixed environment, observed-mount manifest, Linux namespace enforcement, and platform limits. |
| [`library/api-map.md`](library/api-map.md) | The implemented and declared public headers of libliquid, including flow configuration and status, the rules every one obeys, dependency provenance, what is deliberately internal, and the ABI policy. |
| [`library/hash-agility.md`](library/hash-agility.md) | Digest and signature algorithm tags, mixed BLAKE3/SHA-256 stores, rehashing, and present limits. |
| [`library/runtime.md`](library/runtime.md) | The Capy task API, Corosio context, executor model, blocking-work adapter, and coroutine safety rules. |
| [`library/http2.md`](library/http2.md) | Private Corosio HTTP/2 framing, HPACK, gRPC envelopes, and h2c/TLS ALPN interoperability proof. |
| [`library/sync.md`](library/sync.md) | Repository metadata, CRDT, ref, and pack-entry exchange through the public server and HTTP/1.1 client, including peer buckets, retry signals, change authorization, per-ref outcomes, and atomic pushes. |
| [`library/deploy.md`](library/deploy.md) | Operator control-plane deployment client, digest-pinned interim images, rollback, status, and the provisional OpenAPI contract. |
| [`library/transparency.md`](library/transparency.md) | RFC 3161 timestamp and RFC 4998 evidence verification design, trust policy, and implementation notes. |
| [`library/tuf.md`](library/tuf.md) | TUF root bootstrap, ordered metadata verification, rollback state, target acceptance gate, and fixture provenance. |
| [`library/sync.md`](library/sync.md) | Repository metadata, CRDT, ref, and pack-entry exchange through the public server and HTTP/1.1 client, including change authorization, per-ref outcomes, and atomic pushes. |
| [`library/forge-client.md`](library/forge-client.md) | Lake review records, coroutine calls, anchor advancement, polling events, error mapping, and the installable forge double. |
| [`library/drive.md`](library/drive.md) | Issue #195's persisted three-tree folder reconciliation, named conflict values, resumable sync transfer, path history and restore, daemon watcher, and bounded scan state. |
| [`library/sync.md`](library/sync.md) | Repository metadata, CRDT, ref, and pack-entry exchange through the public server and HTTP/1.1 client, including change authorization, per-ref outcomes, and atomic pushes. |
| [`library/drive.md`](library/drive.md) | Issues #195 and #235's persisted three-tree reconciliation, path device states, pinning, hydration and offline policy, Linux desktop badges, conflicts, transfer resume, history, daemon watcher, and bounded scan state. |
| [`library/repository-import-primitives.md`](library/repository-import-primitives.md) | Importer-facing repository recording, metadata, historical-object, mode, and nesting primitives. |
| [`library/git-interop.md`](library/git-interop.md) | Git import/export mapping, compatibility limits, and verification. |
| [`library/chunk-index-benchmark.md`](library/chunk-index-benchmark.md) | Reproducible RocksDB/LMDB chunk-index workload and recorded measurements. |
| [`library/store.md`](library/store.md) | Content-defined local and caller-owned shared stores, pre-pack write-through, remote-registered local eviction, on-disk formats, integrity checks, index, tuning, and rebuild. |
| [`library/gc.md`](library/gc.md) | Repository-scoped leased roots, typed retention rules, cost-aware result eviction, epoch bitmaps, reference extraction, crash-safe sweep order, and collection verification. |
| [`library/encryption.md`](library/encryption.md) | Trust-domain encrypted store objects, versioned data-key rotation and revocation, readability reports, TPM sealing and other key custody, on-disk envelopes, and failure behavior. |
| [`library/sealed-secrets.md`](library/sealed-secrets.md) | The X25519 and XChaCha20-Poly1305 Lake envelope, canonical CBOR, exact scope binding, Go mapping, key custody, and interoperability fixture. |
| [`library/leaf-encryption.md`](library/leaf-encryption.md) | JSON and `.env` leaf encryption, repository path rules, structural key diff, and whole-file fallback. |
| [`library/simulation.md`](library/simulation.md) | Deterministic seeded store simulation, fault injection, recovery invariants, and replay. |
| [`library/identity.md`](library/identity.md) | TAAP/1 software, FIDO2, and PIV identity keys; canonical-CBOR codecs, rotation, signatures, and verification. |
| [`library/release-approval.md`](library/release-approval.md) | Signed release policies, distinct-signer threshold evaluation, TUF alignment, custody trust models, CLI storage, and compromise handling. |
| [`library/vfs.md`](library/vfs.md) | Lazy and policy-driven chunk hydration, drive xattrs, FUSE mounts, the request queue, full materialisation, and platform coverage. |
| [`library/capture.md`](library/capture.md) | Change capture, write flow control, watcher recovery, coalescing, cursor delivery, and telemetry. |
| [`library/secret-scanning.md`](library/secret-scanning.md) | Inline secret detection, repository exceptions, signed scan attestations, redaction, and measured cost. |
| [`library/attestations.md`](library/attestations.md) | TAAP profile registry, typed schemas, freshness, status evidence, selective disclosure, and verification. |
| [`library/admission.md`](library/admission.md) | Image provenance statements, atomically reloaded named policy sets, fail-closed evaluation, evidence sources, transparency behavior, TLS serving, structured decisions, and bounded replay. |
| [`api/admission.md`](api/admission.md) | Named Kubernetes webhook, decision stream, control, metrics, REST, OpenAPI, and MCP routes. |
| [`cli/README.md`](cli/README.md) | Command-line commands, structural edits, hot updates, named build inputs, retention explanations, output, capture, drive sync, path history, flow status and retry output, examples, and verification. |
| [`cli/README.md`](cli/README.md) | Command-line commands, retention explanations, output, capture, examples, and verification. |
| [`cli/README.md`](cli/README.md) | Command-line commands, retention explanations, output, capture, drive sync and pin state, path history, examples, and verification. |
| [`cli/README.md`](cli/README.md) | Command-line commands, named build inputs, retention explanations, output, capture, examples, and verification. |
| [`cli/README.md`](cli/README.md) | Command-line commands, flow status and retry output, retention explanations, capture, examples, and verification. |

## Command line

| Page | What it contains |
|---|---|
| [`cli/README.md`](cli/README.md) | Command-line commands, repository cloning, structural edits, hot updates, named build inputs, retention explanations, output, capture, drive sync and pin state, path history, flow status and retry output, examples, and verification. |
| [`cli/README.md`](cli/README.md) | Command-line commands, structural edits, hot updates, named build inputs, retention explanations, output, capture, drive sync and pin state, path history and blame, flow status and retry output, examples, and verification. |
| [`cli/build-events.md`](cli/build-events.md) | The `liquid build --plan` and `--events` JSON-lines schemas, daemon operations, ordering, exit status, and edit behavior. |
| [`cli/test.md`](cli/test.md) | CMake/native test discovery, filtering, cached action behavior, output, and exit status. |
| [`cli/drive.md`](cli/drive.md) | `liquid drive` sync, status, pin, and unpin: remote configuration, reconciliation, and credentials. |
| [`cli/history.md`](cli/history.md) | `liquid history`: stable-node path history listing and restore. |

## Server API

| Page | What it contains |
|---|---|
| [`api/README.md`](api/README.md) | The server components and which wire surfaces are public. |
| [`api/admission.md`](api/admission.md) | Named Kubernetes webhook, decision stream, control, metrics, REST, OpenAPI, and MCP routes. |
| [`api/scheduler.md`](api/scheduler.md) | The scheduler's h2c gRPC data listener and HTTP/1 control listener, with REAPI instance validation. |
| [`api/worker.md`](api/worker.md) | The worker's outbound data connections and its single inbound HTTP/1 control endpoint. |
| [`api/node-store.md`](api/node-store.md) | The node store's local Unix-socket snapshotter boundary and TCP control listener. |

## Build verification

| Page | What it contains |
|---|---|
| [`build/README.md`](build/README.md) | Index of the build verification pages. |
| [`build/local.md`](build/local.md) | Building liquid on a workstation: prerequisites, configure, build, test, run, install, options, and the documentation checks. |
| [`build/ci.md`](build/ci.md) | GitHub Actions platform matrix, sanitizer coverage, toolchains, the documentation-site job, and the local Linux container proof. |
| [`build/sandboxed-executor.md`](build/sandboxed-executor.md) | Issue #10's local executor checkpoint: fixed environment, observed-mount manifest, Linux namespace enforcement, platform limits, and the mount-proof dependency on #130. |
