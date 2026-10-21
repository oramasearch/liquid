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
queue entry. Liquid clients poll that operation with bounded unary requests, so worker loss and
lease expiry do not depend on one transport request remaining open. Stock clients retain REAPI
`WaitExecution`.

A worker polls `Lease` with its platform, maximum size class, and TAAP identifier, signed by its
key. Placement requires an exact OS and architecture match. A worker may accept its own size class
or a smaller one, and the scheduler selects the largest eligible queued action first. The lease
contains the encoded Action, Command, scheduling record, a random 128-bit token, and the retry
count, and is signed by the scheduler (see *Authentication* below).

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
provenance, and both SBOM forms with the same identity that signs its requests. The scheduler
rejects a completion whose operation, token, worker, scheduling record, or signature differs from
the active lease, and a result signed by any key other than the leased worker's pinned key. The client imports those exact signed records and verifies provenance and SBOMs; it does not
replace the worker signature with a client signature. `liquid why` therefore shows the same
history, size class, estimates, and reason that drove placement.

## Authentication

The October 2026 security review found that the scheduler accepted any self-declared worker
identifier and checked a result's signature only against the key the result itself named, and that
leases were unsigned with sequential tokens (finding L-34,
[liquid #416](https://github.com/oramasearch/liquid/issues/416)). The scheduler and the workers now
authenticate each other with liquid's own TAAP identities; there is no TLS listener and no
certificate authority to run.

**Pinned identities.** Each process signs with the identity in its `LIQUID_HOME`. Each side is
configured with the other's identity document, the base64url output of
`liquid identity show --document`:

- the scheduler with every worker it trusts (`--trust-worker <path>`, repeatable; a directory
  contributes each file not starting with a dot; `LIQUID_TRUSTED_WORKERS` names one path);
- the worker with its scheduler (`--trust-scheduler <path>`, or `LIQUID_TRUSTED_SCHEDULER`).

A document is accepted only if its self-signature verifies. Neither process starts without its
trust configuration: a scheduler with no trusted worker and a worker with no trusted scheduler exit
with a usage error.

**Worker requests.** `Lease`, `Heartbeat`, and `Complete` (including a failure report) carry a
fresh 128-bit nonce, the issue time in Unix milliseconds, and a TAAP signature over the message
encoded with an empty signature field. Each has its own signature purpose
(`liquid-worker-lease-request`, `liquid-worker-heartbeat`, `liquid-worker-complete`), so one
cannot be replayed as another. The scheduler looks up the pinned key of the worker the request
names and verifies the signature with exactly that key; it never uses a key the request carries
and never consults its own identity store. It refuses, with gRPC `PERMISSION_DENIED`:

- a worker identifier that is not trusted;
- a missing, malformed, or wrong signature, including a valid signature by another key;
- an issue time more than five minutes from the scheduler's clock;
- a nonce it has already seen from that worker within twice that window.

Only an authenticated worker is counted as live for `/readyz` and placement.

**Leases.** The scheduler signs every `Lease` response, assigned or idle, with purpose
`liquid-scheduler-lease`. The response names the worker it answers and echoes that request's
nonce. The worker verifies the signature against the pinned scheduler key and checks the worker
and nonce before it reads anything else; an unsigned, foreign, tampered, or replayed lease stops
the worker with an `unauthorized` error rather than running anything. Lease tokens are 128 bits
from the operating system's random source, so a token cannot be guessed, and a request that holds
a token is still refused unless it is signed by the leased worker.

**Results.** A completion is accepted only when the result's producer key is the leased worker's
pinned key and the result signature verifies with that key. A result that names the worker's
identifier with another key is refused.

**Rotation.** A pinned document names one key. After `liquid identity rotate`, the other side needs
the new document; until it has it, requests (or leases) signed by the new key are refused.

**Not covered.** The standard REAPI services (CAS, ByteStream, ActionCache, Execution) and the
private `Submit`, `ReadObject`, `Status`, and `Operation` methods remain unauthenticated, as
before: CAS and `ReadObject` content is digest-verified by the reader, and client access control
is the operator's network boundary. Acknowledgements to `Heartbeat` and `Complete` are not signed;
a forged acknowledgement can only make a worker believe its result was accepted or its lease ended.
The transport is h2c, so requests and leases are authenticated but not confidential.

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
  --trust-worker /etc/liquid/workers \
  --control-listen 0.0.0.0:8080 \
  --store /var/lib/liquid-scheduler \
  --instance builds

liquid worker --scheduler scheduler.example:50051 \
  --trust-scheduler /etc/liquid/scheduler \
  --control-listen 0.0.0.0:8081 \
  --root /var/lib/liquid-worker \
  --instance builds \
  --size-class long

liquid wrap --scheduler scheduler.example:50051 --remote-instance builds make -j8
```

The scheduler accepts `LIQUID_SCHEDULER_LISTEN`, `LIQUID_CONTROL_LISTEN`, `LIQUID_INSTANCE`,
`LIQUID_SCHEDULER_STORE`, `LIQUID_SCHEDULER_LEASE_MS`, and `LIQUID_TRUSTED_WORKERS`. The worker
accepts `LIQUID_SCHEDULER`, `LIQUID_TRUSTED_SCHEDULER`, `LIQUID_CONTROL_LISTEN`,
`LIQUID_INSTANCE`, `LIQUID_WORKER_ROOT`, `LIQUID_WORKER_SIZE_CLASS`, and `LIQUID_WORKER_POLL_MS`.
Both use `LIQUID_HOME` for their own identity. Flags override environment values;
`--trust-worker` adds to `LIQUID_TRUSTED_WORKERS`. Both
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

`src/serve/` owns the bounded HTTP/1.1 parser, asynchronous route table, and Corosio TCP and Unix
accept loops used by long-running liquid components. `src/workers/control.cpp` registers the fixed
scheduler and worker handlers in that table; their routes and response bodies are unchanged.

## Deployment shape

[`../../deploy/workers/`](../../deploy/workers/) contains separate scheduler and worker images plus
a host-network Compose example with two workers. State is mounted at `/var/lib`, each identity at
`/var/lib/liquid-identity`, and the pinned documents at `/etc/liquid`; configuration is flags or
environment. A Linux worker needs the same FUSE and namespace privileges as the Phase 2 executor.
Phase 6 owns operator reconciliation, identity provisioning and rotation, service discovery, and
rollout policy; this issue supplies the deployable process and container boundary.

## Verification

The portable tests pin protocol round trips, duplicate joining, size-class placement, and one
requeue after lease expiry. `WorkerAuthentication.*` pins the authentication: an untrusted worker,
a trusted identifier with another key, an unsigned request, a replayed or stale request, a guessed
token, and a result signed by another key are each refused, and the worker refuses an unsigned,
foreign, tampered, or replayed lease. `cli-workers-trust` checks that neither command starts
without its trust configuration and that a directory of documents loads. The worker fleet proof runs the privileged Linux fixture with a
scheduler and two host-network workers, compares local and remote result digests, kills the leased
worker during an action, and checks the retry counter and second identical result. Static and
shared macOS builds cover the public header and link surface.
