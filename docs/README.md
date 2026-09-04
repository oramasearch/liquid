# liquid documentation

The documentation separates evidence, specification, external standards, and implementation
notes so that each claim has a clear role.

| Area | What it contains |
|---|---|
| [`research/`](research/) | Curated analysis and its web-grounded sources. |
| [`features/`](features/) | Specifications for what liquid does. |
| [`standards/`](standards/) | External standards liquid depends on. |
| [`library/libliquid.md`](library/libliquid.md) | libliquid targets, public API, consumption, and verification. |
| [`library/object-model.md`](library/object-model.md) | The records in the store, the CRDT metadata document, how they reference each other, and how composition, conflicts, undo and locks work. Design for liquid #2 and #3. |
| [`library/api-map.md`](library/api-map.md) | The implemented and declared public headers of libliquid for Phase 1, the rules every one obeys, dependency provenance, what is deliberately internal, and the ABI policy. |
| [`library/runtime.md`](library/runtime.md) | The Capy task API, Corosio context, executor model, blocking-work adapter, and coroutine safety rules. |
| [`library/chunk-index-benchmark.md`](library/chunk-index-benchmark.md) | Reproducible RocksDB/LMDB chunk-index workload and recorded measurements. |
| [`build/ci.md`](build/ci.md) | GitHub Actions platform matrix, sanitizer coverage, and toolchains. |
