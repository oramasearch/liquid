# Distribution: remote cache, remote execution, and delivery

> Phase 3 of the plan ([`action-plan.md`](https://github.com/oramasearch/liquid-bridge/blob/main/docs/action-plan.md),
> epic https://github.com/oramasearch/liquid/issues/35). Specification:
> [`features/06-distributed-execution.md`](../features/06-distributed-execution.md),
> [`features/05-caching.md`](../features/05-caching.md) (the remote tier),
> [`features/09-artifact-service.md`](../features/09-artifact-service.md) (deltas). This page is the
> design the Phase 3 issues implement. The build-without-bytes, replay, and delta slices are
> implemented. Issue #18 adds cost-model scheduling, batching, and racing through the shared
> transport-neutral remote interfaces. The protobuf messages and native record mapping are
> implemented in [`reapi-codec.md`](reapi-codec.md). Issue #16 connects them to the HTTP/2 client,
> remote cache tier, and liquid CAS/ActionCache server as documented in [`reapi.md`](reapi.md).
> Production workers remain separate work.

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

A remote `ActionResult` names output blobs by digest and size. `build::record_remote_result`
records the result and each output as a version 2 blob record whose bytes are *not yet local*: the
blob carries its whole-content digest plus a `remote` location containing a source id and instance
name. Registering that record persists its chunk boundaries without fetching content.

Bytes arrive on read. The lazy VFS (`vfs.md`) already hydrates chunks from the store on page
fault; the remote location adds a second source behind the same hydration queue, fetched over
`remote_blob_source::read_range` in chunk-sized ranges and written into the local store as ordinary
chunks. The source interface also provides `find_missing` and `write`; it contains no gRPC or
protobuf type, so the unresolved transport choice does not leak into libliquid.
A build whose outputs are never read therefore transfers no output bytes, and a consecutive remote
action that consumes them runs on the backend with the inputs already in its CAS.

Materialisation (`materialise`) of a tree containing remote blobs fetches them then; `liquid
status` shows how many bytes of the current tree are still remote. A chunk stops contributing as
soon as its verified bytes enter the local store. Subsequent reads use those verified chunk bytes
directly, including a one-chunk blob whose content digest is also its chunk digest.

**Hermeticity at the read.** `action_input::remote_eligible` defaults to false until the cost model
in #18 sets it. On Linux, an eligible action runs under the sandbox read guard. The guard inspects
file-open system calls, returns `EACCES` for a path outside the input and toolchain roots, follows
forked processes, and writes the first offending normalized path into the launch report. The
executor returns `errc::unauthorized` with that path before a remote dispatcher can accept the
action. `cache_miss_reason::non_hermetic_read` gives `wrap --explain` and `liquid why` the same
path-bearing vocabulary when scheduling starts using the eligibility flag.

## What the tests taught

The on-disk test source is a second liquid store and counts `read_range` calls and bytes. Recording
an unused remote result performs zero reads. A range crossing three chunk boundaries performs
three reads for exactly those complete chunks; repeating the range performs none, and full
materialisation fetches only the remaining chunks. A second remote result can name a tree
containing the first output without a client read. In the privileged Linux sandbox,
`/etc/hostname` is refused on its first open and is returned as the diagnostic context.

## The toolchain is an input; racing by cost (#18)

Issue #18 adds `remote_executor` beside #17's remote blob contract in `liquid/remote.hpp`:

```cpp
struct remote_blob_location {
  std::string source;
  std::string instance;
};
class remote_blob_source {
  runtime::task<std::vector<digest>> find_missing(std::string instance, std::vector<digest>);
  runtime::task<std::uint64_t> read_range(std::string instance, digest, std::uint64_t,
                                          std::span<std::byte> into);
  runtime::task<void> write(std::string instance, digest, std::vector<std::byte> contents);
};
class remote_executor {
  remote_blob_source& blobs();
  std::string instance() const;
  std::uint64_t queue_depth() const;
  bool supports_batch() const;
  runtime::task<action_result> execute(action_input);
  runtime::task<std::vector<action_result>> execute_batch(std::vector<action_input>);
};
```

The client walks both `action_input.inputs` and `action_input.toolchain`, including tree records,
blob records, and chunks. One `find_missing` call determines the absent objects, and only those
objects are written. The test executor uses a second liquid store as the remote CAS, reconstructs
the source and toolchain into a new directory for every execution, and invokes the tool from that
reconstructed toolchain. No worker image supplies the compiler or test tool. A remote result's
output, stdout, and stderr blob graphs are read back through `read_range` before the signed result
is published locally. Issues #16 and #63 implement network and production-worker versions of these
interfaces without changing the scheduler.

### Cost model

The model is deterministic and every input is preserved in `action_result::schedule`:

- Up to eight earlier results for the candidate supply their five-phase total timings. Their median
  is the local estimate and classifies the action as tiny (at most 25 ms), short (at most 250 ms),
  medium, or long (at least 2,000 ms). No history means a local measurement; no remote object query
  is made.
- Before medium, long, or batched-short dispatch, `find_missing` measures total input bytes, absent
  bytes, and cache locality in permille. The executor reports queue depth and batch support.
- The remote estimate is `20 ms dispatch + 50 ms * queue depth + absent bytes / 100 MiB/s +
  0.75 * median recorded total`.
- Tiny actions always stay local. Short actions stay local unless at least four short actions can be
  sent through one `execute_batch` call. That batch shares one missing-object query and upload.
- With at least three samples, an action is uncertain when the range is greater than half the
  median. Uncertain medium or long work races unless the queue depth exceeds eight, or a queried
  remote is below 250 permille locality while missing more than 64 MiB. Stable work uses the lower
  estimate.

The mixed calibration workload in `BuildDistribution.mixed_workload_selects_local_remote_and_race`
feeds `action_timing` records in the same form produced by the wrap fixtures into the model:

| Work | Recorded totals (ms) | Missing / locality | Queue | Batch | Local estimate | Remote estimate | Decision |
|---|---:|---:|---:|---:|---:|---:|---|
| tiny | 12, 14, 13 | 0 B / 1,000 | 0 | 1 | 13 ms | 29 ms | local |
| short | 80, 100, 90 | 0 B / 1,000 | 0 | 8 | 90 ms | 87 ms | remote batch |
| long | 3,200, 3,400, 3,600 | 8 MiB / 875 | 1 | 1 | 3,400 ms | 2,700 ms | remote |
| uncertain | 400, 900, 2,400 | 4 MiB / 750 | 0 | 1 | 900 ms | 735 ms | race |

`local`, `remote`, and `race`, the timing history, size class, input and missing bytes, query flag,
queue depth, locality, batch capability and size, both estimates, and the reason are stored in
version-3 result records. `liquid why`, `liquid build trace`, `liquid why-slow`, and Chrome trace
events expose those recorded fields.

For a race, missing objects are staged first and local and remote execution then start together.
When either branch produces a result, it is the only value passed to the signing and
cache-publication path.
Capy cancellation reaches the losing task; a losing local sandbox kills its process group, and a
losing remote task receives its stop token. The test double delays the remote side, observes that
cancellation, and proves the action index gains exactly one published row whose output equals an
ordinary local execution.

## Replay (#19)

`liquid replay <action|result>` re-executes an action locally from its recorded inputs, toolchain,
environment and sandbox definition, fetching whatever is remote, and compares outputs byte for
byte with the recorded result. It is the same machinery as `liquid verify` with a different
purpose: verify samples for divergence; replay reproduces a specific failure on demand, including
a cached hit ("replay a hit to check it").

**Implemented.** The target may be a result record directly or an action in the local action index;
an action selects its newest recorded result. A nonzero recorded exit is data, not a replay error:
the command succeeds when the local run has the same exit, stdout, stderr, and declared output
bytes. A difference exits nonzero and identifies the first unequal byte and the recorded and local
values. `--json` emits the same report for tooling.

Replay uses the repository's registered `remote_blob_source` for any recorded remote input or
expected output. The source receives the recorded `remote_blob_location` source and instance;
the store requests ranges and verifies every fetched chunk before local storage. The interface
exposes `find_missing`, range reads, and writes without choosing the Phase 3 transport; the REAPI
client can implement it after #16. Tests use its on-disk double backed by a second liquid store.

Replay does not publish the local result, create a reproducibility attestation, emit a critical
alert, or change the divergent flag. `--record` is the explicit exception: it records the same
signed `reproducibility/1` sample and durable quarantine decision as `liquid verify`.

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

**Decided (Steve, 2026-09-05; bridge decision 9): option 2.** The transport is built as two issues
in parallel, #155 (HTTP/2 client and server with gRPC framing on Corosio) and #156 (protobuf wire
codec and the REAPI v2 message set with the record conversions); #16 integrates them into the
remote tier and the `remote_executor`, and proves conformance against NativeLink and BuildBarn in
Docker; #63 follows. #17, #18, #19 and #64 do not depend on it: they are built against
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
like any other: it is content-addressed and attested. `liquid/delta.hpp` exposes the synchronous
codec plus coroutine-native store operations:

```cpp
result<std::vector<std::byte>> make_delta(delta_algorithm, source, target);
result<std::vector<std::byte>> apply_delta(const delta_record&, source, patch);
runtime::task<delta_creation> create_delta(store&, source_path, target_path, algorithm);
runtime::task<void> apply_delta(store&, record_digest, source_path, destination_path);
runtime::task<delta_fetch_result>
fetch_delta(store&, target_digest, source_path, destination_path);
```

The version-1 `delta_record` is a fixed binary record containing the source digest, target digest,
algorithm id, and digest of the patch blob. Source, target, patch, record, and `delta/1`
attestation are ordinary store objects. The attestation uses the target as its artifact subject and
signs `source`, `target`, `algorithm`, and `delta` (the record digest). The store's subject index is
the local delta catalog. Its reference registry traverses the attestation to the delta record and
the record to source, target, and patch, so a leased attestation retains the whole delivery graph.

`zstd-patch` is algorithm 1. It uses zstd reference-prefix compression at level 19 with long-distance
matching. Apply first verifies the supplied source and patch-blob digests, reconstructs the bytes,
and verifies the target digest before writing a destination. A decompression success is not
sufficient. This final hash is also mandatory on the fetch path.

Fetch queries the local target-subject catalog. Its patch or fallback target can be a blob registered
with the store's configured `remote_blob_source`; the store then range-hydrates and verifies its
ordinary content-defined chunks through #17's transport-neutral interface. The on-disk source in
the test suite supplies those remote blobs. Missing or invalid delta data does not silently become
a trusted target: invalid signatures, catalog/record disagreement, decompression errors, source
mismatches, patch mismatches, and target mismatches fail.

### Executable-aware algorithm reservation

Algorithm 2 is reserved as `executable-sections` behind the same create/apply interface. It is not
emitted yet. The one-round experiment split the Mach-O fixture below into its 15 named sections plus
six gaps, made an independent zstd reference patch for each matching old/new region, and counted a
16-byte envelope plus 24 bytes and the region name per entry. That candidate was 477,119 bytes,
15,164 bytes (3.28%) larger than the 461,955-byte generic patch. It therefore failed the required
measurement. A future ELF/Mach-O implementation must beat 461,955 bytes on this fixture and retain
the same source and final-target verification before algorithm 2 may be emitted.

### Recorded savings

Measured on 2026-09-04 on arm64 macOS with Clang 22.1.8, zstd 1.5.7, and Release builds. The first
binary and compilation database were captured from `origin/main`; the second came from the
consecutive build containing #64. `compile_commands.json` is the large text artifact generated by
those builds.

| Artifact | Source bytes | Target bytes | Delta bytes | Bytes avoided | Saving |
|---|---:|---:|---:|---:|---:|
| `liquid` binary | 7,330,840 | 7,463,480 | 461,955 | 7,001,525 | 93.81% |
| `compile_commands.json` | 461,690 | 457,005 | 3,686 | 453,319 | 99.19% |

The numbers measure bytes needed for the patch blob versus fetching the complete target. The signed
attestation and 108-byte delta record are catalog metadata, excluded from the patch comparison.

## What Phase 3 does not do

No daemon, no incremental engine (Phase 4); no package resolution (Phase 5); no lake-hosted cache
or transparency log (Phase 6); no Windows remote workers.

## How to verify

Each issue adds its proof beside the Phase 2 ones (`build-engine.md`, "How to verify"). The phase
exit is the Phase 2 wrap fixtures built against a stock backend in Docker with the same hit rate
as local, and the non-hermetic fixture refused at the read.

Issue #16's `tools/reapi-peers/run-reapi-interop.sh build-linux` uses pinned NativeLink 1.6.6 and
BuildBarn bb-storage containers. The Make wrap fixture records a 100% local second-run baseline;
after removing only the disk action index, both peer ActionCache implementations produce 100% on
the second run as well:

| Backend | Local second run | Remote second run | CAS and ActionCache round trip |
|---|---:|---:|---|
| NativeLink 1.6.6 | 100% | 100% | pass |
| BuildBarn `20260609T094425Z-10acc76` | 100% | 100% | pass |

The privileged Linux read-guard case refuses `/etc/hostname` before remote dispatch. For the server
direction, stock Bazel 8.3.1 writes a genrule's CAS objects and ActionResult to `liquid serve reapi`;
a clean second output base reports `1 remote cache hit`. The upstream `remote-apis` repository does
not provide a server conformance runner and BuildBarn does not publish a black-box conformance
binary, so this stock Bazel cache read/write case is the applicable upstream-client compatibility
case. Client and server details, configuration, and the exact API map are in
[`reapi.md`](reapi.md).

Issue #19's privileged Linux cases replay a deterministic recorded failure, replay an indexed
cache hit by action digest, detect an altered recorded output at byte zero, and hydrate a missing
input through #17's on-disk source double. The wrap case invokes `liquid replay --json` against a
recorded action. macOS static and shared builds compile the API and CLI while mount-backed cases
skip when macFUSE is unavailable.

## How to change

This page tracks design and implementation status together. A Phase 3 issue that finds the design
wrong changes this page in the same commit as the code and says why in the commit body, as the Phase
2 issues did for `build-engine.md`.
