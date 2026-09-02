# Distributed execution

> Evidence: [`sources.md`](sources.md)

## The protocol worth learning from: REAPI

Bazel's Remote Execution API v2 is build-system-neutral and is the closest thing
to a standard. Its data model is the important part:

- An **`Action`** is content-addressed and references a CAS-resident `Command`
  plus an `input_root_digest`, with a timeout, a `do_not_cache` flag, an optional
  salt, and platform properties.
- A **`Command`** holds argv, *sorted* environment variables, working directory and
  declared output paths.
- A **`Directory`** holds sorted `FileNode`/`DirectoryNode`/`SymlinkNode` entries
  whose digests recursively identify content — a Merkle tree.

Canonical ordering matters because the protobuf bytes are hashed. Identical files
and subtrees deduplicate automatically.

**`FindMissingBlobs`** is the key efficiency primitive: the client hashes its
inputs locally and asks the server which digests are absent, uploading only the
miss set. **`BatchUpdateBlobs`/`BatchReadBlobs`** amortise RPC overhead for many
small blobs; **ByteStream** handles large ones with resumable offsets.

The **ActionCache** maps an *action digest* to an `ActionResult` describing output
digests, exit code and stderr/stdout — deliberately separate from the CAS, which
holds the bytes. This split is what enables Build without the Bytes.

Note the subtleties: REAPI does **not** guarantee exactly-once execution (worker
loss can move an operation from `EXECUTING` back to `QUEUED`), and `Action.timeout`
covers process execution only — not queueing, input staging, or output upload.

## The dominant cost is data movement, not compute

This is the single most important finding.

Remote execution parallelises compilation massively, but an eager client then
serialises network transfer, decompression and local filesystem creation for
*every intermediate artifact*. Large builds generate tens to hundreds of gigabytes
of intermediates that nobody looks at.

**Build without the Bytes** keeps output *metadata* — digest, size, type — in the
build graph without fetching the bytes. `--remote_download_minimal` fetches only
what local actions need; `--remote_download_toplevel` adds the outputs of requested
targets. Bazel 7 made `toplevel` the default; Bazel 7.2's Output Service can
present the whole output tree while materialising lazily on filesystem access.
Reported improvements are commonly **2× or more**, workload dependent.

Keeping intermediates inside the remote CAS also avoids worker → CAS → client →
CAS round trips between consecutive remote actions.

**Known REAPI scaling bottlenecks**, in rough order: hashing and walking enormous
input trees; existence queries over millions of blobs; small-blob RPC and metadata
amplification; CAS index, eviction, replication and GC; worker input staging and
output upload; scheduler queue state and large numbers of open Operation streams;
and client output downloads.

## Server implementations

| Implementation | Notable design |
|---|---|
| **BuildBarn** (Go) | Modular: `bb_storage`, `bb_scheduler`, `bb_worker`, separate `bb_runner`. Platform queues, **resource size classes**, per-invocation fairness/stickiness, and optional feedback-driven size-class selection. |
| **BuildGrid** (Python) | REAPI plus Remote Workers API. Scheduler state in memory or persisted to SQL, enabling multiple service instances. Default `max_execution_timeout` 7,200 s. |
| **BuildBuddy** | Distributed **Sparrow-style scheduler** — randomised candidate selection with late binding, avoiding one globally authoritative scheduler. Pools by OS/arch. Implements remote persistent workers. |
| **EngFlow** | Coordinated scheduler instances; workers also form a replicated **peer-to-peer distributed CAS**. Priority-first, FIFO within priority. Notably *caps* over-large timeouts rather than rejecting them, contrary to spec. |
| **NativeLink** (Rust) | One binary, three roles. Stateless schedulers matching platform properties and queue depth; disposable workers. Rejects over-limit timeouts with `InvalidArgument`. |
| **Buildfarm** (JVM) | Redis as a shared **backplane** for operations, worker registration and queues; idle workers pull from balanced shard queues. |

**Persistent and multiplex workers** are worth understanding: they are *not* base
REAPI semantics. Implementations recognise annotations and route compatible
actions to a long-lived compiler process, which amortises JVM/compiler startup.
A plain persistent worker serialises requests; a *multiplex* worker handles several
concurrently. Bazel multiplexes locally, but remote multiplex support is rare —
several backends document persistent workers and none of the public docs promise
remote multiplex.

## Non-REAPI distributed compilation

- **distcc** preprocesses locally and ships preprocessed source; servers still need
  compatible compilers. **Pump mode** ships source plus discovered headers and
  preprocesses remotely, but assumes matching system headers and can fail on
  computed includes.
- **icecream/icecc** adds the thing distcc lacks: it **packages and ships the
  toolchain** (`icecc-create-env`, `ICECC_VERSION`), so heterogeneous workers use
  the intended compiler. A central scheduler assigns jobs by worker performance and
  architecture.
- **Goma → Siso** (Chromium). Goma was deprecated in early 2024. Siso is a
  Ninja-compatible frontend that — contrary to common belief — *is* REAPI-based,
  using a built-in REAPI client since April 2025. Chromium's convergence on REAPI
  is a meaningful signal.
- **FASTBuild** synchronises the exact toolchain into an isolated remote
  environment. **IncrediBuild** virtualises the initiator's process environment so
  helpers need only the agent.

The consistent lesson: **shipping the toolchain beats requiring identical
installations.** icecc, FASTBuild and IncrediBuild all independently concluded
this; distcc's failure to do so is its main operational weakness.

## What goes wrong in practice

- **Non-hermetic actions that only fail remotely.** Undeclared headers, `/usr`
  tools, `PATH`, `HOME`, locale, clocks, or files left by earlier actions. The
  worse variant: two workers produce *different* outputs for the same action
  digest, poisoning the shared cache for everyone.
- **Toolchain and platform drift.** A platform label like `linux` selects a *class*
  of workers, not identical bytes. Mixed compiler patches, sysroots, SDKs, CPU
  features or locale databases produce worker-dependent results.
- **Debugging remote failures.** You need the action digest, operation name, stage
  timings, pool/worker/image, exit status, stdout/stderr, server logs, retry count
  and CAS transfer errors — or you cannot reproduce anything.
- **Cost.** Idle executors, duplicated dynamic executions, CAS capacity and
  replication, cross-zone traffic, developer egress.

## Action granularity

The central tradeoff, and it is genuinely two-sided:

- **Too fine:** maximum theoretical parallelism and cache reuse, but RPC, queueing,
  sandbox setup and blob overhead dominate. Bazel explicitly warns that remote
  overhead exceeds the benefit for fast actions.
- **Too coarse:** overhead amortises well, but parallelism drops, invalidation
  scope grows, stragglers appear, and workers must be large.

**Dynamic execution** — racing local and remote for the same action and taking the
first winner — is Bazel's pragmatic answer, and it works.

## Implications for liquid

1. **Adopt REAPI's data model; strongly consider wire compatibility.** The
   Action/Command/Directory Merkle model is correct and proven. Speaking REAPI
   means liquid can use BuildBarn, BuildBuddy, NativeLink or EngFlow on day one
   instead of shipping its own farm — an enormous reduction in scope for a project
   this large.

2. **Build without the Bytes is not optional.** Output materialisation is the
   dominant cost. liquid's virtual working copy
   ([02](../02-large-files-and-binary-assets/),
   [16](../16-monorepo-scale-and-authoring-loop/)) is the same machinery: fetch on
   access, never eagerly. One materialiser serves source checkout, artifact
   fetching and remote build outputs.

3. **Ship the toolchain in the input tree.** Put the compiler, sysroot and every
   tool in the Merkle input root, addressed by content. This makes toolchain drift
   *structurally impossible* rather than an operational discipline, and it makes
   cross-compilation and reproducibility fall out of the same mechanism.

4. **Sandbox and observe, so hermeticity is not a user responsibility.** Since
   liquid records what each action actually reads
   ([06](../06-build-and-compiler-caching/)), it can detect a non-hermetic access
   at the moment it happens rather than as a mysterious remote-only failure. This
   is the fix for the loudest complaint in this space.

5. **Defend the shared cache against divergent outputs.** Two workers producing
   different bytes for one action digest must be detected, not silently accepted.
   Periodic re-execution and comparison of sampled actions turns reproducibility
   into a live integrity check ([13](../13-supply-chain-security/)).

6. **Dynamic local/remote racing, with a cost model.** Keep very short actions
   local; send long ones remote; race when uncertain. Use recorded historical
   durations — BuildBarn's size-class feedback is the precedent — rather than a
   static threshold.

7. **Make remote failures debuggable by default.** Capture the full action
   digest, inputs and environment so any remote failure can be replayed locally
   against the identical input tree with one command. This should be a headline
   feature; every existing system treats it as an afterthought.

8. **Watch the CAS, not the workers.** The bottleneck is blob existence queries,
   index size, eviction and GC — not CPU. Design the CAS for billions of small
   objects from the start, and co-locate it with the workers.
