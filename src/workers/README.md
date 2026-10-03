# Scheduler and worker implementation

`scheduler.cpp` owns the mutex-protected action-digest operation table, duplicate joining,
size-class placement, lease tokens, heartbeat expiry, and the single requeue. `protocol.cpp` owns
the dependency-free protobuf messages used between matching liquid schedulers and workers.

`worker.cpp` hydrates native input and toolchain graphs into a co-located repository, invokes the
Phase 2 executor, uploads output graphs, and completes with the worker's TAAP-signed result,
provenance, SBOMs, ABI descriptors, and ABI compatibility attestations. Its remote object reads use
a heap 64 KiB buffer, so the reading coroutine reserves no large stack in Debug (liquid #323).
`control.cpp` contains the isolated fixed route registry for health, readiness, Prometheus
metrics, REST discovery, OpenAPI, and MCP discovery. It runs through the shared private HTTP/1.1
server adapter used by other liquid control surfaces.
provenance, SBOMs, ABI descriptors, and ABI compatibility attestations. `control.cpp` registers
health, readiness, Prometheus metrics, REST discovery, OpenAPI, and MCP discovery handlers in the
shared asynchronous route table under `src/serve/`.

The worker detects an existing repository through its `LIQUID_REPO` marker. Reusing a worker root
therefore reopens the co-located CAS and preserves the TAAP identity used to sign results.

Public options and entry points are in `include/liquid/workers.hpp`. Protocol and deployment
details are in `docs/library/workers.md`; server routes are mapped in `docs/api/`.
