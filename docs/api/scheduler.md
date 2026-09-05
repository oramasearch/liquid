# Scheduler API

The scheduler has an h2c gRPC data listener and an HTTP/1 control listener. The configured REAPI
instance name is validated on every instance-scoped request.

## Standard gRPC services

| Service | Methods |
|---|---|
| `Capabilities` | `GetCapabilities` |
| `ContentAddressableStorage` | `FindMissingBlobs`, `BatchReadBlobs`, `BatchUpdateBlobs`, `GetTree` |
| `ByteStream` | `Read`, `Write`, `QueryWriteStatus` |
| `ActionCache` | `GetActionResult`, `UpdateActionResult` |
| `Execution` | `Execute`, `WaitExecution` |
| `Operations` | `CancelOperation` |

The scheduler advertises BLAKE3 and SHA-256 and execution support. ByteStream resolves either
advertised digest without encoding the algorithm in the resource name. Stock `Execute` requests
enter the same action-digest queue with a medium default size class. Their REAPI Directory graph is
imported into the worker's native input format, with the input root also serving as the toolchain
root. The worker supplies liquid's fixed hermetic values for `PATH`, `HOME`, `TMPDIR`, `LANG`, `TZ`,
and `SOURCE_DATE_EPOCH`; values for those names in a stock Command are ignored. Accepted worker
completions populate ActionCache using the request's digest function.

## Worker protocol

| RPC | Request | Response | Purpose |
|---|---|---|---|
| `Submit` | instance, action digest, scheduling record | REAPI `Operation` | Submit or join one operation. |
| `Lease` | instance, worker TAAP ID, platform, size class | assignment, token, retry, Action, Command, schedule | Claim eligible work. |
| `Heartbeat` | operation, worker, token | acknowledgement | Renew an active lease. |
| `Complete` | operation, worker, token, ActionResult or failure | acknowledgement | Verify and accept a result, or apply bounded requeue. |
| `ReadObject` | native algorithm-tagged digest | bytes | Fetch a native store record with client digest verification. |
| `Status` | empty | queue and worker counters | Inspect scheduler state. |
| `Operation` | operation name | REAPI `Operation` | Poll a liquid submission without holding a transport request through execution. |

The method prefix is `/liquid.scheduler.v1.Scheduler/`. This protocol is private to matching liquid
versions; stock clients use the standard services.

## Control listener

`GET /healthz`, `GET /readyz`, `GET /metrics`, `GET /v1`, `GET /v1/openapi.json`, and `POST /mcp`
are described in [`../library/workers.md`](../library/workers.md). Scheduler metrics are
`liquid_scheduler_queued`, `liquid_scheduler_in_flight`, `liquid_scheduler_completed`,
`liquid_scheduler_failed`, `liquid_scheduler_requeued`, and `liquid_scheduler_workers`.
