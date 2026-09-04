# Distributed execution

> Research: [07-distributed-execution](../research/07-distributed-execution/)

## Position

liquid speaks the **Remote Execution API (REAPI v2)**. Its Action / Command /
Directory Merkle model is correct and proven, and speaking it means liquid works with
BuildBarn, BuildBuddy, NativeLink, EngFlow and Buildfarm on day one instead of
shipping its own farm first.

liquid also ships its own scheduler and workers, because owning them enables things
REAPI does not express. But compatibility comes first.

## Data movement is the cost, not compute

The dominant finding: remote execution parallelises compilation massively, and then
an eager client serialises the download, decompression and filesystem creation of
every intermediate. Large builds generate tens to hundreds of gigabytes nobody looks
at.

**liquid never materialises what has not been read.** Output metadata — digest, size,
type — lives in the graph; bytes arrive on page fault through the virtual filesystem
([01](01-object-model-and-storage.md)). Intermediates stay in the remote CAS, so
consecutive remote actions do not round-trip through the client.

This is Bazel's Build-without-the-Bytes, except it is the only mode rather than an
option, and it reuses the materialiser that already serves checkouts, artifacts and
container filesystems.

## The toolchain is an input

Every compiler, sysroot and tool is in the Merkle input root, addressed by content.

This makes **toolchain drift structurally impossible** rather than an operational
discipline. A platform label like `linux` selects a class of workers, not identical
bytes; mixed compiler patches, sysroots, SDKs and locale databases are the classic
cause of worker-dependent results and poisoned caches.

It also makes cross-compilation and reproducibility fall out of the same mechanism —
and it is what distcc got wrong and icecc, FASTBuild and IncrediBuild each
independently got right.

## Hermeticity is enforced, not requested

Because liquid observes what every action reads ([04](04-build-engine.md)), a
non-hermetic access is detected **where it happens**, with a stack of what read what.
The single loudest complaint about remote execution — "it works locally and fails
remotely" — is a diagnostic, not a mystery.

## Scheduling

- **Dynamic local/remote racing.** Short actions stay local; long ones go remote;
  uncertain ones race and the first result wins.
- **Cost-model-driven, not threshold-driven.** Recorded historical durations decide,
  following BuildBarn's size-class feedback rather than a static rule.
- **Granularity awareness.** Tiny actions lose to RPC, queue and sandbox overhead;
  giant actions destroy parallelism and enlarge invalidation scope. The scheduler
  batches very short work.

## Divergence detection

Phase 2 samples actions onto idle local runtime capacity and compares exit status and every output
digest from a fresh sandbox. Divergence means one of: a non-hermetic action, a compromised worker,
or a genuinely non-deterministic tool. All three are critical. Phase 3 schedules the same replay on
a different worker without changing the comparison or attestation format.

This is the same infrastructure as incremental validation
([04](04-build-engine.md)) and reproducibility verification
([10](10-supply-chain.md)).

## Debuggability

```
liquid replay <action-digest>
```

Reconstructs the exact input tree in the exact sandbox and re-runs it locally. Every
remote failure is reproducible with one command, because the inputs are
content-addressed and still exist.

Captured for every failure: action and command digests, stage timings, worker and
image identity, exit status, streams, retry count and transfer errors. Every existing
system treats this as an afterthought.

## Scaling reality

The bottleneck is the **CAS**, not the workers: blob existence queries over billions
of objects, index size, eviction, replication and GC. liquid's chunk index is designed
for that population from the start ([01](01-object-model-and-storage.md)), and the
CAS is co-located with the workers.

Note also that REAPI does **not** guarantee exactly-once execution — worker loss can
requeue an operation — so actions must be idempotent, which purity already gives.

## Offline

Remote execution is an accelerator. Everything works offline against the local
cache, more slowly. This is an axiom, not a fallback.

## Open questions

- **Whether to extend REAPI or stay strictly compatible.** liquid's finer
  granularity and observed dependencies do not have REAPI expressions.
- **Sandbox technology per platform** — namespaces/seccomp, gVisor, or microVMs, and
  what the equivalent is on macOS and Windows.
- **Remote sampling economics.** Local verification defaults to one in 20 actions; Phase 3 must
  decide how remote capacity changes that policy.
