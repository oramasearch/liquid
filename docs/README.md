# liquid documentation

The documentation separates evidence, specification, external standards, and implementation
notes so that each claim has a clear role.

| Area | What it contains |
|---|---|
| [`research/`](research/) | Curated analysis and its web-grounded sources. |
| [`features/`](features/) | Specifications for what liquid does. |
| [`standards/`](standards/) | External standards liquid depends on. |
| [`library/libliquid.md`](library/libliquid.md) | libliquid targets, public API, consumption, and verification. |
| [`library/object-model.md`](library/object-model.md) | The records in the store, compact/shallow CRDT checkpoints, measured open cost, and how composition, conflicts, undo and locks work. Design for liquid #2, #3, and #136. |
| [`library/build-engine.md`](library/build-engine.md) | Phase 2's design plus #18's remote scheduling record: action and result records, observation by the mount with per-platform enforcement, cache lookup, retained timing and scheduling traces, content-keyed configure facts, and how `liquid wrap` splits an existing build into actions. |
| [`library/distribution.md`](library/distribution.md) | Phase 3 design and implemented remote blob hydration, local replay, signed delta delivery, and #18 remote scheduling through the shared remote interfaces, plus the remaining transport and worker work. |
| [`library/reapi-codec.md`](library/reapi-codec.md) | Dependency-free protobuf wire encoding, the REAPI v2.3 structs, native record conversion, hash selection, and pinned reference fixtures. |
| [`library/provenance.md`](library/provenance.md) | Automatic result provenance: canonical TAAP storage, the exact SLSA v1 mapping, verification, and in-toto/DSSE export. |
| [`library/policy.md`](library/policy.md) | Build-input policy: path and toolchain scopes, verified attestation quorums, refusal and warning behavior, and cache isolation. |
| [`library/sbom.md`](library/sbom.md) | Graph-derived SPDX and CycloneDX SBOMs, signed records, distinct foreign-binary inference, and verification. |
| [`build/sandboxed-executor.md`](build/sandboxed-executor.md) | Issue #10's local executor: fixed environment, observed-mount manifest, Linux namespace enforcement, and platform limits. |
| [`library/api-map.md`](library/api-map.md) | The implemented and declared public headers of libliquid for Phase 1, the rules every one obeys, dependency provenance, what is deliberately internal, and the ABI policy. |
| [`library/hash-agility.md`](library/hash-agility.md) | Digest and signature algorithm tags, mixed BLAKE3/SHA-256 stores, rehashing, and present limits. |
| [`library/runtime.md`](library/runtime.md) | The Capy task API, Corosio context, executor model, blocking-work adapter, and coroutine safety rules. |
| [`library/http2.md`](library/http2.md) | Private Corosio HTTP/2 framing, HPACK, gRPC envelopes, h2c proof, and current TLS ALPN limit. |
| [`library/repository-import-primitives.md`](library/repository-import-primitives.md) | Importer-facing repository recording, metadata, historical-object, mode, and nesting primitives. |
| [`library/git-interop.md`](library/git-interop.md) | Git import/export mapping, compatibility limits, and verification. |
| [`library/chunk-index-benchmark.md`](library/chunk-index-benchmark.md) | Reproducible RocksDB/LMDB chunk-index workload and recorded measurements. |
| [`library/store.md`](library/store.md) | Content-defined local store, on-disk formats, integrity checks, index, tuning, and rebuild. |
| [`library/gc.md`](library/gc.md) | Leased roots, typed retention rules, cost-aware result eviction, epoch bitmaps, reference extraction, crash-safe sweep order, and collection verification. |
| [`library/encryption.md`](library/encryption.md) | Trust-domain encrypted store objects, versioned data-key rotation and revocation, readability reports, key custody, on-disk envelopes, and failure behavior. |
| [`library/leaf-encryption.md`](library/leaf-encryption.md) | JSON and `.env` leaf encryption, repository path rules, structural key diff, and whole-file fallback. |
| [`library/simulation.md`](library/simulation.md) | Deterministic seeded store simulation, fault injection, recovery invariants, and replay. |
| [`library/vfs.md`](library/vfs.md) | Lazy chunk hydration, FUSE mounts, the request queue, full materialisation, and platform coverage. |
| [`library/capture.md`](library/capture.md) | Change capture, watcher recovery, coalescing, cursor delivery, and telemetry. |
| [`library/secret-scanning.md`](library/secret-scanning.md) | Inline secret detection, repository exceptions, signed scan attestations, redaction, and measured cost. |
| [`library/attestations.md`](library/attestations.md) | TAAP profile registry, typed schemas, freshness, status evidence, selective disclosure, and verification. |
| [`cli/README.md`](cli/README.md) | Command-line commands, retention explanations, output, capture, examples, and verification. |
| [`build/ci.md`](build/ci.md) | GitHub Actions platform matrix, sanitizer coverage, and toolchains. |
| [`build/sandboxed-executor.md`](build/sandboxed-executor.md) | Issue #10 executor checkpoint and the mount-proof dependency on #130. |
