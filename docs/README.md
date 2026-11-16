# liquid documentation

The documentation separates evidence, specification, external standards, and implementation
notes so that each claim has a clear role. Start with the [first result](../README.md#build-and-first-result)
or an [executable guide](guides/README.md). [`library/status.md`](library/status.md) is the
authoritative implementation inventory; [`features/`](features/) holds specifications and
[`research/99-synthesis/`](research/99-synthesis/) explains the design argument.

| Area | What it contains |
|---|---|
| [`research/`](research/) | What the world currently does, what works, what is broken, and what people actually complain about. 27 topics, each with a curated analysis and, for 24 of them, the web-grounded sources it was drawn from. |
| [`features/`](features/) | What liquid does: the premise, the names, the design axioms, what it must do, and 25 feature areas, each traceable to the research. |
| [`standards/`](standards/) | External standards liquid depends on; currently [TAAP/1](standards/taap-1-spec-draft.md), vendored until it has its own repository. |
| [`library/`](library/README.md) | Implementation, design, and benchmark notes for libliquid contributors, indexed below. |
| [`cli/README.md`](cli/README.md) | Command-line commands, structural edits, hot updates, named build inputs, retention explanations, output, capture, drive sync, path history, flow status and retry output, examples, and verification. |
| [`cli/git-commands.md`](cli/git-commands.md) | The rule that liquid uses Git's command names, every command that matches, every deliberate difference with its reason, and the open cases. |
| [`reference/`](reference/) | Generated command, flag, example, availability, and exit-status reference. |
| [`guides/`](guides/) | Executable user workflows for a first repository, existing builds, encryption, [packaging ordinary files](guides/project-from-files.md), and [package resolution](guides/resolve-packages.md). |
| [`api/README.md`](api/README.md) | Server components and their public wire surfaces, including admission, scheduler, worker, and node store. |
| [`build/`](build/README.md) | Local and continuous-integration build verification, indexed below. |
| [`security/`](security/README.md) | Threat models for liquid's trust boundaries: assets, actors, must-nevers with where each is enforced and its test, and the defects found; first [delegation and multiplayer sessions](security/delegation-threat-model.md). |
| [`releases/`](releases/README.md) | One page per tagged release and how a release is cut; [`v0.1.0`](releases/v0.1.0.md) is the first and lists every implemented feature and command; [`v0.2.0`](releases/v0.2.0.md) is the current one. |

## Library

### Status, shape, and public surface

| Page | What it contains |
|---|---|
| [`library/status.md`](library/status.md) | What libliquid and the `liquid` binary implement today, by phase, and the known limits in one place. |
| [`library/libliquid.md`](library/libliquid.md) | libliquid targets, installed static dependency closure, public API, consumption, and relocated repository/coroutine verification. |
| [`library/api-map.md`](library/api-map.md) | The implemented and declared public headers of libliquid, including flow configuration and status, the rules every one obeys, dependency provenance, what is deliberately internal, and the ABI policy. |
| [`library/runtime.md`](library/runtime.md) | The Capy task API, Corosio context, executor model, blocking-work adapter, and coroutine safety rules. |
| [`library/hash-agility.md`](library/hash-agility.md) | Digest and signature algorithm tags, mixed BLAKE3/SHA-256 stores, rehashing, and present limits. |
| [`library/http2.md`](library/http2.md) | Private Corosio HTTP/2 framing, HPACK, gRPC envelopes, and h2c/TLS ALPN interoperability proof. |
| [`library/readme-history.md`](library/readme-history.md) | Historical README context, reconciliation decisions, and the evidence used to retain competing feature additions. |

### Store, repository, and identity

| Page | What it contains |
|---|---|
| [`library/object-model.md`](library/object-model.md) | Repository records, structural-edit operations, stable identities and anchors, checked compact-or-full CRDT checkpoints, historical state, composition, conflicts, undo, and locks. |
| [`library/split.md`](library/split.md) | Standalone split ownership, original history and identities, nested repositories, publication, and verification. |
| [`library/conflicts.md`](library/conflicts.md) | Issue #40's canonical first-class conflict record, tree and change references, producer contract, status and resolution CLI, sync reachability, and build behavior. |
| [`library/structural-merge.md`](library/structural-merge.md) | Issue #69's structural three-way merge at CRDT convergence, suspicious proposal rules, explicit resolution, operation metadata, and merge-quality measurement. |
| [`library/locks.md`](library/locks.md) | Exclusive checkout rules and records, offline reconciliation, connected lake authority, CLI use, and daemon delivery. |
| [`library/multiplayer.md`](library/multiplayer.md) | Private sessions, leases, presence, epochs, stable-node per-line attribution, daemon transport, plugin delivery, CLI output, and the 10,000-line measurement. |
| [`library/repository-import-primitives.md`](library/repository-import-primitives.md) | Importer-facing repository recording, metadata, historical-object, mode, and nesting primitives. |
| [`library/store.md`](library/store.md) | Content-defined local and caller-owned shared stores, resemblance-selected chunk deltas, repacking, pre-pack write-through, remote-registered local eviction, on-disk formats, integrity checks, index, tuning, and rebuild. |
| [`library/chunk-index-benchmark.md`](library/chunk-index-benchmark.md) | Reproducible RocksDB/LMDB chunk-index workload and recorded measurements. |
| [`library/gc.md`](library/gc.md) | Repository-scoped leased roots, delta-base closure, typed retention rules, cost-aware result eviction, epoch bitmaps, reference extraction, crash-safe sweep order, and collection verification. |
| [`library/simulation.md`](library/simulation.md) | Deterministic seeded store simulation, fault injection, recovery invariants, and replay. |
| [`library/encryption.md`](library/encryption.md) | Trust-domain encrypted store objects, versioned data-key rotation and revocation, readability reports, TPM sealing and other key custody, on-disk envelopes, and failure behavior. |
| [`library/leaf-encryption.md`](library/leaf-encryption.md) | JSON and `.env` leaf encryption, repository path rules, structural key diff, and whole-file fallback. |
| [`library/sealed-secrets.md`](library/sealed-secrets.md) | The X25519 and XChaCha20-Poly1305 Lake envelope, canonical CBOR, exact scope binding, Go mapping, key custody, and interoperability fixture. |
| [`library/identity.md`](library/identity.md) | TAAP/1 public document export, software/FIDO2/PIV keys, canonical-CBOR codecs, rotation, signatures, and verification. |
| [`library/release-approval.md`](library/release-approval.md) | Signed release policies, distinct-signer threshold evaluation, TUF alignment, custody trust models, CLI storage, and compromise handling. |
| [`library/attestations.md`](library/attestations.md) | TAAP profile registry, typed schemas, freshness, status evidence, selective disclosure, and verification. |
| [`library/secret-scanning.md`](library/secret-scanning.md) | Inline secret detection, repository exceptions, signed scan attestations, redaction, and measured cost. |
| [`library/capture.md`](library/capture.md) | Change capture, write flow control, watcher recovery, coalescing, cursor delivery, and telemetry. |
| [`library/vfs.md`](library/vfs.md) | Lazy and policy-driven chunk hydration, drive xattrs and read-only mutation refusal, FUSE mounts, the request queue, full materialisation, platform coverage, and named backend conformance with direct core calibration and Linux libfuse proof still owed. |
| [`library/vfs-conformance-proof.md`](library/vfs-conformance-proof.md) | Nine-case direct core calibration, deliberate wrong-size failure, native suite results, and outstanding Linux libfuse proof. |
| [`library/git-interop.md`](library/git-interop.md) | Git import/export mapping, compatibility limits, and verification. |
| [`library/drive.md`](library/drive.md) | Issues #195, #235, and #258's persisted three-tree reconciliation, per-repository CRDT actors, path device states, pinning, hydration and offline policy, Linux desktop badges, conflicts, transfer resume, history, daemon watcher, and bounded scan state. |
| [`library/sync.md`](library/sync.md) | Repository metadata, CRDT, ref, and pack-entry exchange through the public server and HTTP/1.1 client, including peer buckets, retry signals, change authorization, per-ref outcomes, and atomic pushes. |
| [`library/forge-client.md`](library/forge-client.md) | Lake review records, coroutine calls, anchor advancement, polling events, error mapping, and the installable forge double. |
| [`library/transparency.md`](library/transparency.md) | Lake checkpoint, complete parent-tile walk, cache reauthentication, inclusion, consistency, identity-scan, RFC 3161 timestamp, and RFC 4998 evidence verification; measured cost, trust bootstrap, CLI, local URL validation, public construction errors, and fixture regeneration. |
| [`library/tuf.md`](library/tuf.md) | TUF root bootstrap, ordered metadata verification, rollback state, target acceptance gate, and fixture provenance. |
| [`library/admission.md`](library/admission.md) | Image provenance statements, atomically reloaded named policy sets, fail-closed evaluation, evidence sources, authenticated checkpoint age, TLS serving, structured decisions, and bounded replay. |
| [`library/deploy.md`](library/deploy.md) | Operator control-plane deployment client, digest-pinned interim images, rollback, status, and the provisional OpenAPI contract. |

### Build engine and distribution

| Page | What it contains |
|---|---|
| [`library/build-engine.md`](library/build-engine.md) | Action and result records, named upstream inputs, output trees and normalisation, observed reads, cache lookup, public build streams and plans, configure facts, traces, and wrapping existing builds. |
| [`library/distribution.md`](library/distribution.md) | Phase 3 design and implemented remote blob hydration, local replay, signed delta delivery, #18 scheduling, #16 REAPI interoperability, and #63's scheduler and workers. |
| [`library/workers.md`](library/workers.md) | Scheduler and worker protocol, bounded requeue, TAAP result identity, configuration, control endpoints, and deployment shape. |
| [`library/node-store.md`](library/node-store.md) | Per-node shared chunk storage, metadata-only image mounts, listener boundaries, accounting, budgets, and eviction. |
| [`library/reapi-codec.md`](library/reapi-codec.md) | Dependency-free protobuf wire encoding, the REAPI v2.3 structs, native record conversion, hash selection, and pinned reference fixtures. |
| [`library/reapi.md`](library/reapi.md) | REAPI client and server RPCs, bounded resumable ByteStream uploads, the remote cache tier, configuration and CLI, public API map, and NativeLink, BuildBarn, and Bazel proof. |
| [`library/self-host.md`](library/self-host.md) | The liquid-on-liquid build, fixed-root CMake configuration, cache assertions, and bootstrap output comparison. |
| [`library/provenance.md`](library/provenance.md) | Automatic result provenance: canonical TAAP storage, the exact SLSA v1 mapping, verification, and in-toto/DSSE export. |
| [`library/policy.md`](library/policy.md) | Build-input policy: path and toolchain scopes, verified attestation quorums, refusal and warning behavior, and cache isolation. |
| [`library/sbom.md`](library/sbom.md) | Graph-derived SPDX and CycloneDX SBOMs, signed records, distinct foreign-binary inference, and verification. |
| [`library/abi.md`](library/abi.md) | ELF, Mach-O, and PE ABI descriptors, compatibility rules, automatic signed comparisons, public access, and CLI use. |
| [`library/relocation.md`](library/relocation.md) | Position-independent install-tree rules, build-system conventions, binary and metadata enforcement, public checking, and recorded waivers. |
| [`library/data-branching.md`](library/data-branching.md) | Per-session PostgreSQL application-data branches, lifecycle and promotion guard, template-copy baseline, local reflink fast path, and sandboxed client-tool execution. |

### Incremental engine, frontends, and search

| Page | What it contains |
|---|---|
| [`library/incremental-engine.md`](library/incremental-engine.md) | Query graph, clean validation, daemon lifecycle, cursor fan-out, flow limits, build/test streams, value eviction, frontend granularity, search, symbols, and IDE integration. |
| [`library/frontends.md`](library/frontends.md) | Versioned frontend and liquify plugin C descriptors, multiplayer callbacks, module loading, tree-sitter fallback, Clang definition actions, graph invalidation, assembly, and measurements. |
| [`library/structural-diff.md`](library/structural-diff.md) | Graph-owned canonical parses, source-tree matching, public structural edits, explicit fallback tiers, CLI output, coverage, and measurement. |
| [`library/grammars.md`](library/grammars.md) | The digest-pinned tree-sitter runtime and 49-grammar lock, trust decision, build controls, inventory, update procedure, and measurements. |
| [`library/symbols.md`](library/symbols.md) | Content-addressed per-translation-unit symbol records, query behavior, fallback labelling, public repository access, and CLI use. |
| [`library/edit.md`](library/edit.md) | Structural rename, extract, move, and concrete-tree edits; canonical records, preview/apply guarantees, attribution, grouped undo, CLI use, limits, and own-tree measurement. |
| [`library/hot-update.md`](library/hot-update.md) | Tier 0–4 evidence rules, signed patch and restart records, the cooperative `liquid-hot` C ABI, daemon delivery, and reconstructible process composition. |
| [`library/search.md`](library/search.md) | Issues #22, #67, and #29's BM25F, positional-trigram, and mapped-scan executors; archive entries; cost model; atomic publication; explanations; public APIs; CLI; and crossover measurements. |
| [`library/support-tiers.md`](library/support-tiers.md) | Issue #71's registry-derived language, format, and platform support matrix and strongest-frontend selection. |

### Formats, projection, resolution, and configuration

| Page | What it contains |
|---|---|
| [`library/formats.md`](library/formats.md) | Format registry and C ABI, original archive/data codecs and Tier 2 artifact codecs, fidelity tiers, limits, nested paths, diff/search/storage integration, and measurements. |
| [`library/projection.md`](library/projection.md) | Native and ecosystem packages, platform installers and signing, Debian source packages, OSTree and bootc, install intents, ownership and atomic generations, vendored AppImage runtime provenance, deterministic writers, attestations, and peer proof. |
| [`library/resolution.md`](library/resolution.md) | Package and concretised records, spec grammar, universe layout, solver model, explanations, optimisation, and scale measurements. |
| [`library/configuration-language.md`](library/configuration-language.md) | The typed `.liquid` grammar, exact diagnostics, package/target/source/install-intent lowering, repository-local includes, verified source fetch, generators, and trace origins. |
| [`build/sandboxed-executor.md`](build/sandboxed-executor.md) | Local action execution: fixed environment, observed-mount manifest, Linux namespace enforcement, platform limits, and mount verification. |
| [`api/admission.md`](api/admission.md) | Named Kubernetes webhook, decision stream, control, metrics, REST, OpenAPI, and MCP routes. |
| [`library/import-spack.md`](library/import-spack.md) | Pinned Spack export, package-field mapping, import reports and losses, the 500-package universe, and the Linux build subset. |

## Command line

| Page | What it contains |
|---|---|
| [`cli/build-events.md`](cli/build-events.md) | The `liquid build --plan` and `--events` JSON-lines schemas, daemon operations, ordering, exit status, and edit behavior. |
| [`cli/test.md`](cli/test.md) | CMake/native test discovery, filtering, cached action behavior, output, and exit status. |
| [`cli/drive.md`](cli/drive.md) | `liquid drive` sync, status, pin, and unpin: remote configuration, reconciliation, and credentials. |
| [`cli/history.md`](cli/history.md) | `liquid history`: stable-node path history listing and restore. |

## Server API

| Page | What it contains |
|---|---|
| [`api/scheduler.md`](api/scheduler.md) | The scheduler's h2c gRPC data listener and HTTP/1 control listener, with REAPI instance validation. |
| [`api/worker.md`](api/worker.md) | The worker's outbound data connections and its single inbound HTTP/1 control endpoint. |
| [`api/node-store.md`](api/node-store.md) | The node store's local Unix-socket snapshotter boundary and TCP control listener. |

## Build verification

| Page | What it contains |
|---|---|
| [`build/local.md`](build/local.md) | Building liquid on a workstation: prerequisites, configure, build, test, run, install, options, and the documentation checks. |
| [`build/socket-fixtures.md`](build/socket-fixtures.md) | Unix-domain socket limits, bounded temporary fixtures, cleanup, and deep-worktree verification. |
| [`build/linux-container.md`](build/linux-container.md) | The full suite in an unprivileged Linux container: run-time capability probes, the tests that skip and why, and every Linux failure classified (#348). |
| [`build/hardening.md`](build/hardening.md) | Release hardening flags, sanitizer builds over every C and C++ source, the libFuzzer harnesses, how to run them locally and on the BuildKit lane, and the recorded runs. |
| [`build/ci.md`](build/ci.md) | GitHub Actions platform matrix, sanitizer coverage, toolchains, the documentation-site job, and the local Linux container proof. |
| [`build/phase-2-exit-lane-audit.md`](build/phase-2-exit-lane-audit.md) | Why the Phase 2 exit (#299) has not been demonstrated: each candidate machine checked for a FUSE mount, on 2026-09-08 and 2026-10-12, with commands, output and the decision needed. |
