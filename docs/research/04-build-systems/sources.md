<!-- Raw research source. Generated via Perplexity `perplexity_research` (web-grounded,
     multi-source) on 2026-09-01 for the liquid project. Lightly reformatted only.
     Treat as evidence, not as liquid design decisions - see README.md in this
     directory for the curated analysis and the implications drawn for liquid. -->

# Modern build systems, 2024–2026

**Bottom line:** modern build performance is mostly a dependency-accuracy and data-movement problem. Bazel and Buck2 gain correctness and fleet-scale reuse by making actions content-addressed and remotely executable; CMake, Meson, and Autotools primarily *generate* lower-level builds; Ninja and Make execute file graphs; Gradle and MSBuild operate at coarser task/target granularity. No amount of parallelism fixes an incomplete dependency graph, and remote execution can be slower than local execution when analysis, queueing, hashing, or network transfer dominates.

---

## 1. Bazel

### 1.1 Loading, analysis, action graph, execution

A Bazel build is best understood as three logical stages:

1. **Loading:** parse `BUILD`/`.bzl` files and construct the target graph.
2. **Analysis:** resolve configurations, transitions, toolchains, aspects and providers; invoke each rule implementation; emit actions and artifacts.
3. **Execution:** schedule actions whose outputs are required and not already cached.

The resulting **action graph** is a bipartite graph:

- **Action nodes:** compiler, linker, generator, test, archive or other command invocations.
- **Artifact nodes:** source files, generated intermediates and final outputs.

A target is not an action: one target may emit many actions, while aliases or aggregation targets may emit none. Bazel’s analysis phase converts configured targets into this action graph; Starlark rule implementations register actions but do not execute external commands during analysis. [web:382][web:391]

This separation is central to both Bazel’s strength and its major scaling weakness: execution can be massively distributed, but loading and analysis still happen in the Bazel server process before most execution work is available. Remote execution therefore does not automatically cure a 30-second analysis phase. [web:15]

### 1.2 Skyframe

**Skyframe** is Bazel’s parallel incremental computation engine. Computations are represented by `SkyKey → SkyValue` functions. A function requests other keys instead of reading undeclared state directly, allowing Skyframe to record a dependency graph and invalidate the reverse transitive closure of changed inputs. Bazel currently primarily uses bottom-up invalidation, which requires checking input state from the prior graph. [web:376][web:378]

Skyframe caches substantially more than compiled files:

- package and `glob()` evaluation;
- loaded `.bzl` modules;
- configured targets and providers;
- toolchain/configuration resolution;
- the generated action graph;
- filesystem-derived values.

That is why a warm Bazel server can be dramatically faster than a cold server, and why `bazel shutdown`, configuration changes, large `.bzl` edits or memory-pressure flags can make the next build unexpectedly expensive. Bazel explicitly documents that discarding the analysis cache reduces memory but makes subsequent builds redo analysis; Skyfocus attempts to retain only a working subset of Skyframe state. [web:5][web:145]

### 1.3 Starlark

Starlark is a deliberately restricted, deterministic Python-like language. In Bazel:

- `BUILD` files instantiate rules.
- `.bzl` files define macros, rules, aspects, transitions and repository/module-extension logic.
- Rule implementations receive `ctx`, consume attributes and providers, declare outputs, and call `ctx.actions.*`.
- Returned values become immutable after their evaluation context, limiting cross-target mutable state. [web:393][web:396]

The restriction against arbitrary analysis-time I/O is important: if rule analysis could inspect arbitrary files or run arbitrary programs, Skyframe could not know what invalidates the result. Actual generators therefore become execution-phase actions with declared inputs and outputs.

Starlark itself is hermetic and deterministic, but a **Starlark-defined action is not automatically hermetic**. A rule can still launch a shell script that reads `/usr/include`, inherits `PATH`, checks the clock, uses the network, embeds an absolute path, or invokes an undeclared host tool.

### 1.4 Hermeticity

A hermetic action should be a pure function:

\[
\text{outputs}=f(\text{declared inputs},\text{command},\text{environment},\text{toolchain},\text{platform})
\]

Bazel tries to enforce this through:

- explicit inputs, tools and outputs;
- sandboxed local execution;
- pinned external repositories;
- declared platforms and toolchains;
- controlled environment variables;
- remote workers with defined execution environments.

Sandboxing prevents many undeclared reads by exposing only the declared execution root. Without it, an undeclared header can change while Bazel still considers the action current, producing an incorrect incremental result. [web:115]

Common escapes include:

- `--spawn_strategy=local` or unsandboxed custom actions;
- `/usr/bin`, system SDKs or libraries reached through absolute paths;
- inherited environment variables;
- timestamps, randomness, hostnames and locale;
- network downloads during an action;
- compiler wrappers whose configuration is not declared;
- repository rules or module extensions whose environmental inputs are not correctly recorded;
- tools that mutate undeclared state or write outside their output tree.

The painful consequence is that a non-hermetic action can poison a shared cache: the same nominal action key may correspond to machine-dependent output.

### 1.5 REAPI v2, CAS and action digests

The Remote Execution API v2 divides remote build infrastructure into several services:

- **Content Addressable Storage (CAS):** immutable blobs addressed by a `Digest`, conceptually `(hash, size)`.
- **Action Cache (AC):** maps an action digest to an `ActionResult`.
- **Execution:** accepts an action digest, queues the action and streams status/results.
- Supporting services include ByteStream transfers, capabilities and operations.

The core data model is:

```text
Action
 ├── command_digest ──> Command
 │                       ├── argv
 │                       ├── environment
 │                       ├── output paths
 │                       └── platform/execution details
 └── input_root_digest ─> Directory Merkle-tree root
                          ├── files: name + content digest + executable bit
                          └── child directories: name + Directory digest
```

The cache key is the digest of the canonical serialized `Action`. The `Action` refers to the command and input-root digests and may include timeout and other execution semantics. An `ActionResult` is looked up under the action digest; output files themselves live in CAS under their own content digests. [web:2][web:362][web:368]

The normal protocol is:

1. Hash files and construct the input Merkle tree.
2. Ask the CAS which blobs are missing.
3. Upload missing files, directory protos, `Command` and `Action`.
4. Query the action cache.
5. On a hit, obtain output digests.
6. On a miss, call `Execute`.
7. A scheduler matches platform requirements to a worker.
8. The worker downloads inputs, constructs a sandbox, executes, and uploads outputs.
9. Store the resulting `ActionResult` under the action digest.

Merkle trees provide deduplication: changing one leaf changes its ancestors up to the root, but unchanged files and subtrees retain their digests and need not be uploaded again.

### 1.6 Remote caching versus remote execution

| Mode | Cache hit | Cache miss | Main benefit |
|---|---|---|---|
| Local action cache | Reuse local output | Execute locally | Fast single-machine incrementality |
| Remote cache | Download reusable output | Execute locally, usually upload result | Reuse across clean workspaces and developers |
| Remote execution | Download or defer output | Upload missing inputs and run on worker | Fleet parallelism, isolation and shared toolchains |
| Hybrid/dynamic execution | Reuse cache; otherwise race or choose local/remote | Fastest eligible path wins | Avoid sending tiny or latency-sensitive actions remotely |

Remote cache effectiveness depends on **semantic stability**, not simply repeated source code:

- toolchain, platform or flag changes alter action keys;
- absolute paths or unstable environment variables reduce hits;
- stamping commit IDs or timestamps broadly can invalidate many actions;
- non-deterministic outputs prevent dependable reuse;
- branch divergence reduces cross-developer overlap.

### 1.7 The analysis-phase bottleneck

For large graphs, analysis can dominate because Bazel must:

- instantiate configured targets;
- evaluate Starlark rules and aspects;
- propagate providers;
- resolve toolchains;
- process `select()` values and configuration transitions;
- allocate action and artifact objects;
- maintain Skyframe dependency edges.

Split transitions can multiply one logical target into many configured targets. Wide aspects—coverage, IDE extraction, linting or language indexing—can traverse nearly the entire graph. This work consumes CPU and substantial heap before remote workers help. Bazel warns that sufficiently large builds can OOM and exposes flags that trade retained analysis state for slower incremental builds. [web:145]

Practical remedies are:

- reduce unnecessary transitions and configuration dimensions;
- avoid enormous macros and providers;
- narrow aspects and queried target sets;
- profile with the Build Event Protocol/profile and inspect `targets_configured`;
- keep the Bazel server warm;
- use analysis cache/Skyfocus rather than routinely expunging;
- split monolithic targets where this reduces configured-graph fan-out—but not so finely that target count explodes.

### 1.8 What developers hate about Bazel

| Complaint | Technical cause and consequence |
|---|---|
| **Learning curve** | Users must understand labels, packages, providers, configurations, platforms, execution platforms, toolchains, repository/module extensions, visibility, runfiles and several query languages. A 2024 complaint said the team needed extra contractors and still struggled with minor CI changes. [web:121] |
| **`rules_*` churn** | Much language support lives in separately versioned rulesets. Bazel upgrades may therefore require coordinated upgrades of `rules_python`, `rules_go`, Gazelle, protobuf rules and toolchains; their compatibility windows and breaking changes are not identical. The `rules_python` support policy explicitly permits staged breaking changes, and its changelog shows cross-ruleset version bumps. [web:148][web:149] |
| **Toolchain configuration** | Toolchain type, declared toolchain, execution platform, target platform, constraints and registration must all line up. Failures frequently surface as the indirect “No matching toolchains found,” including cross-platform cases where the intended target is otherwise incompatible. [web:107][web:109] |
| **WORKSPACE → Bzlmod migration** | Bzlmod fixes `WORKSPACE`’s lack of real transitive version resolution, but migration changes repository names, visibility, extension evaluation and toolchain registration. Bazel 8 disabled `WORKSPACE` by default and Bazel 9 removed its implementation, turning deferred migration into mandatory engineering work. [web:133][web:14] |
| **Poor or uneven IDE integration** | IDEs must reproduce Bazel’s configured graph, generated sources and aspects. Support varies sharply by language: developers report missing Python third-party navigation/autocomplete and integration below Gradle/Maven quality; VS Code Java historically did not recognize Bazel classpaths. [web:138][web:147] |
| **Slow analysis and high memory** | Large configured graphs are held in the daemon. Cold analysis, broad aspects, transitions and huge target counts can dominate even when execution has 100% remote-cache hits. |
| **Hermeticity friction** | Legacy compilers and scripts expect the host filesystem. Under a sandbox, undeclared accesses become confusing failures; without a sandbox, they become silently wrong cache entries. |
| **Debuggability** | The visible failure may occur far from the cause: a provider mismatch, repository canonical name, transition, generated action, execution-platform mismatch or cache poisoning. |
| **“Bazel owns the world” effect** | Adoption often requires replacing package discovery, compiler invocation, code generation, tests and CI conventions—not merely writing a new build file. |

One representative Bzlmod complaint from a developer migrating a large monorepo described documentation as insufficient and errors as vague, requiring either prolonged troubleshooting or deep prior knowledge. [web:123]

---

## 2. Buck2

### 2.1 Architecture

Buck2 is Meta’s successor to Buck1. Its core is written in **Rust**, while all language rules are written in **Starlark**, keeping language-specific policy outside the engine. It supports local, remote and hybrid execution. [web:16][web:18]

Its central engine is **DICE**, the Distributed Incremental Computation Engine. Like Skyframe, DICE represents computations and their dependencies, memoizes results, shares concurrent requests for the same node and reevaluates affected nodes when injected inputs change. [web:406]

The important architectural distinction is that Buck2 presents the build as one demand-driven DICE computation graph rather than Bazel’s rigid global loading/analysis/execution barrier. Evaluation, configuration, analysis, action execution and materialization still exist conceptually, but they can interleave and need not proceed as complete sequential phases. [web:16][web:417]

### 2.2 Dynamic dependencies

Buck2 supports **dynamic/monadic dependencies**:

1. An initial action runs—for example, `ocamldep`, a ThinLTO planner or source scanner.
2. Its output describes the real fine-grained dependency graph.
3. A Starlark continuation reads that artifact.
4. Buck2 registers later actions with the discovered dependencies.

This is more expressive than Bazel’s ordinary analysis model, where rule analysis cannot read an artifact that has yet to be generated. Typical uses are OCaml/Haskell import graphs and distributed ThinLTO plans. Buck2 still generally requires the universe of potential input/output artifacts to be declared sufficiently for correctness and remote execution. [web:17][web:26]

### 2.3 Deferred materialization and remote execution

Buck2 can leave intermediate outputs in remote CAS and materialize them locally only when:

- the user requests the artifact;
- a local action needs it;
- another tool outside Buck2 needs the file.

On an action-cache hit, only the output digest initially needs to return. This avoids downloading a large intermediate merely to upload or consume it again remotely. Buck2 also supports hybrid execution, racing local and remote execution and using the first successful result. [web:16]

Buck2 was designed “remote execution first”: directory hashes can be prepared as graph data, and local execution is treated as another execution strategy. Its open-source implementation supports Bazel-compatible REAPI, while Meta uses additional internal infrastructure. [web:18][web:27]

### 2.4 Why Buck2 can beat Bazel—and when it does not

| Buck2 advantage | Why it can matter |
|---|---|
| Rust engine | Predictable native performance and no JVM garbage-collection pauses. |
| One incremental DICE graph | Fine-grained reuse can cross conceptual phase boundaries; Buck2 need not complete an entire analysis frontier before useful downstream work. |
| Dynamic dependencies | The generated dependency graph can remain fine-grained rather than collapsing a dynamically discovered language or ThinLTO plan into one coarse action. |
| RE-first data model | Input digests, remote results and deferred materialization are first-class rather than later additions. |
| Hybrid execution | Small actions can remain local while expensive actions use the fleet, or the two can race. |
| Starlark-only rules | Rule changes do not require rebuilding the engine, unlike Buck1’s Java-integrated rules. |

“Buck2 beats Bazel” is therefore an architectural possibility, not a universal benchmark result. Bazel may win where its rules ecosystem, IDE support, stable remote infrastructure and organizational expertise are already mature. Buck2’s open-source ecosystem remains smaller, and Meta’s internal performance includes proprietary rules and services not reproduced merely by installing the binary.

---

## 3. CMake and the rest of the conventional ecosystem

### 3.1 Why CMake is disliked

CMake is primarily a **meta-build system**: it configures a project and emits Ninja, Makefiles, Visual Studio projects or Xcode projects. Its portability and installed base are excellent; its language and package model carry decades of compatibility constraints.

#### Language and scoping

CMake values are fundamentally strings; lists are semicolon-separated strings. Quoting, list expansion, empty variables and command argument splitting can therefore change semantics unexpectedly. One representative complaint notes that `"a;b;c"` looks like a string but behaves as a list, nonexistent variables often expand silently, and quoting requirements depend on spaces. [web:91]

Scopes are also nonuniform:

- directory scopes inherit variables from parents;
- `function()` creates a scope;
- `macro()` performs textual-style argument substitution and does not create the expected function scope;
- cache variables are persistent and globally visible;
- directory properties, target properties, source properties and global properties follow different lookup rules.

This produces “action at a distance”: an old `include_directories()` or `add_definitions()` in a parent directory can affect unrelated descendants, while modern `target_*` commands attach state to explicit targets.

#### Target versus directory model

Modern CMake wants users to model libraries as targets:

```cmake
target_include_directories(lib
  PUBLIC  include
  PRIVATE src
)
target_link_libraries(app PRIVATE lib)
```

- **PRIVATE:** used only to build the target.
- **INTERFACE:** used only by consumers.
- **PUBLIC:** both.

These **usage requirements** propagate transitively through `INTERFACE_*` properties and the link graph. This is powerful but difficult to debug when a compile definition or include path arrives through several imported/interface targets. [web:52][web:55]

#### Generator expressions

Expressions such as:

```cmake
$<$<CONFIG:Debug>:DEBUG_BUILD>
$<TARGET_PROPERTY:foo,INTERFACE_INCLUDE_DIRECTORIES>
```

are evaluated during generation or build-system emission, not necessarily when the surrounding CMake code is configured. They have context-dependent target semantics and interact badly with spaces and semicolons unless quoted correctly. CMake’s own manual warns that argument splitting can cause an expression not to be recognized. [web:102]

#### `find_package`: Module versus Config

| Mode | Who supplies metadata | Behavior | Failure pattern |
|---|---|---|---|
| **Module** | CMake or consuming project | Executes `FindFoo.cmake`, heuristically searches headers/libraries and sets variables or imported targets | Heuristics age, version support varies, may find an unintended installation |
| **Config** | Package producer | Loads `FooConfig.cmake`, `foo-config.cmake` or newer package metadata and usually creates imported targets | Package may not ship config files; prefix and cross-compilation search paths remain difficult |

CMake describes Config mode as more reliable because the package’s own metadata should match the package, while Module mode is necessarily heuristic. [web:59]

#### Dependency and ABI chaos

CMake has mechanisms—`find_package`, `FetchContent`, package registries and dependency providers—but historically no universal package resolver with one repository, lockfile and ABI model. It usually cannot determine whether a binary package matches all of:

- compiler and compiler version;
- standard-library implementation;
- C++ ABI switches;
- debug versus release runtime;
- architecture and deployment target;
- static versus shared linkage;
- sanitizer/LTO settings;
- `_GLIBCXX_USE_CXX11_ABI`, MSVC runtime, exception or RTTI mode.

Consequently Conan, vcpkg, system package managers, vendor SDKs and hand-built prefixes are layered on top, each with different notions of identity. CMake can faithfully propagate an imported target that points to the *wrong ABI*.

### 3.2 Conventional-system comparison

| System | Model | Incrementality / parallelism | Strengths | Principal limitations |
|---|---|---|---|---|
| **CMake** | Configure/generate meta-build system | Delegates execution to Ninja, Make, MSBuild, etc. | Dominant C/C++ ecosystem support; many generators; target-based usage requirements | Difficult language, two-stage evaluation, package/ABI ambiguity, not intrinsically hermetic |
| **Meson** | Higher-level declarative configuration, usually generating Ninja | Ninja handles fast parallel execution; explicit targets and dependency objects | Cleaner syntax, cross files, built-in test/install concepts, Wrap subprojects | Smaller ecosystem; still depends on external packages/toolchains; escape hatches reduce reproducibility |
| **Ninja** | Minimal file DAG executor | Mtime-based, parallel by default; `.ninja_log`, depfiles, `.ninja_deps`, `restat`, `dyndep` | Extremely low startup overhead; excellent generated backend | Intentionally almost no policy; an underspecified generated graph remains incorrect. [web:83] |
| **SCons** | Python program directly constructs and executes graph | Content signatures and dependency scanners; can use timestamps | Flexible, checksum-oriented correctness, no separate generator | Python evaluation and generality can be slow; arbitrary Python makes builds harder to analyze. SCons records content and build signatures in `.sconsign`. [web:319][web:321] |
| **GNU Make** | Rules mapping prerequisite files to targets | Topological parallelism with `-j`; primarily compares mtimes | Universal, transparent, excellent for small Unix projects | Missing dependencies silently produce stale outputs; recursive Make obscures the global graph; limited configuration abstraction |
| **Autotools** | Autoconf + Automake + Libtool generate portable shell/configuration and Makefiles | Final execution inherits Make’s model | Exceptional portability across old and unusual Unix systems; feature probing | M4/shell complexity, slow configure probes, generated-file layers and weak modern IDE experience. GNU identifies those three tools as the suite’s main components. [web:348][web:352] |
| **Gradle** | JVM task graph configured by Groovy/Kotlin DSL and plugins | Declared input/output snapshots, incremental tasks, local/remote build cache, parallel workers; configuration cache can skip task-graph construction | Strong JVM/Android ecosystem and dependency management | Plugin/build-script behavior can be dynamic; daemon memory, configuration cost and improperly declared task inputs cause cache misses or stale results. [web:331][web:333] |
| **MSBuild** | XML-evaluated project/property/item graph and targets | Target-level `Inputs`/`Outputs`, timestamp comparison and partial incremental builds; project-level parallelism | Deep Visual Studio, .NET and Windows SDK integration | Evaluation/property ordering is subtle; imports obscure provenance; custom targets are correct only when inputs/outputs are fully declared. [web:334][web:337] |

Meson differs especially from CMake by providing a deliberately smaller language and a Wrap subproject/download mechanism; Ninja remains its primary backend. [web:318][web:324]

---

## 4. Correct incremental builds

### 4.1 The “Build Systems à la Carte” decomposition

Mokhov, Mitchell and Peyton Jones model a build system as the composition of:

1. **Tasks:** how to produce one key/value from other keys.
2. **Scheduler:** in what order tasks run and how dependencies are obtained.
3. **Rebuilder:** whether an existing value is valid or must be recomputed.
4. **Store:** current values and, optionally, historical traces/results.

This separates concepts commonly conflated as “the dependency graph.” The paper was published at ICFP 2018 and later expanded, but remains the clearest taxonomy for evaluating 2024–2026 systems. [web:31][web:33]

### 4.2 Static, dynamic and self-tracking dependencies

| Dependency style | How dependencies become known | Examples | Correctness risk |
|---|---|---|---|
| **Static** | Declared before execution | Make prerequisites, ordinary Ninja edges, most Bazel attributes | Missing declarations silently under-invalidate unless sandboxing rejects access |
| **Dynamic/monadic** | A running task explicitly requests another key based on data already observed | Shake `need`, Pluto, Buck2 dynamic actions | Scheduler must suspend/restart safely and detect cycles |
| **Compiler-reported** | Tool emits a depfile or scanner result | GCC/Clang `.d`, Ninja `depfile`, MSVC `/showIncludes` | First-build ordering still needs care; scanner must reflect actual command configuration |
| **Self-tracking** | System observes actual reads/writes | Rattle via filesystem tracing | Tracer coverage, network/process side effects and read/write hazards matter |

Dynamic dependencies enable “read manifest, then determine inputs.” Self-tracking goes further: the script need not enumerate dependencies because the system observes file accesses.

Shake was designed to support dynamic dependencies while retaining minimal rebuilding; when a task requests an unbuilt dependency, it suspends, builds that dependency and resumes. [web:256]

Rattle traces command file accesses, skips commands whose observed inputs are unchanged and can reuse prior outputs. It detects:

- **read-after/write ordering hazards**;
- **multiple writers**;
- speculative-write/read hazards.

A hazard-free forward script can therefore be parallelized without the author manually writing a complete backward dependency graph. [web:260]

### 4.3 Dirty bits, verifying traces and constructive traces

| Rebuilder | Stored information | On a repeated build | Important property |
|---|---|---|---|
| **Dirty bit / timestamp** | Changed flag or file times | Rebuild if marked dirty or prerequisite is newer | Cheap, but cannot distinguish meaningful from irrelevant changes |
| **Verifying trace** | Hashes/values of inputs used last time | Verify old trace; rebuild if observed dependency values differ | Supports precise invalidation and dynamic dependency sets |
| **Constructive trace** | Verifying trace plus resulting output value | Retrieve the prior output instead of executing | This is build caching |
| **Deep constructive trace** | Result keyed mainly by terminal inputs, abstracting intermediate identities | Reuse across more paths/machines | Powerful but demands strong purity/content identity |

The paper explicitly distinguishes a small verifying trace containing hashes from a constructive trace that stores the actual result; constructive traces naturally support shared/cloud caches. [web:31]

### 4.4 Early cutoff

Suppose a comment is added to `main.c`:

```text
main.c → compile → main.o → link → app
```

The compile task must initially rerun because its input changed. If the resulting `main.o` is byte-for-byte unchanged, an **early-cutoff** system records that the value did not change and skips relinking `app`.

Timestamp systems often fail to cut off here because `main.o` has a new mtime. Shake supports early cutoff by comparing resulting values and stopping propagation when a rebuilt output is unchanged. [web:256][web:261]

This distinction matters:

- **minimal rebuilding from known changed inputs** avoids unrelated work;
- **early cutoff** avoids downstream work after recomputation proves the value unchanged;
- **constructive caching** may avoid the recomputation itself if the same input trace has been seen before.

### 4.5 Why `make clean` culture exists

GNU Make considers a target out of date when it is absent or older than a prerequisite. It does not inherently hash contents or know undeclared inputs. [web:231]

A stale incremental build can therefore arise from:

- omitted header/generated-file dependencies;
- a changed command line not represented as a prerequisite;
- clock skew, timestamp preservation or coarse timestamp resolution;
- switching compiler, architecture or build type in the same output directory;
- deletion of a file that an old dependency set did not record;
- generated files that remain after their generating rule changes;
- recursive builds with incomplete cross-directory ordering;
- tools reading environment or filesystem state unknown to Make.

`make clean && make` deletes the misleading historical outputs, so the build is less dependent on the old state. It is not a correctness proof: a clean build can still read an undeclared host file or race because of a missing ordering edge. The culture exists because cleaning is a cheap operational substitute for proving dependency completeness.

### 4.6 mtime versus content hashing

| Property | mtime comparison | Content hashing |
|---|---|---|
| Cost | Usually one filesystem stat | Must read content unless hashes are cached by metadata/filesystem watcher |
| Detects touched-but-unchanged file | Treats it as changed | Recognizes unchanged bytes |
| Detects changed bytes with preserved/old timestamp | May miss | Detects |
| Distributed identity | Weak—clocks and extracted timestamps differ | Strong—same bytes yield same digest |
| Tracks command/tool/environment | Only if modeled separately | Can include them in a composite action hash |
| Remaining weakness | Missing dependencies | Still missing dependencies: an unobserved input cannot be hashed |

Hashing improves change detection; it does not discover what should have been hashed.

### 4.7 ccache, sccache, Ninja, Shake, Pluto and Rattle

| Tool | Mechanism | Important correctness/performance detail |
|---|---|---|
| **ccache** | Compiler wrapper caching object output | Direct mode hashes source/options and records used headers; a newly appearing header that would change include resolution is a known corner case. Compiler identity defaults to mtime+size for speed. [web:241][web:252] |
| **sccache** | Compiler wrapper with local/cloud backends | For C/C++, hashes the preprocessed source plus compiler, arguments, architecture and relevant environment; supports Rust but does not simply cache every rustc incremental artifact. [web:254][web:255] |
| **Ninja** | Generated file DAG | Uses mtimes, but also records command lines, compiler depfiles and `dyndep`; `restat` suppresses downstream work when an output’s mtime did not change. Its correctness still depends on the generator and tools declaring all relevant edges. [web:83] |
| **Shake** | Haskell library with explicit dynamic dependencies and verifying traces | Supports suspending dynamic dependencies and early cutoff. |
| **Pluto** | Java incremental-build framework | Builders dynamically register file/build-step dependencies; semantic stamps can ignore irrelevant changes, and build-script “metadependencies” trigger rebuilds when logic changes. [web:265] |
| **Rattle** | Forward script plus filesystem tracing | Learns dependencies from actual reads/writes, caches command results and reports hazards that would make reordering or incremental execution unsound. |

---

## 5. Distributed and parallel execution

### 5.1 From distcc and icecc to Goma and Siso

| System | Unit and architecture | Toolchain/caching model | Main limitation or status |
|---|---|---|---|
| **distcc** | Distributes individual C/C++ compilations across hosts | Traditionally preprocess locally and compile remotely, with optional “pump” approaches; often paired with ccache | Compiler-only, limited global scheduling and poor fit for links/generators/tests; network preprocessing traffic can dominate |
| **icecc** | Client wrappers send jobs to daemons under a central scheduler | Can distribute compiler environments, improving worker consistency; can be combined with ccache | Still primarily compiler distribution; scheduler and network are separate operational dependencies. [web:189][web:194] |
| **Goma** | Compiler interception service for Chromium/Android; backend compilers plus shared result cache | Could fan out hundreds of compilations and return cached results | Google deprecated Chromium Goma support around early 2024 in favor of REAPI-native approaches. [web:182][web:187] |
| **Reclient** | `rewrapper` per command talks to local `reproxy`, which uses REAPI | Adds remote cache/execution to Ninja and similar systems | Wrapper/proxy integration rather than a complete build graph engine. [web:191] |
| **Siso** | Go-based, Ninja-compatible executor with native remote execution | Directly consumes Ninja-style graphs and speaks REAPI; avoids a wrapper process per command | Newer and less general than Bazel/Buck2 as a configuration/rule system. Chromium used Siso for all official builders by July 2024 and made it the default for non-Google environments in 2025. [web:151][web:153] |

Siso is significant because it combines Ninja’s generated DAG model with native remote execution. It does not replace GN/CMake-style graph generation; it replaces the execution layer.

### 5.2 REAPI backend landscape

| Backend | Type | Technical character |
|---|---|---|
| **BuildBarn** | Open-source/self-hosted | Modular Go REAPI implementation, typically separated into storage, scheduler, worker/runner and browser components; designed for horizontal composition. |
| **BuildGrid** | Open-source/self-hosted | REAPI execution service, configurable job queue/assigners, CAS and AC; commonly paired with BuildBox workers and local CAS proxies. [web:196][web:210] |
| **BuildBuddy** | Open-core/commercial hosted or self-hosted | REAPI cache/execution plus Build Event UI; horizontally scalable executors, action merging and Sparrow-style scheduling. [web:198][web:199] |
| **EngFlow** | Commercial hosted or self-hosted | Remote cache/execution and analytics; schedulers assign actions while workers keep on-disk CAS and can exchange blobs peer-to-peer. [web:205] |
| **NativeLink** | Open-source plus commercial services | Rust-based CAS, action cache, scheduler and workers; scheduler matches platform/capacity/queue constraints and workers sandbox actions. [web:167][web:175] |

Bazel’s maintained service list identifies BuildBarn, BuildGrid and NativeLink as self-service options and BuildBuddy/EngFlow among commercial offerings. [web:166][web:177]

### 5.3 Scheduling

A production scheduler considers more than idle CPU:

- execution-platform properties: OS, architecture, container image, SDK, accelerator;
- action priority and user/tenant fairness;
- estimated CPU, memory and disk requirements;
- queue depth and worker health;
- input locality—whether a worker already has likely CAS blobs;
- critical-path hints;
- retries and speculative execution;
- duplicate-action merging;
- affinity for incremental linkers or persistent workers.

Bad scheduling can leave thousands of cores nominally available while critical actions wait for the one compatible worker pool. Conversely, indiscriminately sending every 5 ms action over gRPC can make a distributed build slower than local Ninja.

### 5.4 Cache hit rates

There is no meaningful universal “good hit rate.” Measure separately:

- **Action-cache hit rate by action count.**
- **Avoided execution time**, which weights expensive hits more heavily.
- **CAS blob hit rate** and bytes avoided.
- local-disk versus remote hits;
- clean CI, incremental CI and developer builds;
- output-download bytes and input-upload bytes;
- queue, execution and materialization time.

A 95% count hit rate can still be poor if the 5% misses are links and integration tests. A 50% count hit rate may be excellent if it avoids all expensive code generation and compilation.

### 5.5 Merkle and network costs

For each remote action, the client must at least:

1. enumerate declared inputs;
2. hash or retrieve cached hashes;
3. build directory Merkle nodes;
4. query the action cache;
5. check/upload missing CAS blobs on a miss;
6. eventually materialize required outputs.

Large common trees—SDKs, Node modules, source archives or toolchains—make even *cache lookup* expensive because the action digest cannot be computed without the input-root digest. Bazel added Merkle-tree memoization, concurrent upload deduplication, asynchronous uploads and zstd transfer compression; reported compression reductions were approximately 60–70% for tested transfers. [web:212]

Historically, downloading every intermediate could move tens or hundreds of gigabytes and make bandwidth the limiting resource. “Build without the Bytes,” now represented by minimal/toplevel download modes, keeps remote-only intermediates in CAS and downloads only locally required outputs. [web:214][web:219]

A useful latency model is:

\[
T_{\text{remote}} =
T_{\text{hash/tree}} + T_{\text{AC lookup}} +
\begin{cases}
T_{\text{materialize}}, & \text{hit}\\
T_{\text{upload}}+T_{\text{queue}}+T_{\text{execute}}+T_{\text{download}}, & \text{miss}
\end{cases}
\]

Remote execution wins when saved compute and extra parallelism exceed this overhead. The usual remedies are:

- CAS and workers in the same low-latency region;
- worker-local CAS;
- persistent local digest caches/file watchers;
- minimal output downloading;
- compression;
- stable action keys;
- batching small actions or keeping them local;
- preinstalled but cryptographically identified toolchains;
- locality-aware scheduling.

Bazel’s performance guidance explicitly says that high, flat network utilization indicates a network bottleneck and recommends minimal downloads and a local disk cache. [web:215]

---

## 6. Compiler-level incrementality

### 6.1 Separate compilation

Classic C and C++ use translation units:

```text
foo.cpp + transitive headers → foo.o
bar.cpp + transitive headers → bar.o
foo.o + bar.o + libraries → executable
```

Separate compilation makes object files the incremental unit. It works well when `.cpp` implementation changes are isolated, but poorly when public headers have high fan-out.

“Build the world” occurs when a change invalidates a very high node in the dependency graph:

- ubiquitous configuration/generated header;
- template-heavy public API;
- compiler flags or language standard;
- precompiled header;
- SDK/toolchain;
- code generator;
- shared ABI macro;
- package feature set;
- build script or lockfile.

### 6.2 Headers, PCH, unity and modules

| Technique | Benefit | Incremental-build cost |
|---|---|---|
| **Ordinary headers** | Universal, textual and macro-compatible | Every translation unit repeatedly preprocesses/parses transitive text; a high-fan-out header change recompiles all consumers |
| **Precompiled header** | Parse a stable common prefix once | Compiler/flags/macros must match; changing the PCH or one of its constituents invalidates a broad set of translation units |
| **Unity/jumbo build** | Combine many `.cpp` files, amortizing parsing/templates and reducing compiler invocations | A change recompiles the entire unity batch; exposes macro/static-name collisions and can hide missing includes |
| **C++ header unit** | Compiles a header into importable form while retaining more header compatibility | Still exposes header-like macros and configuration sensitivity; build systems must schedule BMI dependencies |
| **Named C++ module** | Parse exported interface once; implementation and macros are better isolated | BMI formats remain compiler/version/flag-sensitive; module scanning, partitions and cross-toolchain packaging add build complexity |

Microsoft’s comparison characterizes PCH as a reusable compiler-memory snapshot, header units as faster and smaller than PCH in typical cases, and named modules as faster and better isolated than textual includes. [web:301]

CMake unity mode literally generates sources that `#include` batches of original sources. It improves throughput but deliberately trades away fine-grained recompilation. [web:315]

Practical C++ guidance:

- use forward declarations and PImpl to keep private implementation out of public headers;
- enforce include-what-you-use rather than accidental transitive includes;
- place stable third-party/system headers in a PCH, not frequently edited project headers;
- keep unity batches moderate and disable unity for problematic sources;
- split stable ABI libraries from fast-changing implementation;
- use modules where compiler/build-system support is mature, but do not treat BMIs as portable binary packages;
- use depfiles and content-addressed compiler caches;
- adopt fast linkers and incremental linking where appropriate.

### 6.3 Rust incremental compilation and queries

Cargo’s normal external unit is the **crate**, but rustc incrementality is finer-grained. The compiler is organized as demand-driven queries such as parsing, name resolution, type checking, trait solving, MIR construction, code generation and metadata production.

After a compilation, rustc persists:

- a query dependency DAG;
- fingerprints of query inputs/results;
- selected cached query results;
- incremental code-generation artifacts.

Its **red-green algorithm** works approximately as follows:

1. Changed source inputs are fingerprinted red; unchanged ones are green.
2. A query can be marked green without execution if all prior dependencies remain green.
3. If a dependency is red, recompute the query.
4. Compare the new result with the old fingerprint.
5. If the result is unchanged, mark it green, cutting off propagation.
6. Otherwise mark it red and invalidate dependents. [web:286][web:287]

Rust also divides a crate into multiple code-generation units so unchanged object code can be reused and LLVM work parallelized. More codegen units improve compile parallelism and incremental reuse but may reduce optimization quality; Cargo uses many more units for incremental development profiles than non-incremental builds. [web:298][web:299]

### 6.4 Why Rust still “builds the world”

Broad Rust invalidation commonly comes from:

- changing a public crate interface or exported generic/inline body;
- changing features, `RUSTFLAGS`, target, profile or compiler version;
- modifying `build.rs` inputs or emitted configuration;
- procedural-macro crates and macro expansion;
- changing dependency resolution or the lockfile;
- cleaning or losing the target directory;
- switching between incompatible feature/profile combinations;
- CI workers without persistent local incremental state.

Cargo builds dependencies as crate units, and rustc’s incremental state is mainly intended for workspace/path code. Cargo documents that incremental compilation is enabled for workspace members and path dependencies, not indiscriminately for all registry dependencies. [web:298]

To avoid broad rebuilds:

- maintain stable crate/API boundaries;
- isolate frequently edited code from heavy generic or macro-heavy crates;
- keep generated code and `build.rs` outputs deterministic with precise rerun inputs;
- persist `target/` for local iteration;
- use sccache for reproducible crate/compiler outputs in CI, but do not expect it to substitute perfectly for rustc’s internal incremental cache;
- avoid changing global flags and feature sets between ordinary edits;
- separate development and release output directories;
- profile compile time before creating excessive micro-crates—the extra metadata, linking and graph overhead can exceed the saved incrementality.

---

## Overall assessment

| Requirement | Strongest general fit | Main qualification |
|---|---|---|
| Very large polyglot monorepo with remote execution | Bazel | Highest adoption and rule-maintenance cost; local analysis remains a bottleneck |
| Meta-style monorepo needing dynamic dependency graphs | Buck2 | Promising unified incremental architecture, but smaller public ecosystem |
| Portable open-source C/C++ library | CMake + Ninja | Ecosystem compatibility wins despite language/package pain |
| Greenfield C/C++ with simpler project language | Meson + Ninja | Cleaner model, less universal package/tool support |
| JVM/Android dependency-rich application | Gradle | Correctness and speed depend on plugin task declarations and configuration-cache compatibility |
| Visual Studio/.NET/Windows SDK project | MSBuild | Excellent integration, subtle property/import and target incrementality model |
| Small Unix project or generated low-level graph | Make or Ninja | Fast and understandable only while dependencies remain complete |
| Compiler-only acceleration without replacing build descriptions | sccache, distcc/icecc, Siso/reclient | Cannot correct a bad high-level graph; network and toolchain consistency still matter |

The decisive question is not “Which tool has the most parallelism?” It is: **Can the system identify every semantic input, assign stable content identity, cut off unchanged results, and move less data than the compute it saves?** Bazel and Buck2 make those properties architectural; conventional systems can achieve many of them, but only through disciplined task declarations, compiler-produced dependency data, caches and carefully controlled toolchains.

Citations:
[1] https://deepwiki.com/bazelbuild/remote-apis/2-remote-execution-api-v2
[2] https://android.googlesource.com/platform/external/bazelbuild-remote-apis/+/e0e5a5351a4571f63506eec04ee49a3dd1e60327/build/bazel/remote/execution/v2/remote_execution.proto
[3] https://www.incredibuild.com/blog/bazel-caching-remote-execution-and-the-build-supply-chain
[4] https://bazel.build/reference/glossary
[5] https://blog.engflow.com/2024/05/13/the-many-caches-of-bazel/
[6] https://docs.rs/bazel-remote-apis/latest/src/bazel_remote_apis/generated/build.bazel.remote.execution.v2.rs.html
[7] https://www.buildbuddy.io/blog/bazels-remote-caching-and-remote-execution-explained/
[8] https://docs.rs/bazel-remote-apis/latest/bazel_remote_apis/build/bazel/remote/execution/v2/index.html
[9] https://queue.acm.org/detail.cfm?id=3287302
[10] https://pkg.go.dev/github.com/bazelbuild/remote-apis/build/bazel/remote/execution/v2
[11] https://bitrise.io/blog/post/bazel-remote-caching-api
[12] https://deepwiki.com/bazelbuild/bazel/6.1-remote-execution-and-caching
[13] https://github.com/bazelbuild/remote-apis/issues/141
[14] https://blog.bazel.build/2026/01/20/bazel-9.html
[15] https://docs.engflow.com/re/what-is-re.html
[16] https://buck2.build/docs/concepts/architecture/
[17] https://github.com/facebook/buck2/blob/main/docs/rule_authors/dynamic_dependencies.md
[18] https://buck2.build/docs/about/why/
[19] https://tianpan.co/forum/t/the-build-system-wars-bazel-buck2-pants-and-whether-your-monorepo-actually-needs-one/615
[20] https://www.reddit.com/r/rust/comments/136qs44/hello_rrust_we_are_meta_engineers_who_created_the/
[21] https://www.buildbuddy.io/blog/buck2-review/
[22] https://blogsystem5.substack.com/p/bazel-next-generation
[23] https://ndmitchell.com/downloads/slides-buck2_for_ocaml_users_and_developers-09_sep_2023.pdf
[24] https://www.hermetiq.com/blog/bazel-vs-buck2-vs-pants
[25] https://ndmitchell.com/downloads/slides-buck2_optimizations_and_bxl-23_oct_2023.pdf
[26] https://tweag.io/blog/2023-07-06-buck2/
[27] https://engineering.fb.com/2023/04/06/open-source/buck2-open-source-large-scale-build-system/
[28] https://buck2.build/docs/api/build/ArtifactValue/
[29] https://buck2.build/docs/bxl/how_tos/how_to_run_actions_based_on_the_content_of_artifact/
[30] https://news.ycombinator.com/item?id=35470371
[31] https://www.microsoft.com/en-us/research/wp-content/uploads/2018/03/build-systems-final.pdf
[32] https://www.scribd.com/document/825568575/Build-Systems-a-la-Carte-2018-build-systems-5ab0f42d0f937
[33] https://www.microsoft.com/en-us/research/publication/build-systems-la-carte/
[34] https://thewagner.net/blog/2019/06/19/build-systems-a-la-carte/
[35] https://www.semanticscholar.org/paper/Build-systems-%C3%A0-la-carte-Mokhov-Mitchell/0a8b8e3318839fde73eab4720b680b1b246dcfa4
[36] https://ii.uni.wroc.pl/media/uploads/2020/10/21/mendyk-jakub.pdf
[37] https://notes.billmill.org/programming/academic_publishing/Build_systems_%C3%A1_la_carte.html
[38] http://neilmitchell.blogspot.com/2018/07/inside-paper-build-systems-la-carte.html
[39] https://www.emqx.com/zh/events/paper-reading-build-system
[40] https://www.cambridge.org/core/journals/journal-of-functional-programming/article/build-systems-a-la-carte-theory-and-practice/097CE52C750E69BD16B78C318754C7A4
[41] https://arxiv.org/html/2007.12737v1
[42] https://hackage.haskell.org/package/build-1.0
[43] https://www.stackage.org/nightly-2026-08-26/package/rattle-0.2
[44] https://news.ycombinator.com/item?id=35589506
[45] https://github.com/tpn/pdfs/blob/master/Build%20Systems%20a%20la%20Carte%20-%202018%20(build-systems-5ab0f42d0f937).pdf
[46] https://cmake.org/cmake/help/latest/manual/cmake-generator-expressions.7.html
[47] https://cmake.org/cmake/help/latest/command/find_package.html
[48] https://cmake.org/cmake/help/v4.1/_sources/manual/cmake-generator-expressions.7.rst.txt
[49] https://cmake.org/cmake/help/v3.5/command/find_package.html
[50] https://cmake.org/cmake/help/v3.6/command/find_package.html
[51] https://cmake.org/cmake/help/v3.17/command/find_package.html
[52] http://cmake.org.cn/manual/cmake-buildsystem.7.html
[53] https://cmake.org/cmake/help/book/mastering-cmake/chapter/Finding%20Packages.html
[54] https://manpages.debian.org/experimental/cmake-data/cmake-properties.7.en.html
[55] https://gitlab.kitware.com/cmake/cmake/-/blob/0b3012c36749e0035da8a8130cbdc13f2d7411f2/Help/manual/cmake-generator-expressions.7.rst
[56] https://cmake.org/cmake/help/v3.0/manual/cmake-generator-expressions.7.html
[57] https://manpages.debian.org/unstable/cmake-data/cmake-properties.7.en.html
[58] https://archive.fosdem.org/2013/schedule/event/moderncmake/attachments/slides/258/export/events/attachments/moderncmake/slides/258/cmake_fosdem_2013.pdf
[59] https://cmake.org/cmake/help/latest/guide/using-dependencies/index.html
[60] https://cmake.com.cn/cmake/help/latest/command/find_package.html
[76] https://mesonbuild.com/Simple-comparison.html
[77] https://docs.gradle.org/current/userguide/gradle_optimizations.html
[78] https://blog.gradle.org/introducing-gradle-build-cache
[79] https://deepwiki.com/gradle/gradle/6.3-build-cache-and-incremental-builds
[80] https://docs.gradle.org/current/userguide/build_cache.html
[81] https://docs.gradle.org/current/userguide/incremental_build.html
[82] https://meson.dev.org.tw/Simple-comparison.html
[83] https://ninja-build.org/manual.html
[84] https://docs.gradle.org/current/userguide/build_cache_use_cases.html
[85] https://docs.gradle.org/current/userguide/part5_gradle_inc_builds.html
[86] https://booster.johnsonlee.io/en/architecture/incremental-build-and-build-cache.html
[87] https://docs.gradle.org/current/userguide/build_cache_debugging.html
[88] https://medium.com/cirruslabs/mastering-gradle-caching-and-incremental-builds-37eb1af7fcde
[89] https://www.unrepo.com/gradle/incremental-builds-and-caching-in-gradle
[90] https://docs.gradle.org/current/userguide/performance.html
[61] https://docs.salome-platform.org/latest/dev/cmake/html/config.html
[62] https://www.studyplan.dev/cmake/cmake-find-package
[63] https://github.com/Kitware/CMake/blob/master/Modules/FindGTest.cmake
[64] https://cmake.org/cmake/help/latest/guide/using-dependencies/index.html
[65] https://cmake.org/cmake/help/v3.5/command/find_package.html
[66] https://cmake.org/cmake/help/v3.17/command/find_package.html
[67] https://cmake.org/cmake/help/v3.0/command/find_package.html
[68] https://cmake.org/cmake/help/v3.6/command/find_package.html
[69] https://cmake.org/cmake/help/latest/command/find_package.html
[70] https://iamsorush.com/posts/cpp-cmake-import-and-find-module/
[71] https://stackoverflow.com/questions/35815850/getting-imported-targets-through-find-package/35840894
[72] https://archive.fosdem.org/2013/schedule/event/moderncmake/attachments/slides/258/export/events/attachments/moderncmake/slides/258/cmake_fosdem_2013.pdf
[73] https://github.com/forexample/package-example
[74] https://stackoverflow.com/questions/20746936/what-use-is-find-package-when-you-need-to-specify-cmake-module-path
[75] https://cmake.org/cmake/help/book/mastering-cmake/chapter/Finding%20Packages.html
[91] https://www.reddit.com/r/cpp/comments/qum4sq/whats_the_hate_for_cmake_all_about_is_cmake/
[92] https://twdev.blog/2021/08/cmake/
[93] https://www.reddit.com/r/cpp/comments/w4idw5/cmake_is_a_hell_of_a_software_period/
[94] https://www.reddit.com/r/cpp/comments/yv3qvm/good_reasons_to_not_use_cmake/
[95] https://www.reddit.com/r/cmake/comments/1aootcp/so_discussion_why_do_people_hate_cmake_and_why/
[96] https://discourse.cmake.org/t/inconsistent-behavior-between-generator-expressions-and-if-else-etc/3718/6
[97] https://www.reddit.com/r/cpp_questions/comments/1oxxuhx/why_do_so_many_people_dislike_cmake_and_why_i_dont/
[98] https://blog.csdn.net/qq_63315166/article/details/145146853
[99] https://www.reddit.com/t/cmake/
[100] https://www.reddit.com/r/cpp_questions/comments/1en2eeh/why_is_cmake_so_hated_and_why_not_use_make_files/
[101] https://llnl-blt.readthedocs.io/en/develop/api/target_properties.html
[102] https://cmake.org/cmake/help/latest/manual/cmake-generator-expressions.7.html
[103] http://arbitrary.name/blog/all/cmake.html
[104] https://stackoverflow.com/beta/discussions/77979031/why-do-people-hate-cmake-and-why-hasnt-a-better-cmake-taken-its-place
[105] https://cmake.org/cmake/help/latest/command/get_target_property.html
[121] https://www.reddit.com/r/devops/comments/1c2g3s4/bazel_is_ruining_my_life/
[122] https://github.com/bazelbuild/bazel/issues/23023
[123] https://www.reddit.com/r/bazel/comments/1jgrilm/bazel_documentation_and_community/
[124] https://slaptijack.com/articles/migrating-bazel-workspace-to-bzlmod.html
[125] https://github.com/bazel-contrib/rules_perl/issues/67
[126] https://pro.aspect.build/guides/bzlmod.pro/
[127] https://slaptijack.com/programming/migrating-bazel-workspace-to-bzlmod.html
[128] https://bazel.googlesource.com/bazel/+/0f41d11b7af3c2856273e4ab5954e043d02d1cd2%5E!/
[129] https://groups.google.com/g/bazel-discuss/c/eScnfnQU21I/m/SBn7J5m7AwAJ
[130] https://github.com/envoyproxy/envoy/issues/46539
[131] https://bazel.build/external/migration
[132] https://blog.engflow.com/2024/06/27/migrating-to-bazel-modules-aka-bzlmod---the-easy-parts/
[133] https://bazel.build/versions/6.5.0/external/overview
[134] https://bazel.build/versions/8.1.1/external/migration
[135] https://www.reddit.com/r/cpp/comments/1h67uvd/bazel_for_c_development/
[106] https://github.com/bazelbuild/bazel/issues/23726
[107] https://github.com/bazelbuild/bazel/issues/14867
[108] https://github.com/bazelbuild/bazel/issues/23846
[109] https://github.com/bazelbuild/bazel/issues/22164
[110] https://github.com/bazelbuild/bazel/issues/7471
[111] https://github.com/bazelbuild/bazel
[112] https://perun.au/insights/bazel-production/
[113] https://bazel.virtuslab.com/book/2~3~1/
[114] https://github.com/bazelbuild/bazel/issues/11782
[115] https://be.bazel.build/designs/2016/06/02/sandboxing.html
[116] https://github.com/bazelbuild/bazel/blob/master/src/main/java/com/google/devtools/build/lib/rules/cpp/CppConfiguration.java
[117] https://mindfulchase.com/explore/troubleshooting-tips/build-bundling/advanced-bazel-troubleshooting-for-persistent-rebuilds-and-cache-invalidation.html
[118] https://infragap.com/hermetic-builds/
[119] https://github.com/bazelbuild/bazel/issues/12761
[120] https://github.com/bazelbuild/bazel/issues/17085
[136] https://medium.com/@sridharmocherla/kotlin-language-server-with-bazel-in-vscode-33578ce0ac23
[137] https://github.com/bazelbuild/intellij/issues
[138] https://www.reddit.com/r/bazel/comments/qikzkh/does_bazel_play_well_with_intellij_ide/
[139] https://rules-python.readthedocs.io/en/1.1.0-rc0/changelog.html
[140] https://rules-python.readthedocs.io/en/1.4.2/changelog.html
[141] https://stackshare.io/stackups/bazel-vs-intellij-idea
[142] https://github.com/bazelbuild/bazel/issues/2906
[143] https://bazel.build/install/ide
[144] https://static.sched.com/hosted_files/bazelcon2024/e4/Reducing%20Bazel's%20memory%20consumption%20(1).pdf
[145] https://bazel.build/advanced/performance/memory
[146] https://github.com/bazelbuild/bazel/pull/18447
[147] https://github.com/redhat-developer/vscode-java/issues/909
[148] https://github.com/bazel-contrib/rules_python/blob/1.2.0-rc1/docs/support.md
[149] https://rules-python.readthedocs.io/en/latest/changelog.html
[150] https://github.com/bazel-contrib/rules_python
[151] https://chromium.googlesource.com/infra/infra/go/src/infra/+/1e819790aacf/build/siso
[152] https://chromium.googlesource.com/build/+/refs/heads/main/siso/README.md
[153] https://groups.google.com/a/chromium.org/g/chromium-dev/c/v-WOvWUtOpg
[154] https://transactional.blog/blog/2025-modern-distcc-via-siso
[155] https://nativelink.com/
[156] https://groups.google.com/g/discuss-webrtc/c/y-_PX8WVjCk
[157] https://chromium.googlesource.com/build/+/HEAD/siso/README.md
[158] https://chromium.googlesource.com/build/+/siso/v1.5.23/siso
[159] https://chromium.googlesource.com/infra/infra/go/src/infra/+/792693d8ab21/build/siso
[160] https://news.ycombinator.com/item?id=44307972
[161] https://dl.acm.org/doi/fullHtml/10.5555/1344149.1344153
[162] https://github.com/distcc/distcc/blob/master/README.md
[163] https://www.slideshare.net/slideshow/distributed-compiler-icecc/61085040
[164] https://groups.google.com/a/chromium.org/g/chromium-dev/c/v-WOvWUtOpg/m/yNEZpmhqCQAJ
[165] https://blog.condi.me/i-3-icecream/
[166] https://bazel.build/community/remote-execution-services
[167] https://docs.nativelink.com/remote-execution
[168] https://bazel.build/versions/8.7.0/community/remote-execution-services
[169] https://docs.nativelink.com/remote-execution/first-remote-action
[170] https://docs.nativelink.com/remote-execution/from-cache-to-execution
[171] https://bazel.build/versions/8.5.1/community/remote-execution-services
[172] https://buildgrid.gitlab.io/buildgrid/user/using_bb_browser.html
[173] https://github.com/buildbarn
[174] https://bazel.build/versions/9.1.0/community/remote-execution-services
[175] https://docs.nativelink.com/explanations/architecture
[176] https://sparkles-docs.pages.dev/research/monorepo-tooling/buildbarn/
[177] https://docs.bazel.build/versions/4.2.3/remote-execution.html
[178] https://sipi.io/development/rbe/
[179] https://tracemachina-nativelink.mintlify.app/concepts/remote-execution
[180] https://docs.bazel.build/versions/2.1.0/remote-execution.html
[211] https://github.com/bazelbuild/bazel/issues/10875
[212] https://www.buildbuddy.io/blog/how-bazel-5-0-makes-your-builds-faster/
[213] https://github.com/bazelbuild/bazel/pull/17398
[214] https://blog.bazel.build/2019/05/07/builds-without-bytes.html
[215] https://bazel.build/advanced/performance/build-performance-breakdown
[216] https://www.incredibuild.com/blog/bazel-caching-remote-execution-and-the-build-supply-chain
[217] https://deepwiki.com/bazelbuild/bazel/6.1-remote-execution-and-caching
[218] https://groups.google.com/g/bazel-discuss/c/I1vAtH0uZ1M
[219] https://blog.bazel.build/2023/10/06/bwob-in-bazel-7.html
[220] https://registry.build/flag/bazel@7.3.2/fetch/?flag=experimental_remote_merkle_tree_cache_size
[221] https://www.buildbuddy.io/blog/debugging-slow-bazel-builds/
[222] https://www.warpbuild.com/guides/bazel-remote-execution-github-actions
[223] https://docs.engflow.com/re/client/bazel-known-issues.html
[224] https://sipi.io/development/rbe-write-pressure/
[225] https://github.com/bazelbuild/bazel/issues/25716
[196] https://buildgrid.build/operation/architecture.html
[197] https://buildbuddy-io-buildbuddy.mintlify.app/architecture-cache-read
[198] https://www.buildbuddy.io/docs/remote-build-execution/
[199] https://www.buildbuddy.io/blog/action-merging/
[200] https://www.buildbuddy.io/blog/bazels-remote-caching-and-remote-execution-explained/
[201] https://buildbuddy-io-buildbuddy.mintlify.app/architecture-overview
[202] https://buildgrid.build/developer/architecture_overview.html
[203] https://blog.engflow.com/2025/11/10/enhanced-invocation-level-insights-for-bazel-builds/
[204] https://buildgrid.build/operation/configuration.html
[205] https://docs.engflow.com/re/index.html
[206] https://visdom-maturity-matrix.virtuslab.com/guides/delivery/bazel-remote-caching-engflow
[207] https://www.buildbuddy.io/docs/config-cache/
[208] https://deepwiki.com/buildbuddy-io/buildbuddy/2.4-remote-execution-system
[209] https://www.buildbuddy.io/remote-cache/
[210] https://buildgrid.build/operation/workers.html
[181] https://blog.engflow.com/2023/09/11/goma-is-gone--put-everything-into-reclient/
[182] https://groups.google.com/a/chromium.org/g/chromium-dev/c/rajt7THxIng
[183] https://github.com/EngFlow/goma_server
[184] https://blog.engflow.com/category/goma/
[185] https://blog.engflow.com/archive/2023/page/2/
[186] https://blog.engflow.com/category/reclient/
[187] https://chromium.googlesource.com/infra/goma/client/+/6bae3f8023bfb7b861b2c0b1e12aa0be55f0f829/README.md
[188] https://chromium.googlesource.com/infra/goma/client/+show/5be5287a0a220b44f9b737bdf94f9ebf4b897d29/README.md
[189] https://manpages.ubuntu.com/manpages/xenial/man7/icecream.7.html
[190] https://kubala.github.io/docs/intro
[191] https://blog.engflow.com/2024/02/20/how-to-build-chromium-with-reclient/
[192] https://www.reddit.com/r/cpp/comments/be5j4w/gomagoogles_internal_distributed_compiler_service/
[193] https://manpages.debian.org/testing/icecc/icecc-scheduler.1.en.html
[194] https://manpages.debian.org/testing/icecc/iceccd.1.en.html
[195] https://manpages.ubuntu.com/manpages/jammy/man7/icecream.7.html
[226] https://www.oreilly.com/library/view/the-gnu-make/9781457189883/ch03.html
[227] https://softwareengineering.stackexchange.com/questions/319359/why-incremental-builds-in-make-dont-use-hashing-algorithms
[228] https://arxiv.org/html/2404.13295v1
[229] https://www.mindfulchase.com/explore/troubleshooting-tips/build-bundling/troubleshooting-make-fixing-broken-dependencies-and-unreliable-builds.html
[230] https://lists.gnu.org/archive/html/help-make/2013-07/msg00022.html
[231] https://www.gnu.org/software/make/manual/make.html
[232] https://superuser.com/questions/1798148/why-don-t-make-check-the-timestamp-when-the-dependency-file-is-not-generated
[233] https://man7.org/linux/man-pages/man1/make.1p.html
[234] https://www.evanjones.ca/makefile-dependencies.html
[235] https://stackoverflow.com/questions/18412691
[236] https://lists.gnu.org/archive/html/help-make/2011-08/msg00007.html
[237] https://lists.gnu.org/archive/html/help-make/2005-08/msg00071.html
[238] https://moldstud.com/articles/p-master-incremental-builds-ultimate-makefile-optimization-techniques-for-developers
[239] https://ftp.gnu.org/old-gnu/Manuals/make-3.79.1/html_chapter/make_9.html
[240] https://dmitryfrank.com/blog/2016/1211_i_am_tired_of_makefiles
[241] https://ccache.dev/manual/4.13.1.html
[242] https://ccache.dev/manual/3.7.9.html
[243] https://ccache.dev/manual/4.12.1.html
[244] https://ccache.dev/manual/3.7.7.html
[245] https://ccache.dev/manual/4.8.html
[246] https://ccache.dev/manual/4.8.2.html
[247] https://ccache.dev/manual/3.5.html
[248] https://ccache.dev/manual/4.2.html
[249] https://ccache.dev/manual/4.7.5.html
[250] https://ccache.dev/manual/4.9.1.html
[251] https://ccache.dev/manual/latest.html
[252] https://ccache.dev/manual/4.2.1.html
[253] https://github.com/mozilla/sccache/issues/257
[254] https://github.com/mozilla/sccache/blob/main/docs/Caching.md
[255] https://github.com/mozilla/sccache/blob/main/README.md
[256] https://www.microsoft.com/en-us/research/wp-content/uploads/2018/03/build-systems-final.pdf
[257] https://www.microsoft.com/en-us/research/wp-content/uploads/2020/04/build-systems-jfp.pdf
[258] https://ndmitchell.com/downloads/paper-forward_build_systems_formally-17_jan_2022.pdf
[259] https://www.pl.informatik.uni-mainz.de/files/2019/04/pluto-incremental-build.pdf
[260] https://arxiv.org/html/2007.12737v1
[261] http://neilmitchell.blogspot.com/2021/09/reflecting-on-shake-build-system.html
[262] http://simonmar.github.io/bib/papers/shake.pdf
[263] https://signalsandthreads.com/build-systems/
[264] https://dl.acm.org/doi/10.1145/2398856.2364538
[265] https://pluto-build.github.io/
[266] https://hackage.haskell.org/package/shake-0.13.4/docs/Development-Shake.html
[267] https://www.mathematik.uni-marburg.de/~seba/publications/pie-scalable-incremental-build.pdf
[268] https://ndmitchell.com/downloads/paper-build_scripts_with_perfect_dependencies-18_nov_2020.pdf
[269] https://shakebuild.com/manual
[270] https://hackage.haskell.org/package/shake-0.6
[286] https://rustc-dev-guide.rust-lang.org/queries/incremental-compilation.html
[287] https://rustc-dev-guide.rust-lang.org/queries/incremental-compilation-in-detail.html
[288] https://github.com/nikomatsakis/rustc-on-demand-incremental-design-doc/blob/master/0000-rustc-on-demand-and-incremental.md
[289] https://docs.rs/incremental-query/latest/incremental_query/
[290] https://doc.rust-lang.org/beta/nightly-rustc/rustc_incremental/persist/index.html
[291] https://doc.rust-lang.org/rustc/codegen-options/index.html
[292] https://doc.rust-lang.org/stable/nightly-rustc/rustc_incremental/index.html
[293] https://doc.rust-lang.org/beta/nightly-rustc/rustc_incremental/index.html
[294] https://doc.rust-lang.org/beta/nightly-rustc/rustc_middle/query/index.html
[295] https://github.com/rust-lang/rust/issues/47660
[296] https://blog.rust-lang.org/2016/09/08/incremental/
[297] https://dev.to/simmypeet/building-a-query-based-incremental-compilation-engine-in-rust-nj6
[298] https://doc.rust-lang.org/cargo/reference/profiles.html?highlight=incremental
[299] https://rust-lang.github.io/rfcs/1298-incremental-compilation.html
[300] https://rustc-dev-guide.rust-lang.org/solve/caching.html
[301] https://learn.microsoft.com/en-us/cpp/build/compare-inclusion-methods?view=msvc-170
[302] https://devtalk.blender.org/t/speed-up-c-compilation/30508
[303] https://blog.xizhibei.me/en/2020/07/29/cmake-10-refine-compile-speed-and-program-performance/
[304] https://github.com/j-jorge/unity-build
[305] https://www.reddit.com/r/cpp/comments/gw163l/interesting_cpp_unity_build_benchmarks/
[306] https://onqtam.github.io/programming/2019-12-20-pch-unity-cmake-3-16/
[307] https://github.com/xizhibei/blog/issues/145
[308] https://vladiant.github.io/blog/2023/10/13/cpp-compilation-improve
[309] https://discourse.llvm.org/t/rfc-use-pre-compiled-headers-to-speed-up-llvm-build-by-1-5-2x/89345
[310] https://stackoverflow.com/questions/52423521/speeding-up-c-builds-with-unity-builds-and-reduced-header-dependencies
[311] https://slides.com/onqtam/faster_builds
[312] https://stackoverflow.com/questions/373142/what-techniques-can-be-used-to-speed-up-c-compilation-times
[313] https://docs.unity3d.com/cn/2022.3/Manual/incremental-build-pipeline.html
[314] https://news.ycombinator.com/item?id=47604768
[315] https://cmake.org/cmake/help/latest/prop_tgt/UNITY_BUILD.html
[271] https://stackoverflow.com/questions/10637762/make-doesnt-rebuild-headers-when-changed
[272] https://preview.bazel.build/tutorials/cpp-use-cases
[273] https://internals.rust-lang.org/t/pre-rfc-sandboxed-deterministic-reproducible-efficient-wasm-compilation-of-proc-macros/19359
[274] https://nukethebees.com/cpp-include-order/
[275] https://web.mit.edu/rust-lang_v1.25/arch/amd64_ubuntu1404/share/doc/rust/html/unstable-book/language-features/proc-macro.html
[276] https://news.ycombinator.com/item?id=23549741
[277] https://www.reddit.com/r/C_Programming/comments/1ee5obb/is_dependency_transitive/
[278] https://unliftedq.github.io/modern-cpp-in-practice/en/11-modules-libraries-packaging-and-abi-reality.html
[279] https://www.reddit.com/r/rust/comments/139iff0/how_are_macros_dealt_with_for_incremental/
[280] https://crane.dev/faq/rebuilds-with-proc-macros.html
[281] https://stackoverflow.com/questions/76923661/generating-proc-macro-expanded-version-of-code-at-build-time
[282] https://github.com/MicrosoftDocs/cpp-docs/blob/main/docs/ide/include-cleanup-overview.md
[283] https://scalablecpp.wordpress.com/2022/03/20/header-dependencies-ii-analysis/
[284] https://matklad.github.io/2021/09/04/fast-rust-builds.html
[285] https://discourse.llvm.org/t/rfc-remove-unused-transitive-includes-from-the-libc-headers/90157
[316] https://scons.github.io/docs/scons-man.html
[317] https://mesonbuild.com/Wrap-best-practices-and-tips.html
[318] https://github.com/mesonbuild/meson/blob/master/docs/markdown/Wrap-dependency-system-manual.md
[319] https://www.scons.org/doc/0.92/HTML/scons-python10/x106.html
[320] https://simplifycpp.org/books/cpp/Mastering_Meson_A_Practical_Guide_to_Modern_Build_Systems.pdf
[321] https://www.scons.org/doc/HTML/scons-user.html
[322] https://scons.org/doc/production/HTML/scons-user/ch06.html
[323] https://github.com/mesonbuild/meson/blob/master/mesonbuild/backend/ninjabackend.py
[324] https://mesonbuild.com/FAQ.html
[325] https://legacy.python.org/workshops/2002-02/papers/16/index.htm
[326] https://docs.oracle.com/cd/E75432_01/html/E71066/scons-1.html
[327] https://en.wikipedia.org/wiki/SCons
[328] https://en.wikipedia.org/wiki/Meson_(software)
[329] https://github.com/mesonbuild/meson/discussions/10547
[330] https://mesonbuild.com/Reference-manual_functions_dependency.html
[346] https://docs.raulvilchez.org/04_System_Administration/test_environment_setup/ltp_setup/documents-build.html
[347] https://www.gnu.org/software/automake/manual/1.10.1/automake.pdf
[348] https://www.gnu.org/software/autoconf/manual/autoconf-2.68/html_node/The-GNU-Build-System.html
[349] http://gnu.ist.utl.pt/software/autoconf/manual/autoconf-2.57/html_chapter/autoconf_2.html
[350] https://www.am-utils.org/docs/autotools/autotools.pdf
[351] https://github.com/Macaulay2/M2/wiki/Internals:-Autotools-Build-System
[352] https://www.gnu.org/software/automake/faq/autotools-faq.html
[353] https://releases.llvm.org/3.7.1/docs/BuildingLLVMWithAutotools.html
[354] https://github.com/PDC-support/introduction-to-autotools/blob/main/README.md
[355] https://docs.redhat.com/en/documentation/Red_Hat_Enterprise_Linux/6/html/developer_guide/cmd-autotools-config
[356] https://docs.redhat.com/zh-cn/documentation/Red_Hat_Enterprise_linux/6/html/developer_guide/cmd-autotools-config
[357] https://www.lrde.epita.fr/~adl/dl/autotools.pdf
[358] https://www.gnu.org/software/automake/manual/1.10.2/automake.pdf
[359] https://dokumen.pub/autotools-a-practitioners-guide-to-gnu-autoconf-automake-and-libtool-2nbsped-1593279728-9781593279721.html
[360] https://www.trex-coe.eu/sites/default/files/TREX%20Build-systems%20Hackathon%20-%20Nov%202021/autotools_Christian%20Feld.pdf
[331] https://docs.gradle.org/current/userguide/configuration_cache.html
[332] https://docs.gradle.org/current/userguide/configuration_cache_requirements.html
[333] https://docs.gradle.org/current/userguide/performance.html
[334] https://learn.microsoft.com/en-us/visualstudio/msbuild/how-to-build-incrementally?view=visualstudio
[335] https://learn.microsoft.com/en-us/visualstudio/msbuild/how-to-build-incrementally?view=vs-2022
[336] https://blog.gradle.org/road-to-configuration-cache
[337] https://learn.microsoft.com/en-us/visualstudio/msbuild/incremental-builds?view=vs-2019
[338] https://docs.gradle.org/9.1.0/userguide/configuration_cache.html
[339] https://docs.gradle.org/current/userguide/build_cache.html
[340] https://docs.gradle.org/current/userguide/gradle_optimizations.html
[341] https://jasonatwood.io/archives/2127
[342] https://learn.microsoft.com/en-us/visualstudio/msbuild/incremental-builds?view=vs-2022
[343] https://medium.com/androiddevelopers/configuration-caching-deep-dive-bcb304698070
[344] https://github.com/MicrosoftDocs/visualstudio-docs/blob/main/docs/msbuild/msbuild-targets.md
[345] https://docs.gradle.org/current/userguide/build_lifecycle.html
[376] https://bazel.build/reference/skyframe
[377] https://bazel.build/query/aquery
[378] https://bazel.build/versions/6.5.0/reference/skyframe
[379] https://bazel.build/versions/7.7.1/reference/skyframe
[380] https://bazel.build/versions/9.1.0/reference/skyframe
[381] https://bazel.build/query/cquery
[382] https://bazel.build/reference/glossary
[383] https://docs.bazel.build/versions/3.2.0/skylark/lib/globals.html
[384] https://docs.bazel.build/versions/3.1.0/skylark/lib/globals.html
[385] https://docs.bazel.build/versions/2.1.0/skylark/lib/globals.html
[386] https://bazel.build/reference/command-line-reference
[387] https://bazel.build/versions/9.0.0/reference/glossary?hl=zh-tw
[388] https://docs.bazel.build/versions/main/skylark/rules.html
[389] https://docs.bazel.build/versions/4.2.4/skylark/lib/globals.html
[390] https://bazel.build/advanced/performance/build-performance-breakdown?hl=zh-tw
[391] https://bazel.build/extending/rules
[392] https://bazel.build/rules/lib/providers
[393] https://bazel.build/versions/7.1.0/rules/lib/builtins/ctx
[394] https://bazel.build/rules/lib/builtins/actions
[395] https://bazel.build/rules/lib/toplevel/testing
[396] https://docs.bazel.build/versions/0.24.0/skylark/language.html
[397] https://adincebic.com/2026/02/01/executing-actions-from-bazel-aspects.html
[398] https://bazel.build/versions/6.4.0/rules/lib/testing
[399] https://docs.bazel.build/versions/0.21.0/skylark/language.html
[400] https://docs.bazel.build/versions/0.18.1/skylark/language.html
[401] https://docs.bazel.build/versions/1.1.0/skylark/lib/Action.html
[402] https://docs.bazel.build/versions/0.23.0/skylark/language.html
[403] https://bazel.build/rules/rules-tutorial
[404] https://github.com/bazelbuild/starlark
[405] https://github.com/bazelbuild/starlark/blob/32993fa0d1f1e4f3af167d249be95885ba5014ad/spec.md
[361] https://android.googlesource.com/platform/external/bazelbuild-remote-apis/+/e0e5a5351a4571f63506eec04ee49a3dd1e60327/build/bazel/remote/execution/v2/remote_execution.proto
[362] https://deepwiki.com/bazelbuild/remote-apis/2-remote-execution-api-v2
[363] https://pkg.go.dev/github.com/bazelbuild/remote-apis/build/bazel/remote/execution/v2
[364] https://deepwiki.com/bazelbuild/remote-apis/2.1-core-concepts-and-data-model
[365] https://pkg.go.dev/github.com/twitter/scoot/bazel/remoteexecution
[366] https://pkg.go.dev/go.chromium.org/build/remote-apis/build/bazel/remote/execution/v2
[367] https://www.incredibuild.com/blog/bazel-caching-remote-execution-and-the-build-supply-chain
[368] https://docs.rs/bazel-remote-apis/latest/bazel_remote_apis/build/bazel/remote/execution/v2/index.html
[369] https://docs.rs/bazel-remote-apis/latest/src/bazel_remote_apis/generated/build.bazel.remote.execution.v2.rs.html
[370] https://pkg.go.dev/github.com/buildbuddy-io/buildbuddy/server/remote_execution/execution_server
[371] https://nativelink.com/docs/config/production-config
[372] https://blog.bazel.build/2018/09/14/bazel-0.17.html
[373] https://pkg.go.dev/github.com/buchgr/bazel-remote/v2
[374] https://docs.nativelink.com/reference/protocol-api
[375] https://www.warpbuild.com/glossary/remote-execution
[406] https://buck2.build/docs/insights_and_knowledge/modern_dice/
[407] https://buck2.build/assets/Modern_DICE.pdf
[408] https://github.com/facebook/buck2/discussions/807
[409] https://buck2.build/docs/rfcs/implemented/bxl/
[410] https://buck2.build/docs/rfcs/bxl/
[411] https://github.com/facebook/buck2/blob/main/ci.bzl
[412] http://github.com/facebook/buck2
[413] https://buck2.build/docs/bxl/faq/
[414] https://github.com/facebook/buck2/pulls
[415] https://github.com/facebook/buck2/activity
[416] https://buck2.build/docs/rule_authors/incremental_actions/
[417] https://buck2.build/docs/why/
[418] https://github.com/facebook/buck2/discussions
[419] https://github.com/facebook/buck2/blob/main/README.md
[420] https://github.com/facebook/buck2/blob/main/CONTRIBUTING.md
