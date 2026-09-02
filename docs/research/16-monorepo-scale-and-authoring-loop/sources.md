<!-- Raw research source. Generated via Perplexity `perplexity_research` (web-grounded,
     multi-source) on 2026-09-01 for the liquid project. Lightly reformatted only.
     Treat as evidence, not as liquid design decisions - see README.md in this
     directory for the curated analysis and the implications drawn for liquid. -->

# Deep research synthesis: monorepo scale, virtual filesystems, and the low-latency edit loop, 2024–2026

**Bottom line:** repository scale is no longer solved by “making Git faster” alone. The successful systems make cost proportional to the developer’s **working set and change set**, not to total repository size:

1. virtualize or sparsify the checkout;
2. maintain a durable, incremental record of filesystem changes;
3. keep compiler and build graphs alive in daemons;
4. cancel obsolete work immediately;
5. expose profiles and critical paths rather than optimizing aggregate CPU time;
6. reserve keystroke-time work for sub-100-ms semantic feedback and save-time work for heavier compilation and tests.

The canonical scale numbers below are often historical snapshots because Meta, Microsoft, and Google do not continuously publish internal repository dimensions. The architecture and operational guidance, however, remain directly relevant in 2024–2026.

---

## 1. Very large repositories

| System or technique | How it scales | Important limits and concrete evidence |
|---|---|---|
| **Meta Sapling + EdenFS** | Sapling separates the client, **Mononoke** distributed source-control server, and **EdenFS** virtual working copy. The stated goal is for operations to scale with files a developer uses rather than total repository size. EdenFS initially represents files and directories as source-control objects and fetches their trees/blobs only when traversed or read. Local modifications become **materialized** in an overlay; unmodified entries remain reconstructible from source control. [web:35][web:39] | Meta says this architecture supports an internal repository with **tens of millions of files, commits, and branches**. The cost moves rather than disappears: first access may require network/object-store latency; writes are usually slower than direct local-disk writes; and tools that enumerate the whole namespace can defeat laziness. [web:44][web:531] |
| **EdenFS status/change detection** | Materialization propagates up the directory ancestry, making the modified portion quickly discoverable. EdenFS also keeps a journal of modifying filesystem I/O; Watchman uses `getFilesChangedSince()` rather than crawling the virtual tree. This is why status/checkout can approach **O(changed files)** rather than O(repository files). [web:526][web:532] | The journal is bounded history, not an eternal source of truth. A stale consumer still needs recovery. Builds, IDEs, virus scanners, search indexers, and `find`-style tools must avoid indiscriminately opening every virtual file. |
| **Mononoke** | Centralizes repository storage and serves Sapling-specific lazy commit-graph and object protocols. Sapling’s dedicated graph protocol aims to make clone and pull roughly **O(merges)** in time and space instead of copying the complete graph and object history. [web:33] | Mononoke is used at Meta but is not offered as a supported turnkey public service; the open-source code is primarily suitable for experimentation. EdenFS has a similar support caveat. [web:35] |
| **Watchman integration with Sapling/EdenFS** | With a physical checkout, Watchman makes add/remove/status proportional to changed files. With EdenFS, Watchman can consume EdenFS’s native journal instead of installing ordinary OS watches across an apparently enormous tree. [web:33][web:526] | Watchman is only as correct as its notification synchronization and recovery path. Overflow or journal discontinuity leads to a recrawl or conservative “everything changed” result. |
| **Microsoft VFS for Git / GVFS** | VFS for Git was created specifically to migrate the Windows source tree to Git. A projected working directory exposed all paths, but files were hydrated on first read. Git itself was modified so index/status operations worked over the modified or projected set rather than assuming every tracked file was physically present. [web:3] | The Windows repository snapshot was approximately **3.5 million files and 300 GB**. Conventional Git had roughly **12-hour clone, 2–3-hour checkout, and 10-minute status** behavior; GVFS reduced these to a few minutes, about **30 seconds**, and **4–5 seconds**, respectively. Developers typically needed only **50,000–100,000** files. [web:511][web:518] |
| **Windows repository migration** | Microsoft consolidated the Windows codebase into Git while supporting roughly **3,500–4,000 engineers**, thousands of pushes and approximately **1,760 official builds per day** in the published snapshot. Distributed cache/proxy servers were needed because lazy fetching from a distant central service made clone time strongly geography-dependent. [web:511][web:512] | An especially revealing old baseline was an **86-GB initial pack**, **400-MB Git index**, and conventional operations of about **8 minutes for status and 30 minutes for commit**. VFS changes made Git closer to O(modified) but required a Microsoft Git fork, a filesystem provider, and a compatible server protocol. [web:515] |
| **Scalar** | Scalar evolved from the VFS-for-Git work into a thin orchestration layer around upstream/core Git features: partial clone, sparse checkout, commit graph, multi-pack/index maintenance, background maintenance, and tuned configuration. `scalar clone` can use partial clone or Azure’s GVFS protocol and begins with a sparse working tree. [web:2][web:4][web:14] | Scalar is not itself a general virtual filesystem. It reduces local objects and populated paths using Git features; the Windows repository still depends on VFS for Git for its stronger projection semantics. [web:8] |
| **Google Piper** | Piper is a custom distributed source-control service rather than Git. The published snapshot described approximately **1 billion files, 35 million commits, 86 TB**, 2 billion lines of code, distribution across **10 data centers**, and peak traffic around **800,000 queries/s**. [web:496][web:499] | Those are 2015–2016 snapshot figures, not 2026 measurements. The important result is architectural: no ordinary client clones this repository, and central infrastructure, access control, code search, review, and build tooling were designed around the monorepo. |
| **Google CitC—Clients in the Cloud** | CitC combines a cloud workspace backend with a Linux FUSE filesystem. A workspace shows the entire Piper tree plus the developer’s overlay, while only modified files are stored in that workspace. The published average workspace contained **fewer than 10 files**. No explicit clone or sync is required. [web:23] | Ordinary Unix tools work, but a workload that recursively reads the whole namespace still creates remote metadata/content traffic. CitC is Linux-specific in the published design and depends on low-latency access to Google’s infrastructure. |
| **CitC + Blaze** | CitC supplies a complete-looking source namespace; Blaze, Google’s internal ancestor of Bazel, uses explicit `BUILD` targets and dependency edges. In combination, the filesystem fetches only paths Blaze opens while the build graph limits work to requested targets and their dependency closure. Bazel’s external form likewise treats a monorepo as a target graph rather than an undifferentiated tree. [web:573][web:574] | The VFS does not determine build impact by itself. Correct, fine-grained `BUILD` dependencies are essential: overly broad targets, recursive globs, undeclared inputs, and monolithic linking or tests can turn a one-file change into a huge rebuild. |
| **Git partial clone: `--filter=blob:none`** | Clone and fetch obtain reachable commits and trees but omit file-content blobs. A promisor remote supplies a missing blob when a later command needs it. This sharply reduces initial transfer when history contains many or large blobs. [web:61][web:62] | It does **not** reduce the number of commits or trees, index size, working-tree metadata checks, or build scope. First use becomes a network miss; commands touching many paths can generate a fetch storm; offline operation may fail for missing objects; and the server must advertise filtering. |
| **Sparse checkout** | Sets `SKIP_WORKTREE` so only selected paths are populated. **Cone mode** expresses directory subtrees and avoids expensive arbitrary pattern matching. It complements partial clone: sparse checkout limits the working tree, while partial clone limits downloaded objects. [web:79][web:82] | Non-cone patterns can cost **O(N×M)** for N patterns and M index paths. Sparse checkout can surprise tools that assume all repository paths exist locally, and changing cones can trigger large downloads/materialization. |
| **Sparse index** | Collapses out-of-cone directories to tree entries instead of retaining one index entry per tracked file. Its design target is changing index-sensitive operations from O(total files) toward **O(populated files)**; `status` and `add` can benefit substantially. [web:76][web:77] | Current Git documentation still calls it experimental and warns that external tools may not understand the modified index representation. Unsupported Git commands may expand it back to a full index or become slower. [web:76][web:88] |
| **Commit graph** | Stores commit IDs, parent positions, root trees, dates, and generation numbers so ancestry walks avoid repeatedly parsing commit objects. `--changed-paths` adds Bloom filters that allow path history queries to skip commits that definitely did not modify a path. [web:46][web:47] | It improves history/ancestry, not working-tree scans or blob transfer. Bloom filters are probabilistic and large changes may get no useful filter; one Microsoft implementation used **512 changed paths** as the cutoff. Published testing showed path-log speedups of **2–6×** on Git/Linux/Android and **10–30×** on an Azure DevOps repository, at the cost of a slower graph write. [web:52][web:57] |
| **Where ordinary Git reaches practical limits** | The independent axes are object/history volume, tracked-path count and index size, populated working-tree count, change rate, branch/ref count, and daily server traffic. Partial clone addresses blobs; commit graph addresses history; sparse checkout/index address paths/index; FSMonitor addresses filesystem scans. No single switch covers all axes. | At **500,000 files**, Canva reported a fresh-clone `git status` averaging **10 seconds** and `fetch` ranging from **15 seconds to minutes**; tuning and caches brought status to roughly **2–3 seconds**. GitHub reported large-worktree status falling from **17–85 seconds** to **under one second** with FSMonitor. [web:618][web:616] |

### Practical hierarchy for a large Git monorepo

| Repository condition | First intervention | Escalation point |
|---|---|---|
| Large history/blob volume, moderate working tree | `--filter=blob:none`, commit graph, multi-pack maintenance | Add local object proxy/cache if demand fetches are WAN-bound |
| Millions of tracked paths, small team-specific working sets | Cone-mode sparse checkout plus sparse index | VFS projection if users frequently cross sparse boundaries |
| `status` dominated by `stat()` calls | Built-in FSMonitor or Watchman plus untracked cache | Virtual filesystem/journal if the physical checkout itself is the problem |
| Huge builds despite small changes | Fine-grained build targets, depfiles, remote cache/execution | Refactor coarse compilation, link, packaging, and test units |
| Tools continuously traverse the entire repository | Configure exclusions and graph-aware discovery | Run search/indexing centrally rather than on every workstation |

---

## 2. File-change detection at scale

The critical distinction is between **discovering the current state** and **receiving change hints**. Kernel notifications are lossy queues. A reliable watcher maintains its own indexed view, consumes events promptly, detects overflow, and has a reconciliation strategy.

| Platform/mechanism | Architecture | Scale limits and failure semantics |
|---|---|---|
| **Linux `inotify`** | One inotify instance owns an ordered event queue, but monitoring is not recursive. A tool must walk the tree, install one watch per directory, watch for new subdirectories, and update watches after renames. [web:106][web:158] | Controlled by `fs.inotify.max_user_instances`, `max_user_watches`, and `max_queued_events`. Common documented defaults include **128 instances, 8,192 watches, and 16,384 queued events**, though distributions vary. Overflow drops events and emits `IN_Q_OVERFLOW`; initializing watches on a huge tree is itself a long unprotected scan. [web:107][web:110][web:112] |
| **Linux `fanotify`** | Can mark files, directories, mounts, or whole filesystems, receive file handles/descriptors, and support permission decisions. Filesystem or mount marks avoid one inotify watch per directory. [web:631][web:633] | It is primarily suited to antivirus, audit, and filesystem-wide monitoring, not automatically a superior IDE watcher. Broad marks and permission events may need `CAP_SYS_ADMIN`; event semantics and path reconstruction are more complex. Its queue can also overflow with `FAN_Q_OVERFLOW`; before Linux 5.13 the hard limit was **16,384 events** unless using privileged unlimited-queue behavior. [web:631] |
| **macOS FSEvents** | A daemon-maintained journal reports changes to directory hierarchies with persistent event IDs. A latency parameter intentionally coalesces nearby events to reduce callback and I/O overhead. [web:121][web:127] | Events may represent a directory rather than each changed file. `MustScanSubDirs` means recursively reconcile that subtree. `KernelDropped` or `UserDropped` requires a full scan because exact changes are unknowable. Lower coalescing latency improves responsiveness but increases event rate and CPU wakeups. [web:122][web:123] |
| **Windows `ReadDirectoryChangesW`** | Watches a directory handle and optionally its complete subtree, delivering `FILE_NOTIFY_INFORMATION` records through synchronous, asynchronous, or completion-routine APIs. [web:91] | Windows allocates a fixed-size kernel buffer on the first call and does not resize it for that handle. On overflow, the contents are discarded, success may still be returned with **zero bytes**, and the client must enumerate the tree to recover. Network shares add buffer and protocol constraints. [web:91] |
| **Windows NTFS USN change journal** | A persistent, per-volume sequence of records for file and directory creation, deletion, and modification. Consumers save a USN cursor and later read changes since that cursor, making it much better than process-local notifications for restart/recovery and large trees. [web:92][web:94] | It is volume-wide rather than naturally rooted at one repository, so consumers must map file IDs/parents and filter paths. Journal wrap, replacement, deletion, or a mismatched journal ID requires reconciliation. It is NTFS-specific. |
| **Watchman daemon** | One persistent service indexes watched roots and converts OS-specific event streams into a queryable “files changed since clock X” model. It uses inotify on Linux, FSEvents on modern macOS, and Windows facilities; clients can subscribe or query instead of each installing duplicate watchers. [web:168][web:169] | On queue loss, Watchman deliberately recrawls and may report many files as changed. It uses a dedicated event-consumer thread and recommends tuning inotify limits, but tuning reduces rather than eliminates overflow risk. [web:166][web:168] |
| **Watchman clocks and cookies** | Every query can create a unique temporary **cookie file** inside the watched root and wait until the watcher observes it. Because earlier events precede the cookie in the notification stream, the resulting clock is known to be current at least through query start. The default query synchronization timeout is **60 seconds**. [web:151][web:152] | Cookies establish ordering, not durability against dropped events. They are unavailable before the cookie directory is watched during an initial crawl/recrawl. A caller may set `sync_timeout: 0`, but then accepts a potentially lagging view. |
| **Full scans** | A cold watcher, overflow recovery, or traditional `git status` recursively enumerates entries and performs metadata calls. Even with cached dentries, work is O(number of physical paths), competes for filesystem caches, and can wake disks, antivirus, backup, and remote filesystem activity. | Real measurements show the magnitude: large GitHub worktrees took **17–85 seconds** without FSMonitor and under one second with it; Canva’s 500,000-file tree took about **10 seconds** for fresh `status`. A recrawl can also falsely invalidate every downstream build or index, multiplying the original scan cost. [web:616][web:618][web:139] |

**Engineering implication:** watchers should be treated as a shared database service, not as a convenience callback API. Store a clock/journal cursor, exclude generated trees such as `node_modules` and build outputs, consume events on a dedicated thread, and make overflow recovery observable.

---

## 3. “Compile as you type”: language servers as incremental compilers

| Language server | Incremental and overlay model | Cancellation, latency, and divergence from batch compilation |
|---|---|---|
| **rust-analyzer** | A stateful compiler front end: editor changes enter an internal VFS, change batches feed a Salsa query database, and syntax/semantic results are computed lazily. The architecture explicitly tries to keep edits inside a function body from invalidating global derived data. [web:197][web:198] | Applying a new edit advances Salsa’s revision and cancels analyses operating on the old revision. Long-running highlighting or analysis is discarded rather than allowed to block a newer completion request. [web:201] |
| **clangd** | Maintains one AST and preamble per open translation unit. Correctness depends heavily on the same flags as the real build, usually supplied by `compile_commands.json`; clangd constructs a virtual compiler invocation for every opened file. It tracks unsaved editor contents separately from disk. [web:181][web:182] | Each file has a serialized worker queue. Cancelled reads and superseded writes—such as consecutive keystrokes—are dropped. Writes are debounced until a read arrives or the user pauses, avoiding a complete AST rebuild for every character. [web:191] |
| **gopls** | The cache contains sessions, workspace views, immutable snapshots after edits, on-disk files, unsaved **Overlay** files, memoized parse/index work, dependency analysis, and type-checked packages. [web:211][web:214] | Its completion budget defaults to **100 ms**; deep completion narrows search as the budget is consumed. Parsing and type-checking diagnostics for recently edited packages run immediately, while whole-workspace analysis is normally delayed until about **one second** of idle time or can be configured for save-only. [web:241][web:223] |
| **TypeScript `tsserver`** | Wraps the TypeScript compiler and language-service APIs in a long-running JSON server. `ScriptSnapshot` supplies current text and the changed range from a prior version, enabling incremental parsing; the language service keeps a long-lived compilation context and supports compiler-like watch behavior. [web:542][web:544][web:545] | `tsserver` supports cancellation tokens; its default mechanism can use a named pipe whose presence signals cancellation of an in-flight operation. Project graph updates and diagnostics are batched rather than rebuilding every project synchronously for every character. [web:213] |
| **LSP transport semantics** | `didOpen` establishes the in-memory document; `didChange` can carry range-level incremental edits; `didClose` releases the overlay. Therefore the analyzed source can be newer than—or never written to—the filesystem. [web:227][web:230] | `$/cancelRequest` tells the server that a result is obsolete. The server still must terminate the request and respond, so internal compiler operations need cooperative cancellation points rather than merely ignoring the eventual reply. [web:226][web:229] |

### The appropriate latency ladder

| Interaction | Practical budget | Suitable work |
|---|---:|---|
| Character echo, local syntax highlighting | ~16 ms frame budget | Rope/text update, local lexer/parser delta |
| Completion, hover, signature help | **<100 ms** | Cached AST/query lookup, limited local type inference, indexed symbols |
| Live diagnostics | 100–500 ms, usually debounced | Parse/type-check changed file or package; cancel on next edit |
| Workspace diagnostics | ~1 s idle or background | Reverse-dependency invalidation, static analysis |
| Save-time build/test | Seconds | Emit object code, link a narrow target, run selected tests |
| Full validation/CI | Minutes | Broad integration, packaging, hermetic and cross-platform tests |

The `<100 ms` target is not arbitrary: gopls states that feedback for continuous actions should remain under **100 ms** to be imperceptible and that delays above **200 ms** aggravate users; its completion default is correspondingly 100 ms. [web:251][web:241]

### Why language-server work duplicates batch compilers

| Reason | Consequence |
|---|---|
| **Incomplete and temporarily invalid programs** | A language server must produce useful trees, types, completions, and navigation halfway through a statement. A batch compiler may stop or cascade errors. |
| **Different complexity target** | A batch compiler can perform work proportional to the crate/package/program. An interactive server needs the common edit to be proportional to the changed region and affected dependency slice. |
| **Different output representation** | The compiler optimizes toward object code; the language server needs a persistent, rapidly queryable symbol/reference/type model. |
| **Cancellation and prioritization** | Completion under the cursor must preempt stale background diagnostics—an unusual requirement in a conventional one-shot compiler. |
| **Unsaved overlays** | Build tools read disk; language servers must combine editor buffers with disk dependencies and often multiple workspace configurations. |
| **Build-configuration ambiguity** | clangd must reconstruct real compile flags; gopls must model modules/build tags; rust-analyzer must model Cargo features and generated code; tsserver must map files to configured or inferred projects. |
| **Reuse remains preferable where possible** | clangd directly uses Clang’s front end; `tsserver` encapsulates TypeScript’s compiler/services; gopls reuses Go parser/type components. Rust-analyzer historically reimplemented much of a latency-oriented front end but now shares abstractions such as `rustc_type_ir` and the next trait solver with rustc to reduce semantic drift. [web:260][web:262] |

The apparent duplication is therefore partly real and partly a second orchestration layer over shared parsing/type infrastructure. Sharing compiler libraries improves consistency, but forcing an IDE through an unmodified batch pipeline usually loses latency, cancellation, and malformed-code tolerance.

---

## 4. Continuous/background builds and daemons

| Tool/system | Persistent or incremental mechanism | Trigger granularity and limits |
|---|---|---|
| **Bazel server** | Most Bazel logic lives in a server process retained in RAM. It preserves the Skyframe dependency/analysis graph and caches between commands; default idle shutdown is **10,800 seconds/3 hours**. Persistent compiler workers avoid repeated JVM/compiler startup; Bazel reports its own compilation as about **2.5× faster** with workers. [web:287][web:295][web:300] | Incremental benefit disappears after server restart, incompatible startup-option changes, `clean`, or discarded analysis state. A warm action cache does not remove loading/analysis, input hashing, remote-cache lookup, or output-download costs. |
| **Gradle daemon** | Keeps the JVM warm, retains project information, benefits from JIT optimization, and maintains an in-memory VFS updated by native filesystem watching. [web:302][web:304] | Configuration-heavy or non-cacheable builds can still spend substantial time before tasks execute. Plugins that inspect arbitrary files or perform work during configuration weaken incrementality. |
| **Gradle continuous build** | `--continuous` reruns requested tasks when declared inputs change. It uses filesystem watching and waits for a configurable **quiet period** before rebuilding. [web:301] | It is save/filesystem-event driven, not truly keystroke driven, and does not work with `--no-daemon`. Undeclared inputs, files outside watched project boundaries, or tasks modifying their own inputs can cause misses or loops. |
| **Buck2 daemon** | One daemon per project root communicates with the CLI over gRPC, watches the filesystem, and retains caches plus a DICE—Distributed Incremental Computation Engine—graph covering evaluation, configuration, analysis, execution, and materialization. [web:271][web:272][web:274] | `buck2 clean`, daemon death, isolation-directory changes, or broad invalidation removes warm-state benefits. Action granularity still constrains how incremental the actual compiler/linker can be. |
| **watchexec** | Cross-platform recursive watcher that launches or restarts an arbitrary command. It debounces event bursts; the documented default is **50 ms**. [web:316] | It knows files and processes, not build dependencies. If wired to `make all`, every save still invokes the whole top-level command; correctness and incrementality belong to that command. |
| **entr** | Reads a file list from stdin and executes or restarts a command when one changes; it uses native mechanisms such as inotify/kqueue. [web:319][web:320] | Simpler and predictable, but less naturally recursive/dynamic. New files require directory tracking or regenerating the input list. It provides no persistent build graph itself. |
| **webpack watch + HMR** | Watch mode preserves module state and timestamps; HMR emits a manifest plus changed chunks and applies updated modules without a full page reload. Webpack recommends its built-in watch mode so compilation cache invalidation receives exact timestamp information. [web:560][web:564] | Loader/plugin invalidation, broad chunk boundaries, CSS/runtime side effects, or an unaccepted module can push an update to a larger boundary or full reload. |
| **Vite HMR** | In development, native ESM avoids eagerly bundling the entire application. A changed module is invalidated back to its nearest HMR boundary and refetched. Current Vite documentation reports browser reflection in **under 50 ms** for suitable HMR updates. [web:349][web:358] | Production builds still bundle. Plugin transforms, framework state rules, server round trips, and a change reaching the application root can enlarge the update or cause reload. |
| **esbuild** | A long-lived build context exposes `rebuild()`, `watch()`, and `serve()`, reusing parsed and graph state and automatically rebuilding when a saved file invalidates the build. [web:346][web:348] | Its watch mode is save/event driven. Type checking is generally delegated to TypeScript or another checker, so fast transpilation is not the same as full semantic validation. |
| **Turbopack** | Rust-based incremental bundler integrated into Next.js. It lazily builds modules requested by the dev server and supports incremental bundling and Fast Refresh. [web:355][web:359] | Performance depends on supported loaders/plugins and framework integration. Compatibility fallbacks or broad invalidation can remove the expected advantage over webpack. |
| **Nix** | The daemon realizes derivations, substitutes from binary caches, and memoizes package outputs through store paths/content identities. This is strong reproducibility and package-level reuse. | It is not a Bazel/Buck-style persistent daemon-side source/action graph. Evaluation typically constructs derivations anew in the client, and changing source for a derivation commonly rebuilds the whole package. A 2026 analysis describes Nix incrementality as package-granularity rather than module-granularity; practical reports note that one changed module may rebuild the package. [web:333][web:335] |

### Save versus keystroke

| Model | Best use | Why |
|---|---|---|
| **Build on keystroke** | Parse, type information, completion, local diagnostics | Works from unsaved VFS overlays, uses cancellation, and avoids writing temporary invalid code to disk |
| **Build after short idle** | Broader diagnostics, linting, lightweight package checks | Debouncing prevents repeated work while preserving near-live feedback |
| **Build on save** | Transpilation, asset update, focused tests, HMR | Filesystem watchers and ordinary build tools see a coherent disk version |
| **Explicit build** | Linking, packaging, deployment artifacts, expensive test suites | The developer indicates that heavier and less interruptible work is now valuable |
| **CI** | Hermetic clean validation and broad impact testing | Provides independence from potentially stale workstation daemon state |

A major anti-pattern is running the complete batch build on every keystroke. It creates cancellation pressure without having cancellation-safe compiler/linker phases and tends to make the editor unusable. The LSP should own speculative unsaved analysis; the build daemon should own durable save-time artifacts.

---

## 5. Build-graph observability and critical path

| Signal/tool | What it answers | Interpretation |
|---|---|---|
| **Bazel Build Event Protocol (BEP)** | Streams structured invocation, target, test, artifact, failure, timing-summary, and build-metric events to files or a Build Event Service. It allows external systems to inspect an invocation while it runs. [web:372][web:596] | BEP is the invocation/result envelope, not by itself a full causal execution trace. Failed actions appear by default; publishing every action increases volume. |
| **Bazel execution log** | Records executed spawns/subcommands in JSON or binary. Optional spawn metrics expose local/remote execution, queuing, and other detailed timings; comparing logs can diagnose unexpected cache misses and nondeterminism. [web:602][web:612] | Use it when “why did this action run?” or “why did the remote cache miss?” is the question. Compare action keys, command lines, environments, inputs, execution strategy, and platform. |
| **Bazel JSON trace profile** | Writes `command-$INVOCATION_ID.profile.gz` by default. It opens in `chrome://tracing`/Perfetto and shows loading, analysis, scheduling, execution, remote I/O, workers, and a critical-path lane. `bazel analyze-profile` provides phase totals and critical-path analysis. [web:361][web:366] | The longest-duration action is not necessarily the best target. Optimize actions on the **critical path**—the dependency chain determining wall time—or nodes with high “drag,” not merely high aggregate CPU. |
| **Bazel execution-graph log** | Captures actions and interdependencies, permitting calculation of critical-path drag: how much wall time could be saved if a node disappeared. [web:385][web:390] | Useful when abundant parallelism means a slow action is hidden, or when scheduling/resource contention changes which chain is critical. |
| **BuildBuddy** | Consumes BEP and displays logs, targets, artifacts, flags/environment, profile timing, downloads, actions, and critical-path information in a shareable invocation UI. [web:586][web:589][web:591] | Particularly useful for comparing “works locally” versus CI, cache behavior, and regressions across many invocations rather than inspecting one trace manually. |
| **Gradle Build Scan / Develocity** | `--scan` records startup, configuration, dependency resolution, task execution, tests, changed sources, artifacts, cacheability, and a timeline. The critical-path filter identifies the longest chain of dependent tasks. [web:392][web:393][web:399] | A long task outside the critical path may not affect wall time. Develocity 2024.2 added CPU, memory, network, and disk utilization over the timeline, helping distinguish computation from starvation or I/O. [web:398] |

### What actually makes a build slow

| Bottleneck | Observable signature | Typical remedy |
|---|---|---|
| **Loading/configuration** | Long time before actions exist; repeated plugin/macro/configuration work | Configuration cache, simpler macros/plugins, avoid recursive globs and repository-wide discovery |
| **Analysis graph construction** | High analysis time or unexpectedly high `actions_created` | Finer and cleaner rule APIs, avoid flattening transitive sets, reduce target/configuration explosion |
| **Invalidation too broad** | One source edit recreates or reruns thousands of actions | Correct depfiles, ABI/interface dependencies, narrower targets, eliminate undeclared inputs |
| **Execution critical path** | A serial chain of compile, codegen, link, package, or tests controls elapsed time | Split only where new parallelism or caching results; accelerate or avoid critical-path actions |
| **Insufficient parallelism** | CPU/remote workers idle while graph still has work | Fix dependency serialization; shard tests; split monolithic actions |
| **Resource contention** | Runnable actions queued; CPU, memory, disk, or network saturated | Resource-aware scheduling, more appropriate workers, local SSD, remote executor capacity |
| **Filesystem/input discovery** | Long sandbox setup, hashing, stat, or Merkle-tree time | Narrow declared inputs, VFS-native hashes, avoid enormous runfiles trees |
| **Remote cache** | High lookup/download time, low hit rate, or action keys unexpectedly differ | Normalize toolchains/environment, deterministic actions, geographically close cache, smaller outputs |
| **Remote execution** | Queue dominates action runtime | Increase capacity or platform pools; prioritize interactive work; avoid remote execution for tiny actions |
| **Output materialization** | Build actions are cached but wall time is still dominated by downloading files | Download only top-level outputs, use remote/deferred materialization |
| **Tests** | Long tail of a few shards, retries, flakes | Historical-duration sharding, quarantine flakes, test impact analysis, remove duplicate setup |
| **Cold daemon** | Slow loading/analysis only after restart or `clean` | Keep daemon warm and measure cold and warm latency separately |

---

## 6. Developer complaints and empirical cost

| Evidence or complaint | Concrete finding | What it establishes |
|---|---|---|
| **“A one-line change takes 40 minutes.”** | This should be interpreted as a systems symptom, not as proof that compilation itself requires 40 minutes. Time may be in dependency invalidation, queueing, clean checkout, analysis, remote-cache misses, linking, packaging, tests, or deployment. Microsoft’s pre-VFS Windows figures demonstrate the extreme form: roughly **30-minute commit**, **8-minute status**, and multi-hour clone/checkout operations on a 3.5-million-file tree. [web:515] | Measure the phases. A 40-minute critical-path action needs different treatment from 35 minutes in a CI queue or 35 minutes of unnecessary tests. |
| **Industry build-time survey** | Incredibuild’s 2022 survey reported an average build of **20 minutes**, CI/release build of about **23 minutes**, **nearly 25%** of ordinary builds and **32%** of CI/release builds over 30 minutes, and an average **57 minutes/day** spent waiting. It also reported that **98%** of respondents acknowledged wasting time waiting for builds. [web:422] | Vendor surveys may overrepresent build-heavy customers, but the order of magnitude is consistent with the widespread complaint that build latency consumes a meaningful part of the workday. |
| **Empirical CI study** | A study of **104,442 CI builds across 67 projects** found **84%** lasted longer than the commonly cited acceptable threshold of **10 minutes**. [web:430] | Long CI feedback is not confined to exceptional proprietary monorepos. |
| **Developer perceptions of CI wait** | A qualitative CI waiting-time study found **2 minutes** was considered optimal and **under 10 minutes** acceptable, with longer waits causing CI-specific, cognitive, and emotional effects. [web:423] | “Ten minutes” is an outer acceptability guideline, not an ideal interactive target. |
| **Google controlled build-latency experiment** | Upgraded machines reduced median build time per developer by about **13%**, only a few seconds in absolute terms. Without knowing they were in the experiment, affected developers showed a **4-percentage-point** increase in reporting at least moderate productivity, a **5-point** increase in velocity satisfaction, and submitted about **24 more lines per week**. [web:451] | There is no single magic threshold: even small reductions can improve task persistence and perceived productivity. Predictability also helps developers decide whether to wait or switch tasks. |
| **Task switching** | Google’s study found faster builds reduce task switching; predictable build duration helps developers manage switching even when builds remain slow. [web:451] | A reduction from 30 to 20 minutes may not recover ten productive minutes if the developer still switches tasks. Crossing from “switch away” to “stay in flow” can be more valuable than an equal absolute reduction elsewhere. |
| **Repeated/flaky CI builds** | An OpenStack study of **66,932 code reviews** found recheck commands in **55%** of reviews after a failing build; recheck changed the result in only **42%** of cases. Reviews with rechecks had about **22×** the waiting time, and the aggregate waste was estimated at **187.4 compute-years** and **16.81 years of added review wait**. [web:432] | Reliability and flake elimination can improve feedback more than raw compiler acceleration. |
| **Broad developer-experience friction** | Atlassian’s 2024 research reported **69% of developers losing eight or more hours per week to inefficiencies**. This is broader than builds, but slow build/CI/tool feedback is one recurring component of that friction. [web:486] | Build speed should be evaluated as part of developer experience, not as an isolated benchmark. |
| **DORA interpretation** | DORA defines **change lead time** as time from commit to successful production deployment and recommends finding the delivery system’s largest constraint. Its 2024 work also associates well-designed internal platforms and developer independence with better productivity. [web:408][web:419] | DORA does **not** establish a universal conversion such as “one build minute costs N dollars.” Slow local builds and CI are leading indicators that contribute to lead time, batch size, deployment frequency, and recovery speed; the effect must be measured in the organization’s actual workflow. |

---

## Recommended 2024–2026 reference architecture

| Layer | Recommended design |
|---|---|
| **Repository service** | Central, horizontally scalable object/commit service; regional caches; partial-clone/promisor support; server-side reachability and graph indexes |
| **Workspace** | Cone sparse checkout plus sparse index for ordinary large Git; move to VFS projection when working-tree population itself dominates |
| **Change service** | One persistent watcher per workspace, preferably journal-backed; clocks/cookies or equivalent barriers; explicit overflow counters and recrawl telemetry |
| **IDE semantics** | Language server with an in-memory overlay, incremental query graph, cached project model, cancellation, and foreground/background priority classes |
| **Build daemon** | Persistent graph and workers; exact declared inputs; content-addressed local/remote cache; deferred output materialization |
| **Trigger policy** | Keystroke: parser/type queries; idle: diagnostics; save: narrow build/HMR/tests; explicit or CI: broad validation |
| **Observability** | Record cold and warm p50/p95/p99; separate queue, loading, analysis, execution, remote I/O, and materialization; retain execution logs and critical paths |
| **Developer-facing SLOs** | Completion/hover p95 under **100 ms**; changed-file diagnostics generally under **500 ms**; common incremental build fast enough to avoid task switching; presubmit first useful failure ideally below **10 minutes**, with queue time reported separately |
| **Optimization priority** | First eliminate accidental O(repository) scans and broad invalidation; then improve cache hit rate; then shorten the critical path; only then optimize noncritical aggregate CPU |

The unifying principle is:

> **Keep namespace visibility broad, but make materialization, observation, semantic analysis, and build execution proportional to the developer’s active dependency slice.**

Virtual filesystems solve checkout scale; journals and Watchman solve repeated discovery; language-server overlays solve unsaved-code latency; build daemons solve repeated graph construction and process startup; and critical-path observability tells you which remaining delay actually changes wall-clock feedback.

Citations:
[31] https://sapling-scm.com/docs/scale/overview/
[32] https://github.com/facebook/sapling
[33] https://sapling-scm.com/docs/git/git_support_modes/
[34] https://github.com/facebook/sapling/blob/main/eden/fs/docs/Overview.md
[35] https://github.com/facebook/sapling/blob/main/README.md
[36] http://github.com/orgs/facebook/packages/container/package/sapling%2Fmanylinux_2_34
[37] https://github.com/DurhamG/eden/blob/main/eden/fs/docs/Overview.md
[38] https://developers.facebook.com/blog/post/2022/11/15/meta-developers-workflow-exploring-tools-used-to-code/
[39] https://github.com/facebook/sapling/blob/main/eden/fs/docs/Glossary.md
[40] https://news.ycombinator.com/item?id=39226611
[41] https://github.com/kmancini/eden/blob/main/eden/fs/docs/Overview.md
[42] https://github.com/facebook/sapling/blob/main/eden/fs/docs/Process_State.md
[43] https://github.com/DurhamG/eden
[44] https://engineering.fb.com/2022/11/15/open-source/sapling-source-control-scalable/
[45] https://github.com/facebook/sapling/blob/main/eden/fs/docs/Data_Model.md
[1] https://devblogs.microsoft.com/devops/introducing-scalar/
[2] https://github.com/microsoft/scalar
[3] https://github.blog/open-source/git/the-story-of-scalar/
[4] https://github.com/microsoft/git/blob/f90e3eda8744efad2aba71b4a8feaf9a3d9a546d/contrib/scalar/docs/getting-started.md
[5] https://m.jakstys.lt/2023/microsoft-git/
[6] https://learn.microsoft.com/en-ca/training/modules/manage-git-repositories/2-work-large-repositories
[7] https://devblogs.microsoft.com/devops/announcing-gvfs-git-virtual-file-system/
[8] https://github.com/microsoft/VFSForGit/blob/master/docs/faq.md
[9] https://github.com/microsoft/scalar/blob/main/Readme.md
[10] https://github.com/microsoft/vfsforgit
[11] https://github.com/microsoft/VFSForGit
[12] https://deepwiki.com/microsoft/VFSForGit
[13] https://devblogs.microsoft.com/bharry/the-largest-git-repo-on-the-planet/
[14] https://github.com/microsoft/git/blob/HEAD/contrib/scalar/docs/index.md
[15] https://gitlab.com/gitlab-org/gitlab/-/issues/255355
[16] https://www.uncommonengineer.com/docs/books/papers/Google3/
[17] https://news.ycombinator.com/item?id=13561096
[18] https://github.com/florian/reading-notes/blob/master/papers/003_Why_Google_Stores_Billions_of_Lines_of_Code_in_a_Single_Repository.md
[19] https://paper.lingyunyang.com/reading-notes/journal/communications-of-the-acm/2015/why-google-stores-billions-of-lines-of-code-in-a-single-repository
[20] https://en.wikipedia.org/wiki/Piper_(source_control_system)
[21] https://monorepovspolyrepo.com/case-studies/
[22] https://www.s9a.me/posts/2022-12-21-about-googles-mono-repo/
[23] https://cacm.acm.org/research/why-google-stores-billions-of-lines-of-code-in-a-single-repository/
[24] https://www.twosapp.com/5d9e4615836e22f288f48e55
[25] https://github.com/ruanyf/articles/blob/master/2016/2016-07-01-google-monolithic-source-repository.md
[26] https://courses.cs.vt.edu/cs3704/spring22/student_presentations/Group-4.pdf
[27] https://www.reddit.com/r/coding/comments/acieh0/170201715_software_engineering_at_google/
[28] https://www.cnblogs.com/itech/p/21344207
[29] https://gist.github.com/MartinAmps/c290bc01454d3c1b8c39
[30] https://dev.to/tbodt/comment/1j4e
[61] https://git-scm.com/docs/git-backfill
[62] https://git-scm.com/docs/git-clone/2.47.0
[63] https://git-scm.com/docs/partial-clone/2.16.6
[64] https://git-scm.com/docs/partial-clone/2.24.0
[65] https://git-scm.com/docs/partial-clone/2.25.0
[66] https://git-scm.com/docs/partial-clone/2.19.0
[67] https://gitperf.com/chapter-11.html
[68] https://git-scm.com/docs/partial-clone/2.36.0
[69] https://git-scm.com/docs/git-clone/2.48.0
[70] https://stackharbor.com/en/knowledge-base/git-partial-clone-sparse-checkout-large-repos/
[71] https://anvil.fangorn.io/fangorn/ex_git_objectstore/issues/53
[72] https://www.kdab.com/native-big-file-handling-in-git/
[73] https://stackoverflow.com/questions/79206958/git-partial-clone-with-multiple-remotes
[74] https://latchkey.dev/learn/git/git-partial-clone-filter-not-supported
[75] https://www.mslinn.com/git/600-partial-clone.html
[76] https://git-scm.com/docs/git-sparse-checkout
[77] https://git-scm.com/docs/sparse-index
[78] https://git-scm.com/docs/sparse-checkout
[79] https://git-scm.com/docs/git-sparse-checkout/2.35.0
[80] https://git-scm.com/docs/git-checkout/2.27.0
[81] https://git-scm.com/docs/git-checkout/2.25.0
[82] https://git-scm.com/docs/git-sparse-checkout/2.28.0
[83] https://git-scm.com/docs/git-checkout/2.21.0
[84] https://runbook.fyi/Git/monorepos-and-scaling/
[85] https://gitperf.com/chapter-10.html
[86] https://github.com/git/git/blob/master/sparse-index.h
[87] https://git-scm.com/docs/git-rm
[88] https://git-scm.com/docs/index-format
[89] https://github.com/git-for-windows/git-snapshots/blob/main/sparse-index.h
[90] https://oneuptime.com/blog/post/2026-01-24-git-sparse-checkout/view
[46] https://git-scm.com/docs/commit-graph-format/2.29.0
[47] https://git-scm.com/docs/git-commit-graph
[48] https://git-scm.com/docs/gitformat-commit-graph
[49] https://www.grizzlypeaksoftware.com/library/git-performance-optimization-for-large-repositories-nxxd5xrq
[50] https://git-scm.com/docs/commit-graph
[51] https://git-scm.com/docs/commit-graph-format/2.36.0
[52] https://github.com/gitgitgadget/git/releases/tag/pr-497/garimasi514/coreGit-bloomFilters-v4
[53] https://www.grizzlypeaksoftware.com/library/git-performance-optimization-for-large-repositories-dxifo1ab
[54] https://gitperf.com/chapter-08.html
[55] https://news.ycombinator.com/item?id=17540464
[56] https://stackoverflow.com/questions/57837986/speed-up-git-blame-on-repository-with-many-commits
[57] https://devblogs.microsoft.com/devops/super-charging-the-git-commit-graph-iv-bloom-filters/
[58] https://deepwiki.com/git/git/2.2.2-commit-graph-and-bloom-filters
[59] https://gitlab.utu.fi/taahol/git/-/blob/v2.30.0-rc1/bloom.h
[60] https://github.blog/open-source/git/highlights-from-git-2-28/
[106] https://man.archlinux.org/man/inotify.7.en
[107] https://linux.die.net/man/1/inotifywait
[108] https://linux.die.net/man/1/inotifywatch
[109] https://intellij-support.jetbrains.com/hc/en-us/articles/15268113529362-Inotify-Watches-Limit-Linux
[110] https://docs.rockylinux.org/10/books/learning_rsync/06_rsync_inotify/
[111] https://transang.me/everything-about-inotify-in-linux-and-macos/
[112] https://github.com/passcod/blog/blob/main/2017-jun-26-inotify-watch-limit.md
[113] https://manpages.ubuntu.com/manpages/bionic/man7/fanotify.7.html
[114] https://stackoverflow.com/questions/535768/what-is-a-reasonable-amount-of-inotify-watches-with-linux
[115] https://www.baeldung.com/linux/inotify-upper-limit-reached
[116] https://crackingwalnuts.com/linux/inotify
[117] https://forum.syncthing.net/t/increase-the-inotify-limit-linux-a-hack/17434
[118] https://unix.stackexchange.com/questions/15509/whos-consuming-my-inotify-resources
[119] https://medium.com/@ivanermilov/how-to-fix-inotify-cannot-be-used-reverting-to-polling-too-many-open-files-bb1c1437dbf
[120] https://github.com/fsnotify/fsnotify/issues/148
[121] https://github.com/fsnotify/fsevents/blob/main/README.md
[122] https://leopard-adc.pepas.com/documentation/Darwin/Conceptual/FSEvents_ProgGuide/FSEvents_ProgGuide.pdf
[123] https://developer.apple.com/library/archive/documentation/Darwin/Conceptual/FSEvents_ProgGuide/UsingtheFSEventsFramework/UsingtheFSEventsFramework.html
[124] https://metacpan.org/pod/Mac::FSEvents
[125] https://forums.developer.apple.com/forums/thread/744984
[126] https://www.crowdstrike.com/en-us/blog/using-os-x-fsevents-discover-deleted-malicious-artifact/
[127] https://developer.apple.com/documentation/coreservices/file_system_events
[128] https://metacpan.org/pod/Mac::FSEvents::Event
[129] https://blog.csdn.net/lovechris00/article/details/139566141
[130] https://forums.developer.apple.com/forums/thread/72559
[131] https://developer.apple.com/library/archive/documentation/Darwin/Conceptual/FSEvents_ProgGuide/FileSystemEventSecurity/FileSystemEventSecurity.html
[132] https://alexwlchan.net/2026/watch-files-on-macos/
[133] https://hackmd.io/@M4shl3/FSEvents
[134] https://lore.kernel.org/git/5d12b5d808a4bcdff87fe0c3a30f415081003459.1625150864.git.gitgitgadget@gmail.com/
[135] https://papers.put.as/papers/macosx/2017/summit_archive_1498158287.pdf
[91] https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readdirectorychangesw
[92] https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/fsutil-usn
[93] https://microsoft.github.io/windows-docs-rs/doc/windows/Win32/Storage/FileSystem/fn.ReadDirectoryChangesW.html
[94] https://learn.microsoft.com/en-us/windows/win32/fileio/change-journal-records
[95] https://learn.microsoft.com/en-us/windows/win32/fileio/change-journals
[96] https://learn.microsoft.com/en-us/windows/win32/fileio/creating-modifying-and-deleting-a-change-journal
[97] https://learn.microsoft.com/en-us/windows/win32/fileio/obtaining-directory-change-notifications
[98] https://qualapps.blogspot.com/2010/05/understanding-readdirectorychangesw_19.html
[99] https://developersarea.wordpress.com/tag/readdirectorychangesw/
[100] https://learn.microsoft.com/en-ca/answers/questions/1021759/using-readdirectorychangesw-to-get-information-abo
[101] https://blog.csdn.net/zhu_ing/article/details/148006241
[102] https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-read_usn_journal_data_v0
[103] https://learn.microsoft.com/en-us/windows/win32/fileio/using-the-change-journal-identifier
[104] https://thecodehound.com/synchronizing-files-with-c/
[105] https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-usn_journal_data_v2
[166] https://facebook.github.io/watchman/docs/troubleshooting
[167] https://facebook.github.io/watchman/docs/cookies
[168] https://facebook.github.io/watchman/docs/install.html
[169] https://facebook.github.io/watchman/
[170] https://stackoverflow.com/questions/33713635/how-can-i-configure-watchman-to-watch-multiple-fs-roots
[171] https://deepwiki.com/genesiscz/GenesisTools/10.1-file-watching
[172] https://oneuptime.com/blog/post/2026-03-02-set-up-watchman-file-system-events-ubuntu/view
[173] https://facebook.github.io/watchman/docs/cmd/query
[174] https://stackoverflow.com/questions/29632168/what-are-the-performance-and-reliability-implications-of-watching-too-many-files
[175] https://github.com/ember-cli/ember-cli/issues/4101
[176] https://facebook.github.io/watchman/docs/release-notes
[177] https://www.w3tutorials.net/blog/warning-watchman-watch-project-returned-a-warning-recrawled-this-watch-x-times-most-recently-because/
[178] https://facebook.github.io/watchman/docs/watchman-wait
[179] https://blog.csdn.net/gitblog_01017/article/details/153167817
[180] https://pkg.go.dev/github.com/sjansen/watchman
[151] https://facebook.github.io/watchman/docs/cookies
[152] https://facebook.github.io/watchman/docs/cmd/query
[153] https://facebook.github.io/watchman/docs/cmd/flush-subscriptions
[154] https://facebook.github.io/watchman/docs/cmd/state-enter.html
[155] https://facebook.github.io/watchman/docs/cmd/clock
[156] https://github.com/facebook/watchman/wiki/File-&-Dir-tracking-data-v2
[157] https://stackoverflow.com/questions/23778818/inotify-are-the-events-reported-in-strictly-the-same-order-as-they-have-occured/23798948
[158] https://manpages.ubuntu.com/manpages/focal/man7/inotify.7.html
[159] https://stackoverflow.com/questions/35736577/can-watchman-send-why-a-file-changed
[160] https://facebook.github.io/watchman/docs/cmd/trigger-del
[161] https://facebook.github.io/watchman/support
[162] https://facebook.github.io/watchman/docs/cmd/version
[163] https://facebook.github.io/watchman/docs/nodejs
[164] https://facebook.github.io/watchman/docs/cmd/watch
[165] https://facebook.github.io/watchman/docs/cmd/since
[136] https://kindatechnical.com/git-version-control/performance-troubleshooting-large-repos.html
[137] https://www.grizzlypeaksoftware.com/library/git-performance-optimization-for-large-repositories-dxifo1ab
[138] https://www.canva.dev/blog/engineering/we-put-half-a-million-files-in-one-git-repository-heres-what-we-learned/
[139] https://facebook.github.io/watchman/docs/troubleshooting
[140] https://sigit.si/git/git/blob/maint/t/perf/p7519-fsmonitor.sh
[141] https://github.com/facebook/watchman
[142] https://kb.vastdata.com/docs/optimize-and-accelerate-git-operations
[143] https://stackoverflow.com/questions/49443341/watchman-crawl-failed-retrying-once-with-node-crawler
[144] https://shekhargulati.com/2022/07/03/improve-git-monorepo-performance/
[145] https://www.monperrus.net/martin/one-million-files-on-git-and-github
[146] https://runbook.fyi/Git/monorepos-and-scaling/
[147] https://groups.google.com/g/msysgit/c/fL_jykUmUNE
[148] https://coreui.io/answers/how-to-optimize-git-performance/
[149] https://www.datalad.org/test_fs_analysis.html
[150] https://news.ycombinator.com/item?id=37291765
[196] https://sparkles-docs.pages.dev/research/parsing/rust-analyzer
[197] https://rust-analyzer.github.io/book/contributing/architecture.html
[198] https://rust-lang.github.io/rust-analyzer/src/vfs/lib.rs.html
[199] https://rust-lang.github.io/rust-analyzer/vfs/index.html
[200] https://rust-analyzer.github.io/
[201] https://github.com/rust-lang/rust-analyzer/blob/master/docs/dev/guide.md
[202] https://www.reddit.com/r/rust/comments/1ebgxpl/rustanalyzer_in_vscode_slow_aggressive/
[203] https://deepwiki.com/rust-lang/rust-analyzer/2.2-salsa-and-incremental-computation
[204] https://rust-analyzer.github.io/blog/2023/07/24/durable-incrementality.html
[205] https://rust-lang.github.io/rust-analyzer/src/rust_analyzer/cli/analysis_stats.rs.html
[206] https://docs.rs/rust-analyzer-salsa/latest/salsa/
[207] https://rust-analyzer.github.io/book/faq.html
[208] https://docs.rs/rust-analyzer-salsa/latest/src/salsa/lib.rs.html
[209] https://www.reddit.com/r/rust/comments/lagx7x/faster_incremental_builds_speculation_and/
[210] https://users.rust-lang.org/t/how-to-debug-rust-analyzer-slowness/127820
[181] https://clangd.llvm.org/design/compile-commands
[182] https://clangd.llvm.org/design/
[183] https://clangd.llvm.org/
[184] https://clangd.llvm.org/guides/
[185] https://clangd.llvm.org/design/code.html
[186] https://reviews.llvm.org/D49267
[187] https://clangd.llvm.org/config.html
[188] https://clangd.llvm.org/faq
[189] https://manpages.debian.org/unstable/clangd-15/clangd-15.1.en.html
[190] https://clangd.llvm.org/extensions
[191] https://clangd.llvm.org/design/threads
[192] https://manpages.debian.org/experimental/clangd-14/clangd-14.1.en.html
[193] https://github.com/clangd/vscode-clangd/issues/300
[194] https://clangd.llvm.org/troubleshooting
[195] https://clangd.llvm.org/design/indexing
[211] https://go.googlesource.com/tools/+/refs/heads/master/gopls/doc/design/implementation.md
[212] https://go.googlesource.com/tools/+/refs/tags/v0.25.1/gopls/doc/design/implementation.md
[213] https://github.com/microsoft/TypeScript/wiki/Standalone-Server-(tsserver)
[214] https://pkg.go.dev/golang.org/x/tools/gopls/internal/lsp/cache
[215] https://juejin.cn/post/6987205537935048735
[216] https://github.com/microsoft/TypeScript/wiki/Architectural-Overview/1afea54fbb7a4af15d613708ac0d1951f73aca14
[217] https://www.npmjs.com/package/virtual-tsc
[218] https://pkg.go.dev/golang.org/x/tools/gopls/internal/settings
[219] https://deepwiki.com/sourcegraph/go-langserver/2.2-virtual-file-system
[220] https://golang.ac.cn/gopls/design/implementation
[221] https://github.com/golang/go/issues/33727
[222] https://golang.com.tw/gopls/design/implementation
[223] https://go.dev/gopls/features/diagnostics
[224] https://android.googlesource.com/platform/external/golang-x-tools/+/refs/heads/build-tools-release/gopls/doc/design/implementation.md
[225] https://truffleagent.com/nook/
[241] https://go.dev/gopls/settings
[242] https://pkg.go.dev/golang.org/x/tools/gopls/internal/lsp/source
[243] https://github.com/sublimelsp/LSP-gopls/blob/main/sublime-package.json
[244] https://github.com/golang/go/issues/53992
[245] https://github.com/golang/go/issues/62665
[246] https://www.sciencedirect.com/science/article/pii/S0164121225002237
[247] https://www.generalcompute.com/blog/real-time-ai-inference-100ms-latency-production
[248] https://www.genaisystemdesign.com/blog/copilot-backend-case-study/
[249] https://research.google/blog/ml-enhanced-code-completion-improves-developer-productivity/
[250] https://github.com/golang/go/issues/41434
[251] https://go.dev/gopls/design/design
[252] https://arxiv.org/html/2603.05974v1
[253] https://datafield.dev/aibook/part-06/chapter-33/case-study-02.html
[254] https://www.zenml.io/llmops-database/building-a-low-latency-global-code-completion-service
[255] https://www.gocodeo.com/post/real-time-completion-vs-delayed-suggestions-ux-trade-offs-across-tools
[256] https://golang.design/under-the-hood/en/part5toolchain/ch16tools/gopls/
[257] https://dspace.cvut.cz/bitstream/handle/10467/108860/F8-DP-2023-Kvapil-Ondrej-ctufit-thesis.pdf?sequence=-1&isAllowed=y
[258] https://www.reddit.com/r/rust/comments/12cukw3/why_doesnt_rustanalyzer_reuse_infrastructures_of/
[259] https://rust-analyzer.github.io/book/faq.html
[260] https://rustc-dev-guide.rust-lang.org/solve/sharing-crates-with-rust-analyzer.html
[261] https://github.com/rust-lang/rust/issues/73467
[262] https://github.com/rust-lang/rfcs/blob/master/text/2912-rust-analyzer.md
[263] https://deepwiki.com/llvm/clangd-www/2-clangd-language-server
[264] https://github.com/rust-lang/rust-analyzer/blob/master/docs/user/manual.adoc
[265] https://www.reddit.com/r/ProgrammingLanguages/comments/lz8skk/language_server_built_into_the_compiler/
[266] https://rust-lang.github.io/rust-analyzer/src/ide_diagnostics/lib.rs.html
[267] https://www.linkedin.com/posts/akashsivanandan_github-ak4-shrahu-a-python-lsp-written-activity-7417423444633526272-BDZM
[268] https://www.oreilly.com/library/view/clang-compiler-frontend/9781837630981/text/B19722_08.xhtml
[269] https://pierce.dev/notes/how-language-servers-work
[270] https://linuxcommandlibrary.com/man/clangd
[226] https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/
[227] https://github.com/microsoft/language-server-protocol/blob/gh-pages/_specifications/lsp/3.18/specification.md
[228] https://deepwiki.com/microsoft/language-server-protocol/2-lsp-specification
[229] https://github.com/Microsoft/language-server-protocol/blob/gh-pages/_specifications/specification-3-16.md
[230] https://github.com/Microsoft/language-server-protocol/blob/main/versions/protocol-2-x.md
[231] https://llvm.org/doxygen/Protocol_8h_source.html
[232] https://wiki.eclipse.org/images/d/d8/LanguageServers_democamp_zurich_2017.pdf
[233] https://learn.microsoft.com/en-us/visualstudio/extensibility/language-server-protocol?view=visualstudio
[234] https://github.com/phpactor/language-server
[235] https://language-server-protocol.readthedocs.io/en/latest/api/client.html
[236] https://learn.microsoft.com/en-us/dotnet/api/microsoft.visualstudio.languageserver.protocol?view=visualstudiosdk-2022
[237] https://github.com/AArnott/vscode-languageserver-protocol/blob/master/README.md
[238] https://symposium.dev/research/lsp-overview/message-reference.html
[239] https://emisa-journal.org/emisa/article/view/320/210
[240] https://github.com/redhat-developer/lsp4ij/blob/main/docs/LSPSupport.md
[286] https://bazel.build/reference/command-line-reference
[287] https://bazel.build/run/client-server
[288] https://bazel.build/versions/6.0.0/run/client-server
[289] https://bazel.build/versions/6.5.0/run/client-server
[290] https://bazel.build/versions/7.7.0/reference/command-line-reference
[291] https://bazel.build/versions/9.0.0/reference/command-line-reference
[292] https://bazel.build/versions/9.0.0/docs/user-manual
[293] https://bazel.build/advanced/performance/iteration-speed
[294] https://bazel.build/advanced/performance/build-performance-breakdown
[295] https://bazel.build/contribute/codebase
[296] https://bazel.googlesource.com/bazel/+/staging
[297] https://bazel.build/versions/8.7.0/docs/android-build-performance
[298] https://arxiv.org/html/2405.00796v1
[299] https://bazel.build/docs/user-manual
[300] https://bazel.build/remote/persistent
[301] https://docs.gradle.org/current/userguide/continuous_builds.html
[302] https://docs.gradle.org/current/userguide/performance.html
[303] https://docs.gradle.org/current/userguide/gradle_daemon.html
[304] https://docs.gradle.org/current/userguide/file_system_watching.html
[305] https://docs.gradle.org/6.7/release-notes.html
[306] https://gradle.github.net.cn/current/userguide/continuous_builds.html
[307] https://docs.gradle.org/6.7.1/release-notes.html
[308] https://docs.gradle.org/6.5.1/release-notes.html
[309] https://docs.gradle.org/6.5/release-notes.html
[310] https://docs.gradle.org/7.0.1/release-notes.html
[311] http://man.hubwiz.com/docset/Gradle_User_Guide.docset/Contents/Resources/Documents/userguide/gradle_daemon.html
[312] https://github.com/gradle/gradle/issues/19584
[313] https://blog.gradle.org/how-gradle-works-2
[314] https://docs.gradle.org/2.5/release-notes.html
[315] https://bugs.eclipse.org/bugs/show_bug.cgi?id=473037
[271] https://buck2.build/docs/concepts/daemon/
[272] https://buck2.build/docs/concepts/architecture/
[273] https://buck2.build/docs/rule_authors/incremental_actions/
[274] https://buck2.build/docs/insights_and_knowledge/modern_dice/
[275] https://buck2.build/docs/developers/architecture/buck1_vs_buck2/
[276] https://buck2.build/docs/rfcs/implemented/bxl/
[277] https://buck2.build/docs/concepts/isolation_dir/
[278] https://buck2.build/docs/about/bootstrapping/
[279] https://buck2.build/docs/about/why/
[280] https://buck2.build/docs/api/
[281] https://buck2.build/docs/api/build/AnalysisActions/
[282] https://ndmitchell.com/downloads/slides-buck2_for_ocaml_users_and_developers-09_sep_2023.pdf
[283] https://dpe.org/wp-content/uploads/2023/12/Neil-Mitchell_Accelerating-Builds-with-Buck2_PDF.pdf
[284] https://buck2.build/docs/concepts/configurations/
[285] https://buck2.build/docs/why/
[316] https://man.archlinux.org/man/watchexec.1
[317] https://man.archlinux.org/man/extra/watchexec/watchexec.1.en
[318] https://linuxcommandlibrary.com/man/watchexec
[319] https://manpages.ubuntu.com/manpages/trusty/man1/entr.1.html
[320] https://linuxcommandlibrary.com/man/entr
[321] https://kx.cloudingenium.com/en/watchexec-file-watcher-run-commands-on-change-guide
[322] https://watchexec.github.io/
[323] https://www.pistack.xyz/posts/2026-05-22-self-hosted-file-watch-tools-watchexec-vs-entr-vs-inotify-tools-guide/
[324] https://docs.rs/crate/watchexec/latest/source/CHANGELOG.md
[325] http://cheatsheetshero.com/user/all/872-watchexec-cheatsheet.pdf
[326] https://tech.stonecharioteer.com/posts/2025/til-watchexec/
[327] https://deepwiki.com/watchexec/cargo-watch/3-user-guide
[328] https://kx.cloudingenium.com/en/watchexec-execute-commands-file-changes-automatically-guide
[329] https://eradman.com/entrproject/
[330] https://www.libhunt.com/compare-entr-vs-watchexec
[346] https://esbuild.github.io/api/
[347] https://www.grizzlypeaksoftware.com/library/vite-for-development-fast-builds-and-hmr-804sgphu
[348] https://github.com/evanw/esbuild/pull/2816
[349] https://vite.dev/guide/features
[350] https://romellem.github.io/turbo-v1-docs/pack
[351] https://tessl.io/registry/tessl/npm-esbuild/0.25.0/files/docs/build-context.md
[352] https://vite.dev/guide/api-hmr
[353] https://www.infoq.com/news/2025/09/nextjs-15-5-ships/
[354] https://frontend-build-tooling.com/esbuild-turbopack-workflows/esbuild-api-and-cli-for-rapid-builds/
[355] https://nextjs.org/docs/14/architecture/turbopack
[356] https://github.com/evanw/esbuild/releases/tag/v0.17.0
[357] https://stackoverflow.com/questions/72971679/get-esbuild-to-watch-for-changes-rebuild-and-restart-express-server
[358] https://www.slideshare.net/slideshow/viteenpdf/254109337
[359] https://nextjs.org/docs/pages/api-reference/turbopack
[360] https://nextjslaunchpad.com/article/nextjs-turbopack-default-bundler-guide
[331] https://jade.fyi/blog/the-postmodern-build-system/
[332] https://news.ycombinator.com/item?id=28244665
[333] https://arxiv.org/pdf/2604.11398
[334] https://news.ycombinator.com/item?id=28241661
[335] https://garnix.io/blog/incremental-builds/
[336] https://discourse.nixos.org/t/a-look-at-nixos-nixpkgs-evaluation-times-over-the-years/65114
[337] https://jade.fyi/blog/nix-evaluation-blocking/
[338] https://github.com/NixOS/nix/issues/4847
[339] https://github.com/NixOS/nix/issues/10862
[340] https://jade.fyi/blog/nixcon2022-retrospective/
[341] https://nix.dev/manual/nix/2.18/command-ref/new-cli/nix3-build
[342] https://garnix.io/blog/garn2/
[343] https://determinate.systems/blog/changelog-determinate-nix-3111/
[344] https://github.com/NixOS/nixpkgs/issues/57477
[345] https://discourse.nixos.org/t/incremental-builds/9988
[361] https://bazel.build/advanced/performance/json-trace-profile
[362] https://bazel.build/versions/7.3.0/advanced/performance/json-trace-profile
[363] https://bazel.build/versions/8.5.0/advanced/performance/json-trace-profile?hl=en
[364] https://bazel.build/versions/7.1.0/advanced/performance/json-trace-profile
[365] https://blog.bazel.build/2022/11/15/build-performance-metrics.html
[366] https://bazel.build/versions/8.1.1/advanced/performance/json-trace-profile
[367] https://bazel.build/versions/7.0.0/advanced/performance/json-trace-profile
[368] https://bazel.build/versions/9.1.0/advanced/performance/json-trace-profile
[369] https://www.hermetiq.com/blog/how-to-speed-up-bazel-builds
[370] https://github.com/brentleyjones/brentley.dev/blob/main/src/posts/2023-12-11-What's-New-in-Bazel-7.0.md
[371] https://bazel.build/remote/bep-glossary
[372] https://bazel.build/remote/bep
[373] https://docs.bzl.io/events
[374] https://analyzer.engflow.com/
[375] https://docs.engflow.com/re/faq.html
[376] https://docs.bazel.build/versions/3.1.0/skylark/performance.html
[377] https://docs.bazel.build/versions/main/skylark/performance.html
[378] https://bazel.build/versions/8.0.0/advanced/performance/json-trace-profile
[379] https://docs.bazel.build/versions/2.0.0/skylark/performance.html
[380] https://bazel.build/advanced/performance/build-performance-breakdown
[381] https://bazel.build/versions/8.7.0/advanced/performance/json-trace-profile
[382] https://bazel.build/versions/8.5.0/advanced/performance/json-trace-profile?hl=en
[383] https://groups.google.com/g/bazel-discuss/c/I1vAtH0uZ1M
[384] https://bazel.build/advanced/performance/json-trace-profile
[385] https://bazel.build/versions/9.0.0/advanced/performance/build-performance-metrics
[386] https://bazel.build/advanced/performance/iteration-speed
[387] https://bazel.build/versions/7.0.0/advanced/performance/json-trace-profile
[388] https://bazel-docs-staging.netlify.app/versions/master/guide.html
[389] https://skillstore.io/skills/wshobson-bazel-build-optimization
[390] https://bazel.build/versions/8.4.0/advanced/performance/build-performance-metrics
[391] https://develocity.ai/scans/gradle/
[392] https://docs.develocity.ai/2025.3/using-develocity/build-scan/
[393] https://gradle.com/develocity/releases/2021.2
[394] https://docs.develocity.ai/2026.1/using-develocity/build-scan/
[395] https://docs.gradle.com/enterprise/tutorials/performance-build-scans/
[396] https://github.com/palantir/gradle-guide/blob/develop/guide/diagnosing-build-performance.md
[397] https://docs.develocity.ai/gradle/4.5/
[398] https://gradle.com/develocity/releases/2024.2
[399] https://docs.gradle.org/current/userguide/inspect.html
[400] https://gradle.com/develocity/releases/2025.2
[401] https://gradle.com/develocity/releases/2018.4?_ga=2.171548903.634104509.1676406800-1992801467.1676406800
[402] https://newsletter.gradle.org/2025/10
[403] https://gradle.com/develocity/legacy-releases/2024.1
[404] https://www.buildbuddy.io/blog/whats-new-in-bazel-6-0/
[405] https://github.com/buildbuddy-io/buildbuddy/blob/master/README.md
[436] https://news.ycombinator.com/item?id=24877034
[437] https://www.computer.org/csdl/magazine/so/2023/04/10176199/1OAJyfknInm
[438] https://www.reddit.com/r/programming/comments/zgr0ak/experiment_the_hidden_costs_of_waiting_on_slow/
[439] https://stonetusker.com/newsletters/
[440] https://www.industriallogic.com/blog/how-long-is-too-long-for-a-build/
[441] https://stackoverflow.co/internal/resources/your-developers-deserve-better-insights-from-the-2024-developer-survey/
[442] https://www.simplethread.com/dear-client-heres-why-that-change-took-so-long/
[443] https://www.mediawiki.org/wiki/Developer_Satisfaction_Survey/2024/Continuous_integration
[444] https://aws.amazon.com/solutions/case-studies/depot-dev/
[445] https://www.incredibuild.com/blog/cicd-pipeline-statistics-trends-2026
[446] https://softwareengineering.stackexchange.com/questions/394027/when-building-time-takes-so-long-how-to-improve-engineers-efficiency
[447] https://search.library.wisc.edu/article/cdi_proquest_journals_3082737197
[448] https://dl.acm.org/doi/10.5555/2820678.2820680
[449] https://dev-tester.com/the-real-cost-of-a-slow-ci-build/
[450] https://survey.stackoverflow.co/
[406] https://services.google.com/fh/files/misc/2024_final_dora_report.pdf
[407] https://research.google/pubs/dora-accelerate-state-of-devops-2024-report/
[408] https://dora.dev/research/2024/dora-report/
[409] https://www.askantech.com/cicd-pipeline-optimization-reduce-build-times/
[410] https://fortegrp.com/insights/the-2024-dora-report-key-insights-for-devops-and-platform-engineering
[411] https://journal.idscipub.com/index.php/digitus/article/view/952
[412] https://services.google.com/fh/files/misc/dora_one_pager_2024.pdf
[413] https://www.gitclear.com/research/google_dora_2024_summary_ai_impact
[414] https://getdx.com/blog/dora-metrics/
[415] https://techdebt.guru/devops-debt/
[416] https://www.linkedin.com/pulse/2024-dora-report-summary-laura-tacho-3bzvf
[417] https://dora.dev/ai/
[418] https://www.computer.org/csdl/magazine/so/2023/04/10176199/1OAJyfknInm
[419] https://dora.dev/guides/dora-metrics/
[420] https://devopslaunchpad.com/blog/dora-report-2024/
[421] https://www.computer.org/csdl/magazine/so/2023/04/10176199/1OAJyfknInm
[422] https://www.incredibuild.com/survey-report-2022
[423] https://dl.acm.org/doi/10.5555/2820678.2820680
[424] https://coderefinery.nz/2021/11/07/slow-builds-are-an-out-sized-business-opportunity/
[425] https://dl.acm.org/doi/abs/10.1109/MS.2023.3275268
[426] https://research.google/pubs/developer-productivity-for-humans-part-4-build-latency-predictability-and-developer-productivity/
[427] https://2020.icse-conferences.org/details/icse-2020-Journal-First/71/An-Empirical-Study-of-the-Long-Duration-of-Continuous-Integration-Builds
[428] https://www.incredibuild.com/wp-content/uploads/2025/03/2025-Build-Time-Survey-Report-.pdf
[429] https://dl.acm.org/doi/10.1007/s10664-019-09695-9
[430] https://seal-queensu.github.io/publications/pdf/EMSE-Taher-2019.pdf
[431] https://seal-queensu.github.io/publications/pdf/TSE-Taher-2022.pdf
[432] https://arxiv.org/pdf/2308.10078
[433] https://www.microsoft.com/en-us/research/wp-content/uploads/2016/02/meyer-fse-2014.pdf
[434] https://www.computer.org/csdl/proceedings-article/techdebt/2018/571301a105/17D45VUZMTY
[435] https://www.honeycomb.io/blog/most-important-developer-productivity-metric-build-times
[466] https://www.warpbuild.com/
[467] https://news.ycombinator.com/item?id=26565632
[468] https://aws.amazon.com/solutions/case-studies/depot-dev/
[469] https://news.ycombinator.com/item?id=29562557
[470] https://www.ycombinator.com/companies/attune
[471] https://stackoverflow.com/questions/68992552/how-to-make-llvm-project-compile-faster
[472] https://www.reddit.com/r/rust/comments/76jq7h/long_time_to_compile_rustc/
[473] https://newsletter.getdx.com/p/build-times-and-developer-productivity
[474] https://pushbuildtestdeploy.com/9-ways-to-speed-up-your-ci/cd-pipelines/
[475] https://www.tobias-weiss.org/content/devops/ci-cd-pipeline-optimization/
[476] https://www.linkedin.com/pulse/7-tips-how-speed-up-your-build-pipeline-s%C3%B8ren-pedersen
[477] https://softwareengineering.stackexchange.com/questions/430633/how-is-it-possible-to-have-an-efficient-edit-compile-try-cycle-on-large-codebase
[478] https://medium.com/silicon-tribe-techdev/7-ways-to-run-faster-ci-cd-builds-5108a3f2f5d6
[479] https://learn.microsoft.com/en-nz/azure/well-architected/operational-excellence/release-engineering-performance
[480] https://stackoverflow.com/questions/8633313/improving-ci-build-time-net
[481] https://www.atlassian.com/software/compass/resources/state-of-developer-2024
[482] https://www.incredibuild.com/blog/signs-pipeline-needs-build-acceleration
[483] https://www.jetbrains.com/lp/devecosystem-2024/
[484] https://www.jetbrains.com/help/teamcity/working-with-build-queue.html
[485] https://medium.com/build-performance/why-platform-teams-are-focused-on-ci-cd-optimization-1df12fa1b9e3
[486] https://www.atlassian.com/blog/development/developer-experience-report-2024
[487] https://reinvent.awsevents.com/content/dam/reinvent/2024/slides/dop/DOP105-S_The-state-of-developer-experience-with-Atlassian-engineering-leaders-sponsored-by-Atlassian.pdf
[488] https://blog.jetbrains.com/teamcity/2022/05/solving-the-build-queue-bottleneck-problem/
[489] https://blog.jetbrains.com/team/2024/12/11/the-state-of-developer-ecosystem-2024-unveiling-current-developer-trends-the-unstoppable-rise-of-ai-adoption-leading-languages-and-impact-on-developer-experience/
[490] https://www.incredibuild.com/blog/cicd-pipeline-statistics-trends-2026
[491] https://dam-cdn.atl.orangelogic.com/AssetLink/v26bjt7fvawounqfnlx4745445ng160f/fl_keep_metadata/CSD-7616_Document_ADO_State_of_DevEx_Survey-FINAL.pdf
[492] https://intellij-support.jetbrains.com/hc/en-us/community/posts/21055856324754-Build-time-and-runtime-configuration-start-time-very-slow-IntelliJ-2024-2-x?page=2
[493] https://www.incredibuild.com/wp-content/uploads/2025/03/2025-Build-Time-Survey-Report-.pdf
[494] https://www.oreilly.com/radar/coding-was-never-a-bottleneck/
[495] https://www.atlassian.com/webinars/software/deep-dive-state-of-developer-experience-report-2024
[451] https://www.computer.org/csdl/magazine/so/2023/04/10176199/1OAJyfknInm
[452] https://research.google/pubs/developer-productivity-for-humans-part-4-build-latency-predictability-and-developer-productivity/
[453] https://storage.googleapis.com/gweb-research2023-media/pubtools/6862.pdf
[454] https://research.google/blog/ml-enhanced-code-completion-improves-developer-productivity/
[455] https://habr.com/en/articles/1076016/
[456] https://cloud.google.com/blog/products/application-development/build-containers-faster-with-cloud-build-with-kaniko
[457] https://jmmv.dev/2021/02/google-monorepos-and-caching.html
[458] https://arxiv.org/html/1805.05504v1
[459] https://shiftmag.dev/measuring-developer-productivity-per-googles-research-3739/
[460] https://dpe.org/sessions/ciera-jaspan-collin-green/understanding-and-measuring-developer-productivity/
[461] https://arxiv.org/pdf/2102.09098
[462] https://arxiv.org/pdf/1707.00794
[463] https://dl.acm.org/doi/10.1145/3084100.3084116
[464] https://lifetips.alibaba.com/tech-efficiency/sharemouse-controls-multiple-computers-with-a-single-ke
[465] https://boa.unimib.it/retrieve/d087c5cc-f27e-4173-912f-5ed464bb96ed/Cornejo-2024-SQJ-VoR.pdf
[511] https://devblogs.microsoft.com/bharry/the-largest-git-repo-on-the-planet/
[512] https://www.theregister.com/software/2017/05/25/windows-is-now-built-on-git-but-microsoft-has-found-some-bottlenecks/1512663
[513] https://www.infoq.com/news/2017/05/microsoft-windows-git/
[514] https://devblogs.microsoft.com/bharry/perf-results-on-scaling-git-on-vsts-with-gvfs/
[515] https://indico.cern.ch/event/608592/contributions/2830118/attachments/1596893/2545626/GVFS_-_CERN_VM_workshop_2018.pdf
[516] https://arstechnica.com/information-technology/2017/05/90-of-windows-devs-now-using-git-creating-1760-windows-builds-per-day/
[517] https://news.ycombinator.com/item?id=15723926
[518] https://devblogs.microsoft.com/devops/announcing-gvfs-git-virtual-file-system/
[519] https://blog.gitbutler.com/git-tips-3-really-large-repositories
[520] https://www.drwindows.de/news/12747-gvfs-microsoft-nennt-neue-details-git-dateisystem
[521] https://stackoverflow.com/questions/17888604/git-with-large-files
[522] https://gigazine.net/gsc_news/en/20170208-microsoft-gvfs-problem/
[523] https://news.mynavi.jp/techplus/article/20170526-a194/
[524] https://devblogs.microsoft.com/devops/optimizing-git-beyond-gvfs/
[525] https://www.infoq.com/news/2017/02/GVFS/
[526] https://github.com/facebook/sapling/blob/main/eden/fs/docs/Glossary.md
[527] https://github.com/facebook/sapling/blob/main/eden/fs/docs/Overview.md
[528] https://github.com/DurhamG/eden/blob/main/eden/fs/docs/Overview.md
[529] https://github.com/kmancini/eden
[530] https://github.com/facebook/sapling/blob/main/eden/fs/docs/Process_State.md
[531] https://github.com/facebook/sapling/blob/main/eden/fs/docs/Redirections.md
[532] https://github.com/facebook/sapling/blob/main/eden/fs/docs/InodeStorage.md
[533] https://github.com/kmancini/eden/blob/main/eden/fs/docs/Overview.md
[534] https://github.com/facebook/sapling/blob/main/eden/fs/docs/Data_Model.md
[535] https://news.ycombinator.com/item?id=24454777
[536] https://www.linkedin.com/posts/ankur-raj-921b54124_git-sapling-mercurial-activity-7484822656601546752-8rMO
[537] https://engineering.fb.com/2023/06/27/developer-tools/meta-developer-tools-open-source/
[538] https://news.ycombinator.com/item?id=45668160
[539] https://github.com/facebook/sapling/blob/main/eden/fs/docs/Inodes.md
[540] https://www.linkedin.com/posts/astitvaaggarwal_devops-monorepo-sourcecontrol-activity-7475034920847945728-M9a7
[496] https://research.google/pubs/why-google-stores-billions-of-lines-of-code-in-a-single-repository/
[497] https://www.uncommonengineer.com/docs/books/papers/Google3/
[498] https://paper.lingyunyang.com/reading-notes/journal/communications-of-the-acm/2015/why-google-stores-billions-of-lines-of-code-in-a-single-repository
[499] https://github.com/florian/reading-notes/blob/master/papers/003_Why_Google_Stores_Billions_of_Lines_of_Code_in_a_Single_Repository.md
[500] https://dl.acm.org/doi/10.1145/3183519.3183550
[501] https://barghouthi.github.io/repsatsixty/larus.pdf
[502] https://research.google/pubs/the-google-file-system/
[503] https://research.google/pubs/?category=distributed-systems-and-parallel-computing
[504] https://courses.cs.vt.edu/cs3304/fall20meng/lecture_notes/p13.pdf
[505] https://courses.cs.vt.edu/cs3704/spring22/student_presentations/Group-4.pdf
[506] https://news.ycombinator.com/item?id=19303082
[507] https://research.google/pubs/the-anatomy-of-a-large-scale-hypertextual-web-search-engine/
[508] https://research.google/pubs/
[509] https://daily.dev/posts/why-google-stores-billions-of-lines-of-code-in-a-single-repository-2016--gwsc2jk70
[510] https://stateful.com/pdf/adv-dis-monolithic-repo.pdf
[571] https://blaze-monorepo.dev/docs/guides/get-started/
[572] https://aws.github.io/aws-pdk/developer_guides/monorepo/dependency_visualization.html
[573] https://sourcegraph.com/blog/monorepo-build-tools
[574] https://www.twosapp.com/5d9e4615836e22f288f48e55
[575] https://sesamedisk.com/monorepo-tools-ci-cd-2026/
[576] https://www.uncommonengineer.com/docs/books/papers/Google3/
[577] https://medium.com/@erfan.mohebi/solving-monorepo-hell-with-bazel-a-deep-dive-into-modern-build-systems-f70c831bb227
[578] https://www.wix.engineering/post/virtual-monorepo-for-bazel
[579] https://github.com/google/sge-monorepo
[580] https://dev.to/alex_aslam/cicd-for-monorepos-taming-the-beast-with-smart-strategies-3np0
[581] https://www.uber.com/us/en/blog/how-we-halved-go-monorepo-ci-build-time/
[582] https://paulhammant.com/2015/05/20/turning-bazel-back-into-blaze-for-monorepo-nirvana/
[583] https://www.reddit.com/r/rust/comments/1feytpt/repost_blaze_a_simple_and_flexible_monorepo_based/
[584] https://buildkite.com/resources/blog/continuous-integration-for-monorepos/
[585] https://medium.com/pinterest-engineering/how-we-designed-our-continuous-integration-system-to-be-more-than-50-faster-b70a59342fe2
[556] https://webpack.js.org/guides/hot-module-replacement/
[557] https://webpack.js.org/api/hot-module-replacement/
[558] https://docs.webpack.js.org/plugins/hot-module-replacement-plugin
[559] https://webpack.js.org/blog/2026-06-25-webpack-5-108/
[560] https://webpack.js.org/concepts/hot-module-replacement/
[561] https://stackoverflow.com/questions/42679353/how-to-do-incremental-builds-with-webpack-across-each-invocation-from-the-comma
[562] https://stackoverflow.com/questions/42679353/how-to-do-incremental-builds-with-webpack-across-each-invocation-from-the-comma/42679774
[563] https://stackoverflow.com/a/42679774
[564] https://webpack.js.org/guides/build-performance/
[565] https://docs.webpack.js.org/guides/development
[566] https://stephenzhao.github.io/webpack-cn/docs/hot-module-replacement.html
[567] https://webpack.js.org/configuration/experiments/
[568] https://webpack.kr/guides/hot-module-replacement/
[569] https://webpack.js.org/configuration/watch/
[570] https://webpack.js.org/glossary/
[541] https://github.com/microsoft/TypeScript/wiki/Using-the-Compiler-API
[542] https://github.com/microsoft/TypeScript/wiki/Architectural-Overview/1afea54fbb7a4af15d613708ac0d1951f73aca14
[543] https://github.com/microsoft/TypeScript/wiki/
[544] https://github.com/microsoft/TypeScript/wiki/Standalone-Server-(tsserver)
[545] https://github.com/microsoft/TypeScript/wiki/Using-the-Language-Service-API/461b47978a9c5a89b184e34dc555f95da78af22f
[546] https://github.com/microsoft/TypeScript/wiki/README/e6332a972e6dda8d75792376b7e53b240ae99a26
[547] https://github.com/microsoft/TypeScript/wiki/Codebase-Compiler-Services
[548] https://github.com/microsoft/TypeScript/wiki/Debugging-Language-Service-in-VS-Code
[549] https://github.com/microsoft/TypeScript/wiki/Integrating-with-Build-Tools
[550] https://github.com/microsoft/typescript/wiki/using-the-language-service-api
[551] https://github.com/microsoft/TypeScript/wiki/Writing-a-Language-Service-Plugin
[552] https://deepwiki.com/microsoft/TypeScript/5.2-project-management
[553] https://github.com/microsoft/TypeScript/blob/main/src/tsserver/server.ts
[554] https://github.com/microsoft/TypeScript/issues/54222
[555] https://deepwiki.com/microsoft/TypeScript/8-compiler-api
[601] https://bazel.build/advanced/performance/build-performance-metrics?hl=zh-cn
[602] https://bazel.build/versions/7.3.0/advanced/performance/build-performance-metrics
[603] https://bazel.build/versions/8.4.0/advanced/performance/build-performance-metrics
[604] https://bazel.build/versions/8.5.0/advanced/performance/build-performance-metrics?hl=en
[605] https://bazel.build/versions/9.0.0/advanced/performance/build-performance-metrics
[606] https://bazel.build/advanced/performance/build-performance-metrics?hl=ja
[607] https://bazel.build/advanced/performance/build-performance-metrics?hl=es-419
[608] https://bazel.build/advanced/performance/build-performance-metrics?hl=pt-br
[609] https://bazel.build/advanced/performance/build-performance-metrics?hl=hi
[610] https://bazel.build/advanced/performance/build-performance-metrics?hl=ko
[611] https://bazel.build/advanced/performance/build-performance-metrics?authuser=3&hl=tr
[612] https://bazel.build/reference/command-line-reference
[613] https://bazel.build/docs/user-manual
[614] https://bazel.googlesource.com/bazel/+/refs/tags/3.3.1/src/tools/execlog/README.md
[615] https://bazel.virtuslab.com/book/4~4~6/
[586] https://www.buildbuddy.io/blog/buildbuddy-v1-3-0-release-notes/
[587] https://www.buildbuddy.io/changelog/
[588] https://www.buildbuddy.io/docs/quickstart/
[589] https://github.com/buildbuddy-io/buildbuddy/blob/master/README.md
[590] https://www.buildbuddy.io/
[591] https://www.buildbuddy.io/blog/meet-buildbuddy/
[592] https://deepwiki.com/buildbuddy-io/buildbuddy/3.1-web-ui
[593] https://deepwiki.com/buildbuddy-io/buildbuddy/4.1-build-event-protocol
[594] https://github.com/brentleyjones/brentley.dev/blob/main/src/posts/2023-12-11-What's-New-in-Bazel-7.0.md
[595] https://www.buildbuddy.io/blog/introducing-buildbuddy-v1/
[596] https://bazel.build/remote/bep-glossary
[597] https://www.buildbuddy.io/blog/bazels-remote-caching-and-remote-execution-explained/
[598] https://www.buildbuddy.io/changelog/bb-explain-profile/
[599] https://www.buildbuddy.io/docs/introduction/
[600] https://github.com/buildbuddy-io/buildbuddy-foss
[616] https://github.blog/engineering/infrastructure/improve-git-monorepo-performance-with-a-file-system-monitor/
[617] https://news.ycombinator.com/item?id=37291765
[618] https://www.canva.dev/blog/engineering/we-put-half-a-million-files-in-one-git-repository-heres-what-we-learned/
[619] https://www.grizzlypeaksoftware.com/library/git-performance-optimization-for-large-repositories-dxifo1ab
[620] https://kindatechnical.com/git-version-control/performance-troubleshooting-large-repos.html
[621] https://sapling-scm.com/docs/scale/axes/
[622] https://dropbox.tech/application/speeding-up-a-git-monorepo-at-dropbox-with--200-lines-of-code
[623] https://www.grizzlypeaksoftware.com/library/git-performance-optimization-for-large-repositories-nxxd5xrq
[624] https://code.googlesource.com/git/+/refs/tags/v2.17.5/templates/hooks--fsmonitor-watchman.sample
[625] https://stackoverflow.com/questions/3313908/git-is-really-slow-for-100-000-objects-any-fixes
[626] https://www.infoq.cn/article/tomhtgpmuy4oqhpvf0w1
[627] https://runbook.fyi/Git/monorepos-and-scaling/
[628] https://news.ycombinator.com/item?id=37295378
[629] https://stackoverflow.com/questions/8735875/is-it-ok-performance-wise-to-have-hundreds-or-thousands-of-files-in-the-same-l
[630] https://github.com/restic/restic/issues/4828
[631] https://manpages.opensuse.org/Tumbleweed/man-pages/fanotify.7.en.html
[632] https://www.pistack.xyz/posts/2026-05-25-self-hosted-linux-file-monitoring-fanotify-vs-inotify-vs-fsnotify-guide/
[633] https://kernel-internals.org/vfs/inotify/
[634] https://crackingwalnuts.com/linux/inotify
[635] https://docs.kernel.org/admin-guide/filesystem-monitoring.html
[636] https://linuxvox.com/blog/best-way-to-monitor-file-system-changes-in-linux/
[637] https://news.ycombinator.com/item?id=13865484
[638] https://lwn.net/Articles/896055/
[639] https://oneuptime.com/blog/post/2026-03-02-configure-fanotify-file-access-monitoring-ubuntu/view
[640] https://github.com/parcel-bundler/watcher/issues/87
[641] https://www.systemshardening.com/articles/linux/linux-fanotify-security-monitoring/
[642] https://stackoverflow.com/questions/1835947/how-do-i-program-for-linuxs-new-fanotify-file-system-monitoring-feature
[643] https://mathscantor.github.io/posts/linux-file-monitoring-with-fanotify/
