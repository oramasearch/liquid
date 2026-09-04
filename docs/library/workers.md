# Scheduler and worker fleet

**Status.** Issue #63 is implemented: liquid owns an execution scheduler, a long-running worker,
and the client path that carries #18's recorded cost decision to that fleet. The scheduler also
offers the REAPI Execution and ActionCache services, so a stock REAPI client and a liquid client
share one queue and CAS.

## Data path

The client prepares a normal REAPI `Action` and `Command`, uploads their Merkle inputs through
CAS, and submits the action digest. A liquid submission also carries the exact
`execution_schedule` selected by the cost model. The scheduler keys its operation table by action
digest. A repeated submission returns the existing operation name; it does not create another
queue entry.

A worker polls `Lease` with its platform, maximum size class, and TAAP identifier. Placement
requires an exact OS and architecture match. A worker may accept its own size class or a smaller
one, and the scheduler selects the largest eligible queued action first. The lease contains the
encoded Action, Command, scheduling record, opaque token, and retry count.

The worker checks its co-located liquid store first. Missing native input-tree, toolchain-tree,
blob, and chunk records are read from the scheduler CAS and verified against their native digest
before storage. The existing Phase 2 executor reconstructs the declared toolchain, replaces
captured fixed environment entries with its deterministic sandbox values, runs the action in a
fresh sandbox, applies the Action timeout, and captures stdout, stderr, outputs, timings, and the
observed-input manifest. Output streams and their native blob graphs are uploaded through CAS
before completion.

Clients upload each prepared native graph directly. Repeated object writes are digest-idempotent,
which avoids a large preflight request while retaining the scheduler CAS as the source for workers.

For a captured wrap action, the client carries the captured action family's timing history into
placement and maps its selected executable into the Phase 2 `toolchain/bin` layout. The scheduler
therefore places it from the established measurements while workers use the same executable lookup
and sandbox path as direct Phase 2 actions.

The worker sets `action_result.worker` to its TAAP identifier and signs the result, SLSA
provenance, and both SBOM forms with the same identity used for the lease. The scheduler rejects a
completion whose operation, token, worker, scheduling record, or signature differs from the active
lease. The client imports those exact signed records and verifies provenance and SBOMs; it does not
replace the worker signature with a client signature. `liquid why` therefore shows the same
history, size class, estimates, and reason that drove placement.

## Worker loss and duplicates

Workers heartbeat while an action runs. A lease that expires is returned to the queue with retry
count one. If that second lease expires or reports failure, the operation fails and is never
queued again. A late result from an expired token is rejected. A repeated completion of the
already accepted token is idempotently accepted.

This is at-least-once execution with one bounded retry, not exactly-once execution. Correctness
comes from the action digest, isolated execution, verified output digests, and the signed result.
An ActionCache entry is written only after the scheduler accepts the completion.

## Commands and configuration

```sh
liquid scheduler --listen 0.0.0.0:50051 \
  --control-listen 0.0.0.0:8080 \
  --store /var/lib/liquid-scheduler \
  --instance builds

liquid worker --scheduler scheduler.example:50051 \
  --control-listen 0.0.0.0:8081 \
  --root /var/lib/liquid-worker \
  --instance builds \
  --size-class long

liquid wrap --scheduler scheduler.example:50051 --remote-instance builds make -j8
```

The scheduler accepts `LIQUID_SCHEDULER_LISTEN`, `LIQUID_CONTROL_LISTEN`, `LIQUID_INSTANCE`,
`LIQUID_SCHEDULER_STORE`, and `LIQUID_SCHEDULER_LEASE_MS`. The worker accepts
`LIQUID_SCHEDULER`, `LIQUID_CONTROL_LISTEN`, `LIQUID_INSTANCE`, `LIQUID_WORKER_ROOT`,
`LIQUID_WORKER_SIZE_CLASS`, and `LIQUID_WORKER_POLL_MS`. Flags override environment values. Both
commands use only the local filesystem and the named network endpoints; credentials and TLS use
the existing `remote_config` fields.

`remote_config::backend` selects `reapi` or `liquid_scheduler`. `--remote` retains the stock REAPI
path. `--scheduler` selects the liquid path and persists that choice with the repository's remote
configuration.

## Server surfaces

The scheduler's gRPC listener offers standard REAPI Capabilities, CAS, ByteStream, ActionCache,
Execution, and Operations RPCs. It adds private Submit, Lease, Heartbeat, Complete, ReadObject,
and Status methods for native scheduling records and worker operation. The detailed map is in
[`../api/scheduler.md`](../api/scheduler.md).

Each component has a separate HTTP/1 control listener:

| Method | Path | Contract |
|---|---|---|
| `GET` | `/healthz` | Process and listener are healthy. |
| `GET` | `/readyz` | Scheduler has a live worker; worker has connected to its scheduler. |
| `GET` | `/metrics` | Prometheus text counters and gauges. |
| `GET` | `/v1` | Component and API version discovery. |
| `GET` | `/v1/openapi.json` | OpenAPI 3.1 description of the control listener. |
| `POST` | `/mcp` | MCP JSON-RPC discovery response. No mutating tools are registered. |

No shared registry library is present in this repository. `src/workers/control.cpp` is the minimal
fixed route registry and is deliberately isolated for extraction when the organization has a
shared registry.

## Deployment shape

[`../../deploy/workers/`](../../deploy/workers/) contains separate scheduler and worker images plus
a host-network Compose example with two workers. State is mounted at `/var/lib`; configuration is
flags or environment. A Linux worker needs the same FUSE and namespace privileges as the Phase 2
executor. Phase 6 owns operator reconciliation, identity provisioning, certificates, service
discovery, and rollout policy; this issue supplies the deployable process and container boundary.

## Verification

The portable tests pin protocol round trips, duplicate joining, size-class placement, and one
requeue after lease expiry. The worker fleet proof runs the privileged Linux fixture with a
scheduler and two host-network workers, compares local and remote result digests, kills the leased
worker during an action, and checks the retry counter and second identical result. Static and
shared macOS builds cover the public header and link surface.
