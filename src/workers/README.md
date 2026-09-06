# Scheduler and worker implementation

`scheduler.cpp` owns the mutex-protected action-digest operation table, duplicate joining,
size-class placement, lease tokens, heartbeat expiry, and the single requeue. `protocol.cpp` owns
the dependency-free protobuf messages used between matching liquid schedulers and workers.

`worker.cpp` hydrates native input and toolchain graphs into a co-located repository, invokes the
Phase 2 executor, uploads output graphs, and completes with the worker's TAAP-signed result,
provenance, SBOMs, ABI descriptors, and ABI compatibility attestations. `control.cpp` contains the
isolated fixed route registry for health, readiness, Prometheus metrics, REST discovery, OpenAPI,
and MCP discovery.

The worker detects an existing repository through its `LIQUID_REPO` marker. Reusing a worker root
therefore reopens the co-located CAS and preserves the TAAP identity used to sign results.

Public options and entry points are in `include/liquid/workers.hpp`. Protocol and deployment
details are in `docs/library/workers.md`; server routes are mapped in `docs/api/`.
