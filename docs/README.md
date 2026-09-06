# liquid documentation

The documentation separates evidence, specification, external standards, and implementation
notes so that each claim has a clear role.

| Area | What it contains |
|---|---|
| [`research/`](research/) | Curated analysis and its web-grounded sources. |
| [`features/`](features/) | Specifications for what liquid does. |
| [`standards/`](standards/) | External standards liquid depends on. |
| [`library/libliquid.md`](library/libliquid.md) | libliquid targets, public API, consumption, and verification. |
| [`library/object-model.md`](library/object-model.md) | The records in local or caller-owned shared stores, public tree/change diff, stale-reasoning guards, edit-surviving text anchors, compact/shallow CRDT checkpoints, historical document state, measured open cost, and how composition, conflicts, operation/session undo, and locks work. Design for liquid #2, #3, #41, #86, #136, #165, #166, and #174. |
| [`library/build-engine.md`](library/build-engine.md) | Phase 2's design plus #18's remote scheduling record, #166's paged public record enumeration, #193's public build streams, and #200's configured repository plans and one-call builds: action and result records, observation by the mount with per-platform enforcement, cache lookup, event ordering and JSON projection, retained timing and scheduling traces, content-keyed configure facts, and how `liquid wrap` splits an existing build into actions. |
| [`library/distribution.md`](library/distribution.md) | Phase 3 design and implemented remote blob hydration, local replay, signed delta delivery, #18 scheduling, #16 REAPI interoperability, and #63's scheduler and workers. |
| [`library/workers.md`](library/workers.md) | Scheduler and worker protocol, bounded requeue, TAAP result identity, configuration, control endpoints, and deployment shape. |
| [`api/README.md`](api/README.md) | Scheduler and worker network capability maps. |
| [`library/incremental-engine.md`](library/incremental-engine.md) | Phase 4's implemented demand-driven query graph, measured restart and continuous clean validation, daemon lifecycle, build-stream forwarding, cached tests as actions, cursor fan-out, graph value eviction, per-definition frontend granularity, local search index, and cost planner; also designs symbols and liquify on the graph. |
| [`library/frontends.md`](library/frontends.md) | The versioned frontend C ABI, module loading, tree-sitter fallback, Clang C++ definition actions, graph invalidation, assembly, and own-tree measurement. |
| [`library/grammars.md`](library/grammars.md) | The digest-pinned tree-sitter runtime and 49-grammar lock, trust decision, build controls, inventory, update procedure, and measurements. |
| [`library/search.md`](library/search.md) | Issues #22 and #67's BM25F, positional-trigram, and mapped-scan executors; cost model; atomic publication; explanations; public APIs; CLI; and crossover measurements. |
| [`library/symbols.md`](library/symbols.md) | Content-addressed per-translation-unit symbol records, query behavior, fallback labelling, public repository access, and CLI use. |
| [`library/reapi-codec.md`](library/reapi-codec.md) | Dependency-free protobuf wire encoding, the REAPI v2.3 structs, native record conversion, hash selection, and pinned reference fixtures. |
| [`library/reapi.md`](library/reapi.md) | REAPI client and server RPCs, the remote cache tier, configuration and CLI, public API map, and NativeLink, BuildBarn, and Bazel proof. |
| [`library/provenance.md`](library/provenance.md) | Automatic result provenance: canonical TAAP storage, the exact SLSA v1 mapping, verification, and in-toto/DSSE export. |
| [`library/policy.md`](library/policy.md) | Build-input policy: path and toolchain scopes, verified attestation quorums, refusal and warning behavior, and cache isolation. |
| [`library/sbom.md`](library/sbom.md) | Graph-derived SPDX and CycloneDX SBOMs, signed records, distinct foreign-binary inference, and verification. |
| [`library/abi.md`](library/abi.md) | ELF, Mach-O, and PE ABI descriptors, compatibility rules, automatic signed comparisons, public access, and CLI use. |
| [`build/sandboxed-executor.md`](build/sandboxed-executor.md) | Issue #10's local executor: fixed environment, observed-mount manifest, Linux namespace enforcement, and platform limits. |
| [`library/api-map.md`](library/api-map.md) | The implemented and declared public headers of libliquid for Phase 1, the rules every one obeys, dependency provenance, what is deliberately internal, and the ABI policy. |
| [`library/hash-agility.md`](library/hash-agility.md) | Digest and signature algorithm tags, mixed BLAKE3/SHA-256 stores, rehashing, and present limits. |
| [`library/runtime.md`](library/runtime.md) | The Capy task API, Corosio context, executor model, blocking-work adapter, and coroutine safety rules. |
| [`library/http2.md`](library/http2.md) | Private Corosio HTTP/2 framing, HPACK, gRPC envelopes, and h2c/TLS ALPN interoperability proof. |
| [`library/sync.md`](library/sync.md) | Repository metadata, CRDT, ref, and pack-entry exchange through the public server and HTTP/1.1 client, including change authorization, per-ref outcomes, and atomic pushes. |
| [`library/repository-import-primitives.md`](library/repository-import-primitives.md) | Importer-facing repository recording, metadata, historical-object, mode, and nesting primitives. |
| [`library/git-interop.md`](library/git-interop.md) | Git import/export mapping, compatibility limits, and verification. |
| [`library/chunk-index-benchmark.md`](library/chunk-index-benchmark.md) | Reproducible RocksDB/LMDB chunk-index workload and recorded measurements. |
| [`library/store.md`](library/store.md) | Content-defined local and caller-owned shared stores, pre-pack write-through, remote-registered local eviction, on-disk formats, integrity checks, index, tuning, and rebuild. |
| [`library/gc.md`](library/gc.md) | Repository-scoped leased roots, typed retention rules, cost-aware result eviction, epoch bitmaps, reference extraction, crash-safe sweep order, and collection verification. |
| [`library/encryption.md`](library/encryption.md) | Trust-domain encrypted store objects, versioned data-key rotation and revocation, readability reports, key custody, on-disk envelopes, and failure behavior. |
| [`library/leaf-encryption.md`](library/leaf-encryption.md) | JSON and `.env` leaf encryption, repository path rules, structural key diff, and whole-file fallback. |
| [`library/simulation.md`](library/simulation.md) | Deterministic seeded store simulation, fault injection, recovery invariants, and replay. |
| [`library/identity.md`](library/identity.md) | TAAP/1 identity documents, public canonical-CBOR codecs, rotation, signatures, and verification. |
| [`library/vfs.md`](library/vfs.md) | Lazy chunk hydration, FUSE mounts, the request queue, full materialisation, and platform coverage. |
| [`library/capture.md`](library/capture.md) | Change capture, watcher recovery, coalescing, cursor delivery, and telemetry. |
| [`library/secret-scanning.md`](library/secret-scanning.md) | Inline secret detection, repository exceptions, signed scan attestations, redaction, and measured cost. |
| [`library/attestations.md`](library/attestations.md) | TAAP profile registry, typed schemas, freshness, status evidence, selective disclosure, and verification. |
| [`cli/README.md`](cli/README.md) | Command-line commands, retention explanations, output, capture, examples, and verification. |
| [`cli/build-events.md`](cli/build-events.md) | The `liquid build --plan` and `--events` JSON-lines schemas, daemon operations, ordering, exit status, and edit behavior. |
| [`cli/test.md`](cli/test.md) | CMake/native test discovery, filtering, cached action behavior, output, and exit status. |
| [`build/ci.md`](build/ci.md) | GitHub Actions platform matrix, sanitizer coverage, and toolchains. |
| [`build/sandboxed-executor.md`](build/sandboxed-executor.md) | Issue #10 executor checkpoint and the mount-proof dependency on #130. |
