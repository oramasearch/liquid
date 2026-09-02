<!-- Raw research source. Generated via Perplexity `perplexity_research` (web-grounded,
     multi-source) on 2026-09-01 for the liquid project. Lightly reformatted only.
     Treat as evidence, not as liquid design decisions - see README.md in this
     directory for the curated analysis and the implications drawn for liquid. -->

# Remote execution API and distributed build backends, 2024-2026

**Status as of 2026-09-02.** REAPI is a build-system-neutral execution-and-cache protocol; it does not itself prescribe a scheduler, sandbox, storage topology, retry policy, or worker implementation.

| Area | Item | Concise technical findings |
|---|---|---|
| **1. REAPI v2** | **Data model: `Action`, `Command`, `Directory`** | An `Action` is content-addressed and references a CAS-resident `Command` plus `input_root_digest`; it also carries execution `timeout`, `do_not_cache`, optional `salt`, and-in current v2-the preferred `platform`. `Command` specifies argv, sorted environment variables, working directory and declared output paths. A `Directory` contains sorted `FileNode`, `DirectoryNode`, and `SymlinkNode` entries whose digests recursively identify content. Canonical ordering is essential because protobuf bytes are hashed. [web:151][web:152] |
| **1. REAPI v2** | **Merkle-tree input root** | `Action.input_root_digest` identifies the root serialized `Directory`; child directories are separate CAS blobs referenced by digest, while files reference content blobs. The root, every descendant `Directory`, all files, `Command`, and normally `Action` must exist before `Execute`. Identical files/subtrees are naturally deduplicated. Do not confuse this with `Tree`, a packed root-plus-children representation mainly used for output directories; an input root is ordinarily the graph of individually addressed `Directory` protos. |
| **1. REAPI v2** | **CAS and `FindMissingBlobs`** | `ContentAddressableStorage` stores arbitrary bytes by `(hash,size)`. `FindMissingBlobs` accepts many digests and returns only absent ones, allowing a client to hash its inputs but upload only the CAS miss set. Other principal CAS RPCs are `BatchUpdateBlobs`, `BatchReadBlobs`, and paginated/streaming `GetTree`; recent revisions also define optional split/splice support. Efficient `FindMissingBlobs` requires an indexed existence store-BuildGrid explicitly identified indexed CAS lookup and cleanup as performance work. [web:136][web:150] |
| **1. REAPI v2** | **Batch versus streaming transfer** | Use `BatchUpdateBlobs`/`BatchReadBlobs` to amortize RPC overhead for many small blobs; each member has an independent status. Use Google `ByteStream.Write`/`Read` for large blobs: client-streamed writes use an upload UUID and offsets and can be resumed; reads are server-streamed and support offsets. Batch limits come from `CacheCapabilities.max_batch_total_size_bytes`; there is **no universal REAPI 4-MiB cutoff**, although configured gRPC/message limits still apply. Zstd-compressed blob resource names are supported when advertised. [web:151][web:159][web:160] |
| **1. REAPI v2** | **`ActionCache`** | The AC maps an **Action digest**, not an output digest, to `ActionResult`. `GetActionResult` returns output-file/directory digests, exit code, stdout/stderr and metadata; `UpdateActionResult` publishes one after referenced data has been stored in CAS. Consequently, an AC entry can exist while referenced CAS data has been evicted-robust services validate or extend blob lifetime before returning a hit. [web:19][web:20] |
| **1. REAPI v2** | **Execution and long-running Operations** | `Execution.Execute` is a server-streaming RPC returning `google.longrunning.Operation`. Metadata reports `CACHE_CHECK`, `QUEUED`, `EXECUTING`, and `COMPLETED`; the final operation embeds `ExecuteResponse`/`ActionResult`. A disconnected client resumes with `WaitExecution` or, where implemented, `GetOperation`. Worker loss may move an operation from `EXECUTING` back to `QUEUED`; REAPI does not guarantee exactly-once execution. Setup failures are RPC errors, while errors after operation creation belong in `ExecuteResponse.status`, not `Operation.error`. [web:151] |
| **1. REAPI v2** | **Platform properties and timeouts** | `Platform.Property{name,value}` is server-defined and commonly selects OS, architecture, container image, pool, CPU/RAM or special hardware. Properties are sorted and included in the Action digest; since v2.2 `Action.platform` is preferred and `Command.platform` is deprecated. `Action.timeout` covers process execution-not queueing, input staging, output upload, or the client's `--remote_timeout`. The specification says a request above the server maximum must be rejected. |
| **1. REAPI v2** | **Known scaling bottlenecks** | The dominant pressure points are: (a) hashing/walking enormous input trees and existence queries for millions of blobs; (b) small-blob RPC and metadata amplification; (c) CAS index, eviction, replication, GC and hot-object load; (d) worker input staging and output upload; (e) scheduler queue/fairness state and large numbers of open Operation streams; and (f) client output downloads. Actions with huge input sets can spend far longer in "upload missing inputs," while very short actions cannot amortize queue/RPC/sandbox latency. [web:144][web:165][web:359] |
| **2. Servers** | **Persistent/multiplex distinction** | Persistent workers are **not a base REAPI-v2 execution semantic**: implementations recognize Bazel annotations/platform properties, retain tool inputs and route compatible actions to a long-lived compiler process. A normal persistent process handles requests serially; a *multiplex* worker handles several request IDs concurrently. Bazel supports multiplexing locally, but remote backends must explicitly implement compatible routing, isolation and protocol handling-support for "remote persistent workers" does not automatically prove remote multiplex support. [web:74][web:84][web:88] |
| **2. Servers** | **BuildBarn** | Open-source, modular Go services: `bb_storage`, an in-memory `bb_scheduler`, `bb_worker`, and separate `bb_runner`. The scheduler uses platform queues, resource **size classes**, per-invocation fairness/stickiness, retry and optional feedback-driven initial size-class selection; workers synchronize/pull work. Default/maximum execution timeouts are configured through the action router and validated against `Action.timeout`. Remote persistent workers remained an explicit implementation proposal/issue rather than established support; therefore no production multiplex claim. [web:46][web:47][web:50][web:60] |
| **2. Servers** | **BuildGrid** | Python REAPI plus Remote Workers API implementation. Its Execution service validates/enqueues jobs; configurable assigners select property-matching bots. Scheduler state can be in memory or externally persisted in SQL, enabling multiple service instances. The documented default `max_execution_timeout` is 7,200 seconds, with separate queue-timeout machinery. Workers commonly use BuildBox components. No native remote persistent/multiplex facility is documented in the cited architecture. [web:61][web:63][web:64][web:69][web:92] |
| **2. Servers** | **BuildBuddy** | Open-core/managed platform using a distributed Sparrow-style scheduler with randomized candidate selection and late binding, avoiding a single globally authoritative scheduler. Executors may be divided by `Pool`, OS and architecture, including self-hosted/specialized pools. It implements remote persistent workers using `persistentWorkerKey`, tool-input marking, and proto/JSON worker protocols; public documentation does not promise remote multiplex concurrency. Action execution can use an Action timeout or `default-timeout`; `termination-grace-period` controls TERM-to-KILL delay. [web:107][web:288][web:298][web:299] |
| **2. Servers** | **EngFlow** | Commercial clustered service with coordinated scheduler instances and worker instances that provide several one-action-at-a-time executors. Named pools contain equivalent executors; scheduling is highest priority first and FIFO within a priority. Workers also form a replicated, peer-to-peer distributed CAS. Remote persistent workers are supported with tool-input annotations, `persistentWorkerKey`, and reusable local/Docker environments; Linux/macOS support was enabled by default by 2026. `--max_execution_timeout` defaults to 15 minutes and is also the default for actions without a timeout; EngFlow documents silently capping larger requested values, a notable difference from REAPI's prescribed rejection. [web:121][web:122][web:124][web:130][web:301][web:302][web:306] |
| **2. Servers** | **NativeLink (Rust)** | Cache, scheduler and worker roles are one Rust binary configured differently. Schedulers are designed as stateless dispatchers matching platform properties, queue depth and routing rules; disposable workers fetch inputs, sandbox execution and upload outputs. By 2026 it documents pooled persistent workers driven by action properties, but not general remote multiplex concurrency. Worker `max_action_timeout_s` defaults to 20 minutes: absent Action timeouts receive that value, while requests above it are rejected with `InvalidArgument`, rather than clamped. [web:31][web:32][web:39][web:91][web:271] |
| **2. Servers** | **Buildfarm** | Open-source JVM implementation using Redis as a shared **backplane** for operations, worker registration and queues; idle workers pull from balanced Redis shard queues. It supports scalable server/shard-worker deployments and configurable requeue behavior. The current configuration documents a 600-second default and 3,600-second maximum Action timeout. Remote-persistent-worker code landed in the repository in late 2025, but remote multiplex behavior is not documented as a general feature. [web:76][web:77][web:79][web:272][web:276] |
| **3. Build without the Bytes** | **`minimal` and `toplevel`** | BwoB keeps remote output **metadata**-digest, size and type-in Bazel's output graph without eagerly fetching every byte. `--remote_download_minimal` downloads only outputs needed by local actions and commands such as `bazel run`; `--remote_download_toplevel` additionally materializes outputs of requested top-level targets/output groups. Bazel 7 changed the default from `all` to `toplevel`; Bazel 7.2's Output Service can expose the whole output tree while materializing content lazily on filesystem access. [web:1][web:4][web:8][web:137] |
| **3. Build without the Bytes** | **Why materialization dominates** | Remote execution can make compilation massively parallel, but an eager client still serializes network transfer, decompression and local filesystem creation for every intermediate. Large builds can generate tens or hundreds of gigabytes, making bandwidth and filesystem work-not worker CPU-the wall-time bottleneck. Keeping intermediates inside the remote CAS also avoids worker->CAS->client->CAS round trips between consecutive remote actions. Bazel reported common 2x-plus improvements from suppressing those downloads, workload dependent. [web:137][web:149][web:177] |
| **4. Distributed compilation** | **distcc / pump** | Plain distcc runs preprocessing locally and sends preprocessed C/C++ plus compiler arguments to `distccd`, so servers still need compatible compiler binaries. Pump mode runs an incremental Python include server, ships source and recursively discovered non-system headers, and performs preprocessing remotely. It can greatly reduce client preprocessing load, but assumes matching server/client system headers, stable sources during the build, and can fail on difficult computed includes. It has no general REAPI Merkle CAS or arbitrary-action execution. [web:196][web:197][web:198][web:200] |
| **4. Distributed compilation** | **icecream / icecc** | A central `icecc-scheduler` assigns compile jobs to `iceccd` daemons and considers worker performance/architecture. Unlike ordinary distcc, icecream can package and ship compiler environments/toolchains (`icecc-create-env`, `ICECC_VERSION`) so heterogeneous workers use the intended compiler; multiple native/cross toolchains can coexist. It is compile-specific rather than a general action/CAS protocol, and the central scheduler is a coordination dependency. [web:211][web:212][web:222][web:225] |
| **4. Distributed compilation** | **Goma and Siso in Chromium** | Goma used `gomacc` plus a local `compiler_proxy` to discover inputs, submit compile requests and use a compile-result cache; its later open server was itself a proxy onto a REAPI backend. Chromium announced Goma removal/deprecation in early 2024 and now marks it unmaintained. Siso is a Ninja-compatible build frontend, **not actually non-REAPI**: it became default on Chromium gLinux in November 2024, was used for official builders from July 2024, and by April 2025 used its built-in REAPI client rather than Reclient. [web:181][web:185][web:188][web:191][web:226][web:235] |
| **4. Distributed compilation** | **FASTBuild** | A complete build system with distributed compilation and object caching. `FBuildWorker` instances advertise through a shared brokerage path (or explicit worker list); eligible compile tasks are sent to available workers. FASTBuild synchronizes the exact compiler toolchain into an isolated remote environment, reducing installation drift, and can separately read/write its object cache. It is compiler/build-graph specific rather than a standard REAPI service. [web:241][web:243][web:246] |
| **4. Distributed compilation** | **IncrediBuild** | Commercial process-distribution system: one Coordinator tracks Agents and gives Initiator Agents idle Helper capacity. Its process virtualization mirrors the initiator's files/environment and returns generated outputs, so Helpers generally need only the Agent-not source, compiler or application installation. This is broader than compiler wrapping but uses proprietary orchestration/virtualization rather than REAPI's Action/CAS model. [web:257][web:258][web:262][web:269] |
| **5. Operational pain** | **Non-hermetic, remote-only failures** | Local actions often read undeclared headers, `/usr` tools, `PATH`, `HOME`, locale, clocks, network services or files left by earlier actions. A remote sandbox exposes the omission, or worse, two workers produce different outputs for the same Action digest and poison the shared cache. Use declared inputs, deterministic environment variables, strict local sandboxing and Bazel toolchain rules; do not discover compilers through `PATH`/`JAVA_HOME`. [web:331][web:336][web:337][web:342] |
| **5. Operational pain** | **Toolchain and platform drift** | A platform label such as `linux` or mutable container tag selects a class of workers but does not prove identical bytes. Mixed compiler patches, sysroots, SDKs, CPU features or locale databases cause worker-dependent failures and cache-unsound results. Put the toolchain/sysroot in the Merkle input tree or use an immutable container-image digest; include every result-affecting worker property in the Action platform and rotate/salt cache keys when infrastructure semantics change. |
| **5. Operational pain** | **Flaky workers and debugging** | Preserve the Action/Command digests, Operation name and stage timings, selected pool/worker/image, exit status, stdout/stderr, `ExecuteResponse.server_logs`, retry count and CAS-transfer errors. Retry `UNAVAILABLE`/worker-loss failures cautiously; repeated `INTERNAL`, digest mismatch or one-worker-only failures should quarantine the worker rather than be hidden indefinitely. Reproduce using the exact CAS input root in the same image; locally use `--verbose_failures --sandbox_debug`, and compare Bazel execution logs across clean/local/remote runs to identify changing action keys or inputs. [web:316][web:318][web:332] |
| **5. Operational pain** | **Cost and action granularity** | Costs include provisioned/idle executors, duplicated dynamic executions, scheduler/control-plane load, CAS capacity/replication/GC, cross-zone traffic and developer egress. BwoB and co-locating CAS/workers reduce bytes; autoscaling and resource/size-class pools reduce idle and overprovisioned CPU/RAM. Granularity is a balance: tiny actions maximize theoretical parallelism/cache reuse but lose to RPC, queue, sandbox and blob overhead; giant actions amortize overhead but reduce parallelism, enlarge invalidation scope, create stragglers and require large workers. Keep sub-millisecond/very short work local or batch it, while splitting long independent work enough to saturate the farm. Bazel explicitly warns that remote overhead can dominate fast actions and supports dynamic local/remote racing for this reason. [web:346][web:354] |

## Citations

[1] https://blog.bazel.build/2023/10/06/bwob-in-bazel-7.html
[4] https://bazel.build/reference/command-line-reference
[8] https://developers.googleblog.com/bazel-7-release/
[19] https://android.googlesource.com/platform/external/bazelbuild-remote-apis/+/e0e5a5351a4571f63506eec04ee49a3dd1e60327/build/bazel/remote/execution/v2/remote_execution.proto
[20] https://www.buildbuddy.io/blog/bazels-remote-caching-and-remote-execution-explained/
[31] https://docs.nativelink.com/remote-execution
[32] https://docs.nativelink.com/explanations/worker-execution
[39] https://docs.nativelink.com/explanations/architecture
[46] https://github.com/buildbarn/bb-remote-execution/blob/master/cmd/bb_scheduler/main.go
[47] https://github.com/buildbarn/bb-remote-execution/blob/master/pkg/proto/configuration/bb_scheduler/bb_scheduler.proto
[50] https://github.com/buildbarn/bb-remote-execution/blob/master/README.md
[60] https://github.com/buildbarn/bb-remote-execution/issues/112
[61] https://buildgrid.build/api/buildgrid.server.scheduler.html
[63] https://buildgrid.build/api/buildgrid.server.scheduler.impl.html
[64] https://buildgrid.build/api/buildgrid.server.scheduler.queue_timeout.html
[69] https://buildgrid.build/operation/architecture.html
[74] https://github.com/bazelbuild/proposals/blob/main/designs/2021-03-06-remote-persistent-workers.md
[76] https://github.com/bazelbuild/bazel-buildfarm/issues/294
[77] https://github.com/buildfarm/buildfarm/issues/351
[79] https://buildfarm.github.io/buildfarm/docs/configuration/configuration/
[84] https://bazel.build/remote/multiplex
[88] https://github.com/bazelbuild/proposals/blob/main/designs/2021-03-06-remote-persistent-workers.md
[91] https://docs.nativelink.com/
[92] https://github.com/antmicro/buildgrid
[107] https://www.buildbuddy.io/docs/rbe-platforms/
[121] https://docs.engflow.com/re/config/pools.html
[122] https://docs.engflow.com/re/index.html
[124] https://docs.engflow.com/re/client/remote-execution-priority.html
[130] https://docs.engflow.com/re/config/options.html
[136] https://android.googlesource.com/platform/external/bazelbuild-remote-apis/+/e0e5a5351a4571f63506eec04ee49a3dd1e60327/build/bazel/remote/execution/v2/remote_execution.proto
[137] https://blog.bazel.build/2024/07/23/remote-output-service.html
[144] https://github.com/bazelbuild/remote-apis/issues/178
[149] https://blog.bazel.build/2019/05/07/builds-without-bytes.html
[150] https://github.com/antmicro/buildgrid
[151] https://github.com/bazelbuild/remote-apis/blob/main/build/bazel/remote/execution/v2/remote_execution.proto
[152] https://github.com/bazelbuild/remote-apis
[159] https://github.com/bazelbuild/remote-apis/issues/178
[160] https://github.com/bazelbuild/remote-apis/issues/212
[165] https://github.com/bazelbuild/remote-apis/issues/252
[177] https://bazel.build/versions/7.4.0/advanced/performance/build-performance-breakdown
[181] https://chromium.googlesource.com/build/+/refs/heads/main/siso/README.md
[185] https://chromium.googlesource.com/infra/infra/go/src/infra/+/792693d8ab21/build/siso
[188] https://chromium.googlesource.com/chromium/src/+/lkgr/build/toolchain/rbe.gni
[191] https://groups.google.com/a/chromium.org/g/chromium-dev/c/rajt7THxIng
[196] https://www.distcc.org/man/pump_1.html
[197] https://www.distcc.org/man/distcc_1.html
[198] https://github.com/distcc/distcc
[200] https://www.distcc.org/man/include_server_1.html
[211] https://github.com/icecc/icecream
[212] https://github.com/icecc/icecream/blob/master/README.md
[222] https://manpages.debian.org/testing/icecc/icecc-scheduler.1.en.html
[225] https://github.com/icecc/icecream/issues/458
[226] https://chromium.googlesource.com/infra/goma/server/+/refs/tags/v0.0.1/README.md
[235] https://chromium.googlesource.com/infra/goma/client/
[241] https://www.fastbuild.org/docs/features/distribution.html
[243] https://www.fastbuild.org/docs/features.html
[246] https://github.com/fastbuild/fastbuild/blob/main/README.md
[257] https://docs.incredibuild.com/windows/components-and-architecture
[258] https://docs.incredibuild.com/windows/process-virt-overview
[262] https://www.incredibuild.com/distributed-processing
[269] https://docs.incredibuild.com/linux/incredibuild-technology
[271] https://docs.nativelink.com/remote-execution/persistent-workers
[272] https://github.com/buildfarm/buildfarm
[276] https://buildfarm.github.io/buildfarm/docs/quick_start/
[288] https://www.buildbuddy.io/docs/rbe-platforms/
[298] https://www.buildbuddy.io/blog/distributed-scheduling-for-faster-builds/
[299] https://www.buildbuddy.io/docs/rbe-pools/
[301] https://docs.engflow.com/re/client/remote-persistent-workers.html
[302] https://docs.engflow.com/re/index.html
[306] https://docs.engflow.com/CHANGELOG.html
[316] https://blog.bazel.build/2016/03/18/sandbox-easier-debug.html
[318] https://docs.bazel.build/versions/main/remote-execution-caching-debug.html
[331] https://bazel.build/basics/hermeticity
[332] https://docs.bazel.build/versions/3.4.0/remote-execution-caching-debug.html
[336] https://bazel.build/remote/rules
[337] https://bazel.build/versions/8.3.0/remote/rules
[342] https://bazel.build/remote/workspace
[346] https://bazel.build/remote/dynamic
[354] https://blog.bazel.build/2019/02/01/dynamic-spawn-scheduler.html
[359] https://github.com/bazelbuild/bazel/issues/15872
