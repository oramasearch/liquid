# Distribution: remote cache, remote execution, and delivery

> Phase 3 of the plan ([`action-plan.md`](https://github.com/oramasearch/liquid-bridge/blob/main/docs/action-plan.md),
> epic https://github.com/oramasearch/liquid/issues/35). Specification:
> [`features/06-distributed-execution.md`](../features/06-distributed-execution.md),
> [`features/05-caching.md`](../features/05-caching.md) (the remote tier),
> [`features/09-artifact-service.md`](../features/09-artifact-service.md) (deltas). This page is the
> design the Phase 3 issues implement; it is written before them and corrected by them.

Phase 2 built the local action path: records for actions and results
([`build-engine.md`](build-engine.md)), observation by the mount, the signed disk cache, `liquid
wrap`, verification, provenance, policy, `why`, traces, facts, retention and SBOMs. Phase 3 makes
the same records travel: a result can be produced on another machine, its outputs can stay there
until read, and a release can be delivered as a delta. Nothing in this phase changes what an
action or a result *is*.

**Exit for the phase:** a Phase 2 build runs against a stock REAPI backend with the same cache-hit
rate as local, and a deliberately non-hermetic action is caught at the read, not at the remote
failure.

## What already lines up with REAPI

The Phase 2 records were shaped to make this mapping mechanical:

| liquid | REAPI v2 | Note |
|---|---|---|
| `action_input` (command, cwd, environment, declared outputs, inputs tree digest, toolchain digest, platform, timeout) | `Action` + `Command` | `Command.platform` carries liquid's `platform` plus the sandbox definition digest as a property, so a backend never mixes results from different sandboxes. |
| a liquid tree record | `Directory` | Converted on demand into the Merkle form REAPI expects; the conversion is content-addressed and cached, so a subtree that did not change is not re-encoded. Modes map 1:1 (`0100644`, `0100755`, `0120000`, `040000`). |
| the toolchain tree | part of the input root | Exactly as `features/06` requires: the compiler is an input. |
| `action_result` (exit, outputs, stdout/stderr blobs, manifest, timing, worker) | `ActionResult` | The observed-input manifest has no REAPI field; it travels as auxiliary metadata (`ActionResult.execution_metadata.auxiliary_metadata`) and is also stored locally, so `why`, verification and policy keep working on remote results. |
| result signature, `slsa_provenance/1`, `reproducibility/1`, `sbom/1` | none | Attestations stay in liquid's store; a remote result is signed by the worker's identity when the worker is liquid's own (#63) and by the client on receipt when it is a stock backend, with `builder` naming the backend. |
| candidate key and manifest validation | `ActionCache` lookup by action digest | The remote cache is a third tier behind the disk tier, consulted with the same candidate key; validation of the manifest against the current tree happens locally, exactly as for the disk tier. |
| chunk store | `ContentAddressableStorage` + `ByteStream` | See "digest functions" below. |

**Digest functions.** REAPI v2.3 lists BLAKE3 among its digest functions; liquid's default is
BLAKE3 with SHA-256 available through hash agility (`hash-agility.md`). A backend advertises what it
accepts in `GetCapabilities`; liquid uses BLAKE3 when the backend offers it and otherwise computes
SHA-256 digests for the REAPI view of the same objects (the store keeps both, per-algorithm run
sets already exist). A result cached remotely under SHA-256 is still indexed locally under the
BLAKE3 candidate key.

## Build without the bytes, as the only mode (#17)

A remote `ActionResult` names output blobs by digest and size. liquid records the result and each
output as a blob record whose bytes are *not yet local*: the blob carries a location (`remote`,
the backend and instance name) beside its digest. Nothing is fetched at that point.

Bytes arrive on read. The lazy VFS (`vfs.md`) already hydrates chunks from the store on page
fault; the remote location adds a second source behind the same hydration queue, fetched over
`ByteStream.Read` in chunk-sized ranges and written into the local store as ordinary chunks.
A build whose outputs are never read therefore transfers no output bytes, and a consecutive remote
action that consumes them runs on the backend with the inputs already in its CAS.

Materialisation (`materialise`) of a tree containing remote blobs fetches them then; `liquid
status` shows how many bytes of a tree are still remote.

**Hermeticity at the read.** The mount observes every read an action makes. A read outside the
input tree (an absolute path on the host, an undeclared environment file) is recorded in the
manifest today and makes the result non-reproducible elsewhere. In Phase 3 that read is a
*failure at the read*: when an action is eligible for remote execution, the sandbox refuses reads
outside the input tree and the toolchain tree, and the executor reports the path before anything
is dispatched. The non-hermetic action from the exit criterion is caught on the first local run,
never as a confusing remote error.

## The toolchain is an input; racing by cost (#18)

The toolchain tree is materialised on a remote worker from the CAS like any other input; there is
no image to bake. For stock backends this is only a size question (a toolchain is hundreds of
megabytes once, then a cache hit); for liquid's own workers (#63) the CAS is co-located so it is a
local hydration.

Scheduling races local against remote when the cost model is uncertain. The inputs of the model
are all recorded already: the action's own history (`result.timing` across previous runs, the
size class it fell into), the bytes the remote side lacks (`FindMissingBlobs` before dispatch),
queue depth reported by the backend, and cache locality. The model's output is a decision
(`local`, `remote`, `race`) and its inputs are recorded on the result so `liquid why` explains the
choice. Tiny actions never leave the machine; very short work is batched into one remote action
when the backend supports it.

## Replay (#19)

`liquid replay <action|result>` re-executes an action locally from its recorded inputs, toolchain,
environment and sandbox definition, fetching whatever is remote, and compares outputs byte for
byte with the recorded result. It is the same machinery as `liquid verify` with a different
purpose: verify samples for divergence; replay reproduces a specific failure on demand, including
a cached hit ("replay a hit to check it").

## The transport decision (#16)

REAPI is defined as gRPC: HTTP/2 framing, protobuf messages, and the `google.longrunning`
operations pattern for execution. liquid's networking is Corosio, and the org's HTTP library
(`oramasearch/http`) is HTTP/1 only. Three ways to speak REAPI:

1. **Depend on `grpc++` and `protobuf`.** Fastest to first contact; brings a very large
   dependency with its own threading model that does not sit on Capy and Corosio, and drags a
   code generator into every build of libliquid.
2. **Own HTTP/2 client on Corosio, own protobuf codec for the REAPI message set.** REAPI uses
   about twenty message types, ByteStream, Capabilities, ActionCache, ContentAddressableStorage,
   Execution and Operations; protobuf wire encoding is small and stable. HTTP/2 client framing
   (HPACK, flow control, streams) is the larger piece, on the order of the chunk index in size.
   Everything stays coroutine-native and behind the runtime boundary, and the same HTTP/2 layer
   serves lake later.
3. **A sidecar proxy** that speaks gRPC on liquid's behalf. Adds a process and a second
   language; compatibility "unchanged" with stock backends would depend on it being present.

**Recommendation: option 2**, consistent with decisions 5, 7 and 8 in `CLAUDE.md` (Capy and
Corosio everywhere, libliquid as the product with Boost behind the boundary, storage engines built
from scratch rather than adopted). It is more work up front and it is the option that leaves
liquid owning its transport, which #63 (liquid's own scheduler and workers) needs anyway. The
conformance proof is a real backend: the tests run NativeLink and BuildBarn in Docker and drive
the Phase 2 fixture builds against them.

This decision is Steve's; it is recorded in the bridge's `docs/decisions.md` when made, and #16
and #63 do not start before it. #17, #18, #19 and #64 do not depend on it: they are built against
the local executor and the store, with the remote location abstracted behind one interface
(`remote_blob_source`: `find_missing`, `read_range`, `write`) that the REAPI client implements
later.

## liquid's own scheduler and workers (#63)

Compatibility first; ownership second, because REAPI cannot express observed dependencies, finer
granularity or liquid's attestations. The worker is the Phase 2 executor plus a client of the
scheduler; the scheduler is a queue keyed by action digest with idempotent requeue (a worker
killed mid-action is requeued once and the output is identical, because the action is
deterministic by construction). The CAS is the liquid store on the worker nodes, co-located, and
results are signed by the worker's TAAP identity. Deployment is the operator's (Phase 6).

## Delta encoding for delivery (#64)

Content-defined chunking is the baseline for everything; deltas are an optimisation for
generate-once, apply-many cases: published releases and OTA-style updates. A delta is an artifact
like any other: it is content-addressed, attested (`delta/1` names source, target and algorithm),
and a client that has the source fetches the delta and falls back to chunks when none exists.
`zstd --patch-from` is the generic algorithm; executable-aware deltas (section-aware, relocation-
aware) are reserved for binaries and land behind the same interface. Measured savings go into this
page when the issue lands; they are the reason the feature exists.

## What Phase 3 does not do

No daemon, no incremental engine (Phase 4); no package resolution (Phase 5); no lake-hosted cache
or transparency log (Phase 6); no Windows remote workers.

## How to verify

Each issue adds its proof beside the Phase 2 ones (`build-engine.md`, "How to verify"). The phase
exit is the Phase 2 wrap fixtures built against a stock backend in Docker with the same hit rate
as local, and the non-hermetic fixture refused at the read.

## How to change

This page precedes its implementation. A Phase 3 issue that finds the design wrong changes this
page in the same commit as the code and says why in the commit body, as the Phase 2 issues did for
`build-engine.md`.
