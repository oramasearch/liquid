<!-- Raw research source. Generated via Perplexity `perplexity_research` (web-grounded,
     multi-source) on 2026-09-01 for the liquid project. Lightly reformatted only.
     Treat as evidence, not as liquid design decisions - see README.md in this
     directory for the curated analysis and the implications drawn for liquid. -->

# Deep research: incremental/query-based compilation and content-addressed code, 2024–2026

**Bottom line:** the strongest modern architecture treats compilation as a graph of memoized, deterministic queries rather than a fixed pipeline. Fine-grained dependency recording, stable identities, result fingerprints, early cutoff, and persistent caches let an edit invalidate only the semantic facts and artifacts it can actually affect. Content-addressed systems such as Unison push the same idea further: immutable definitions become their own stable cache keys. Neither approach eliminates the hardest runtime problem, however—**migrating live state, stacks, resources, and protocols when code changes**.

## 1. Query-based and demand-driven compiler architecture

### 1.1 General model

A conventional pipeline eagerly performs:

```text
parse all → resolve all → type-check all → lower all → optimize all → emit all
```

A query compiler instead exposes facts such as:

```text
parse(file)
items(module)
type_of(definition)
trait_solution(goal)
mir(definition)
codegen_unit(crate, cgu_id)
```

Each invocation is identified by `(query kind, key)`. Its provider computes a value on a cache miss and may invoke other queries. Those calls dynamically record edges:

```text
type_check(foo)
 ├── type_of(foo)
 │    └── HIR(foo)
 └── type_of(bar)
      └── HIR(bar)
```

This gives three independent benefits:

1. **Demand-driven execution:** compute only facts required for the requested diagnostic, completion, object file, or other output.
2. **In-process memoization:** repeated requests for the same key return the cached value.
3. **Cross-session incrementality:** persist enough of the query graph, stable identities, fingerprints, and selected values to prove that old results remain valid.

The central correctness invariant is approximately:

> If a query result is reused, every input that could have influenced it must have been tracked, and either all those inputs are unchanged or recomputation proved that the result itself is unchanged.

An omitted dependency is therefore more dangerous than excess invalidation: excess edges waste time, while a missing edge can produce stale diagnostics or a miscompiled binary.

### 1.2 Implementations and mechanisms

| System | Query/incremental mechanism | Granularity and persistence | Early cutoff or invalidation behavior | Important limitations |
|---|---|---|---|---|
| **rustc query system** | Most compiler facts are registered queries. A provider computes a missing `(query, key)` value; subsequent calls are memoized. Query-to-query calls are dynamically recorded, and cycles are detected from active query jobs. [web:391][web:393] | Nodes represent particular query invocations; keys commonly identify crates, definitions, HIR owners, MIR bodies, or code-generation units. Selected results plus the prior dependency graph are serialized between compiler processes. | The red-green algorithm recursively tries to prove dependencies green. If a dependency may have changed, rustc runs its provider and compares the new fingerprint with the old one; an equal result becomes green and prevents false-positive invalidation from propagating further. [web:31] | Queries must act like pure functions over tracked inputs. Reads of files, environment variables, compiler options, global state, or side effects must be represented explicitly or handled through mechanisms such as `eval_always`. Fingerprinting and cache I/O also impose clean-build overhead. |
| **rustc fingerprints and stable identity** | A `DepNode` consists of a dependency kind and a 128-bit fingerprint. Query results are hashed with stable forms of session-local identities—for example, a `DefPathHash` instead of a numerically unstable `DefId`. [web:401] | The old graph is indexed using fingerprints of query keys; result fingerprints are loaded cheaply without deserializing every old result. Green nodes and edges are copied into the current graph. | A recomputed result with the same stable fingerprint is green even if an input changed. This is rustc’s equivalent of value-sensitive early cutoff. | Hash collisions are theoretically unsound, although negligible with 128-bit fingerprints. Stable hashing is expensive and every semantic input must have a stable representation. `no_hash` queries cannot perform result-based cutoff and conservatively make dependents rerun. |
| **rustc CGU partitioning** | LLVM is not itself a rustc query engine, so rustc creates a dependency node for each codegen unit and records the queries used to construct it. A green CGU reuses its object/bitcode artifact. | The partitioner places monomorphized items into CGUs, historically separating stable non-generic code from more volatile monomorphized/specialized code per source module. [web:34][web:40] | If the CGU dependency node is green, backend compilation is skipped; otherwise the entire CGU is regenerated. | Backend reuse is coarser than front-end queries. More/smaller CGUs improve reuse and parallelism but reduce cross-unit optimization and increase metadata, linking, and LLVM overhead. |
| **Salsa / rust-analyzer** | Salsa models mutable external facts as inputs and derived facts as tracked, memoized Rust functions. Dependencies are recorded as tracked functions execute. rust-analyzer uses this as an on-demand analysis database. [web:8][web:14] | Dependencies can be per input field in current Salsa. Queries are normally long-lived in the language-server process rather than primarily an on-disk compiler cache. | Updating an input increments a revision in `O(1)`. On demand, Salsa validates dependencies. If a recomputed query has the same output, propagation stops—**early cutoff**. Whitespace can alter source text while leaving a position-independent syntax result unchanged. [web:1] | Equality is architectural: values containing syntax nodes, source offsets, unstable IDs, or oversized aggregates can make almost every revision appear different. Retained memo tables consume memory, cancellation and concurrency complicate execution, and an accidentally untracked read is incorrect. |
| **Salsa durability** | Inputs receive durability levels such as low/volatile for workspace files and high/durable for standard-library data. A derived query gets the minimum durability of its transitive ground inputs. | Salsa keeps a revision component for each durability class. A low-durability edit does not change the high-durability revision. | A query depending only on high-durability inputs can be validated without traversing its whole dependency subgraph after a local edit. This eliminated substantial no-op traversal in rust-analyzer. [web:1][web:14] | Durability is not a correctness exemption. If apparently durable library analysis can read a volatile path, that dependency lowers its effective durability. Misclassification must not suppress a real edge. |
| **Salsa “firewall queries”** | This is primarily a design idiom rather than a distinct modern Salsa primitive: insert a query whose output deliberately removes volatile detail. Examples include source text → syntax without positions, or full semantic data → compact public interface. | The firewall’s key/value boundary becomes the coarsening point. Downstream queries depend on the compact result rather than on raw source or a whole compilation object. | Equality at the firewall converts many upstream edits into an unchanged derived result, terminating invalidation. | A firewall that omits semantically relevant data is unsound; one that retains spans, object identity, or broad collections fails to stop invalidation. It can also add cache and graph overhead when the protected computation is cheap. |
| **Roslyn compiler/workspace** | Syntax trees are immutable persistent red-green trees: internal green nodes contain kind, width, and children but no parents or absolute positions; red wrappers add parent and positional context. Incremental parsing replaces the affected region and spine while structurally sharing unchanged green subtrees. [web:20] | The IDE maintains immutable `Solution`, `Project`, `Compilation`, syntax-tree, and semantic snapshots. Files not edited retain trees; changed text is incrementally lexed and parsed. | Structural sharing and immutable snapshots permit old and new analyses to coexist. Cached semantic information can be reused when the corresponding syntax and references are unchanged. | This is less uniformly exposed as a universal query graph than rustc/Salsa. Whole `Compilation` values are broad invalidation boundaries, and command-line builds do not necessarily preserve all IDE memoization across processes. |
| **Roslyn incremental generators** | `IIncrementalGenerator` builds a dataflow graph from providers such as syntax, additional files, analyzer options, and compilation. `Select`, `Where`, `Combine`, and output registrations form pure transformations. Generators are additive: they emit source but cannot rewrite user source. [web:17] | Values at each pipeline node are cached. Narrow, value-equatable records permit item-level reuse; retaining `ISymbol`, `SyntaxNode`, `Compilation`, `Location`, or reference-equal collections can root old snapshots and defeat caching. | When a stage produces an equal value, the generator driver reuses downstream cached output. The recommended approach is to extract a small equatable model early and generate from that. [web:337] | Combining every item with `CompilationProvider`, or using `Collect` too early, can turn one edit into a change to every downstream pair. Generators are unordered and cannot generally consume another generator’s standard output. Cross-command-line persistence is more limited than IDE-session caching. |
| **Swift** | The frontend/request evaluator records names provided by each source file and names required during type checking. It emits an opaque binary `.swiftdeps` dependency file per source file. [web:61][web:62] | Dependencies are semantically name-sensitive, but the driver’s schedulable recompilation unit remains a source file. The driver first recompiles changed files, then uses changed provided names to discover dependent files. | If implementation text changes without changing a provided declaration relevant to another file, fewer dependents need rebuilding. Batch mode combines compatible primary-file jobs to reduce repeated frontend startup and work. [web:69] | External module/header dependencies have historically been coarser; an external change can invalidate a large portion of a module. `.swiftdeps` is compiler-private and unstable across versions. Timestamp-based freshness also needs conservative handling; Swift has explored recording file hashes to distinguish touched-but-identical inputs. [web:64] |
| **Kotlin/JVM with Gradle** | Gradle first decides whether the Kotlin task is up to date from declared inputs/outputs. Kotlin’s own incremental compiler maintains source/class relationships, symbol lookups, and classpath snapshots to calculate dirty files. | Fine-grained classpath snapshots contain member-level API information; coarse snapshots for stable cached JARs contain class ABI hashes. A member change can therefore recompile only source files that used that member, while a coarse class ABI change invalidates all users of the class. [web:76] | Private implementation changes that preserve ABI may permit compilation avoidance. Dirty-source compilation can expose further changed symbols/classes and iterate until the affected set reaches a fixed point. | Inline bodies, constants, generated code, Java/Kotlin interoperation, compiler plugins, kapt/KSP declarations, class hierarchy changes, and missing dependency declarations can widen invalidation or force a rebuild. Incremental mode is distinct from Gradle’s build cache, which reuses complete task outputs. |
| **Gradle** | Tasks declare normalized inputs, implementation identity, and outputs. An unchanged snapshot skips the task; a cache hit can restore its complete outputs. Java/Kotlin compilation avoidance additionally compares dependency ABI rather than all implementation bytes. [web:78][web:82] | Usually task-, project-, file-, or class-ABI-level—not an arbitrary compiler semantic query graph. | Unchanged task inputs give an up-to-date hit; ABI-compatible dependency edits can leave downstream compilation tasks untouched. | Correctness depends on complete input/output declarations. Undeclared environment, network, filesystem, or tool inputs cause stale outputs; overly broad classpaths or task inputs cause excessive misses. |
| **TypeScript project references** | A composite `tsconfig` references other projects. `tsc -b` discovers the project DAG, topologically builds out-of-date projects, and uses emitted `.d.ts` interfaces rather than treating the monorepo as one compilation. [web:46][web:55] | `.tsbuildinfo` serializes project-graph and per-file builder state for later invocations. Composite projects always produce it; it is a disposable performance cache. [web:47] | A changed file can be rechecked while an unchanged declaration signature prevents downstream projects from rebuilding. Declaration maps restore cross-project navigation to source. | The cache format is compiler-version-specific, not a portable artifact. Project boundaries are coarse; poorly chosen references, cyclic package organization, broad ambient declarations, or frequently changing generated `.d.ts` files cause cascades. |
| **Adapton** | Adapton combines memoized lazy thunks with a **demanded computation graph** (DCG). It separates mutation from observation: mutations dirty relevant graph portions, but only demanded results are brought up to date. [web:106][web:109] | The graph represents the dynamic execution trace rather than only a static module graph. “Articulation points” let programmers expose useful incremental boundaries. | Change propagation re-executes only dirty demanded computations, and unchanged results can be reused compositionally. | Graph maintenance, dependency edges, memo tables, and change propagation can cost more than recomputation for small or unstable tasks. Dynamic control flow can destroy reuse when computations lack stable identity. |
| **Nominal Adapton** | First-class names identify computations and data across runs, giving programmers control over matching old and new graph nodes. | Names preserve identity even when allocation order or surrounding list/tree structure changes. | Stable matching improves reuse for maps, folds, trees, and incremental probabilistic structures. | Incorrect naming or name collisions can break intended reuse; the programming model exposes more identity management to the programmer. Its formal target is **from-scratch consistency**: results must equal complete reevaluation. [web:107] |
| **Self-adjusting computation — Umut Acar and collaborators** | An initial run records a dynamic dependence graph or execution trace containing reads, writes, and code to rerun. Mutable inputs trigger change propagation; memoization can match and reuse computations in the changed execution. | Finer than files or functions: dependencies can correspond to individual modifiable cells, reads, writes, and control-flow intervals. | Only the affected trace is repaired; the trace’s structure itself may change as conditionals or recursion follow a different path. [web:108][web:111] | Efficient asymptotics require carefully chosen mutable data structures and stable computation matching. Space overhead, trace garbage collection, side effects, concurrency, and worst-case global invalidation make transparent incrementalization difficult. |

### 1.3 Why rustc incremental compilation is sometimes disabled

| Reason | Technical explanation |
|---|---|
| **Correctness risk** | The cache is sound only if every dependency is recorded and every fingerprint is stable and complete. An omitted edge, incorrectly stable hash, session-local ID, compiler-option omission, or cache-deserialization bug can reuse stale code and miscompile. Rust 1.52.1 temporarily disabled incremental compilation by default after verification exposed widespread incremental-build failures and potential miscompilations. [web:318] |
| **Release-code quality** | Incremental reuse favors multiple stable CGUs. More CGUs restrict cross-unit optimization and may produce slower or larger code than a less-partitioned, LTO-oriented release build. Cargo therefore defaults the release profile to `incremental = false`, while development profiles favor turnaround time. [web:316][web:318] |
| **Cold-build overhead** | Stable hashing, dependency recording, serialization, extra object/bitcode artifacts, disk reads, validation, and cache promotion can make a clean or one-off build slower. Fingerprint computation is specifically a major incremental overhead. |
| **Cache size and CI economics** | Incremental directories can be large, machine/compiler-specific, and expensive to upload or restore. In ephemeral CI, the cache may never be warm enough to recover its transfer and validation cost. A remote artifact cache at crate/task granularity can be more effective. |
| **Hard-to-track computations** | Queries that read global state or always perform side effects use `eval_always`; unhashable or expensive results can use `no_hash`. Both reduce reuse. LLVM modules are opaque and must be handled as whole CGU artifacts rather than ordinary query values. |
| **Recovery and reproducibility** | Deleting the incremental cache or setting `CARGO_INCREMENTAL=0` is a practical way to distinguish a cache defect from a source/compiler defect. Production builds often prefer the smaller trusted-state surface of a clean build. |

---

## 2. Unison: content-addressed code

### 2.1 Representation and workflow

Unison applies content addressing at the semantic-definition level rather than merely hashing source files:

```text
source definition
   ↓ parse, resolve names, type-check, normalize binders
internal AST with dependency references replaced by hashes
   ↓ SHA3-512
definition hash
   ↓
immutable codebase object
```

Names are metadata pointing to hashes; they are not the identity by which compiled definitions refer to one another. Unison uses 512-bit SHA3 hashes of internal term/type structure with names excluded. Mutually recursive definitions are canonicalized and hashed as one cycle, with members addressed by cycle hash and index. [web:100][web:104]

| Claimed property | Concrete mechanism | Precise consequence | Qualification or limitation |
|---|---|---|---|
| **Every definition is addressed by its AST hash, with dependencies hashed in** | Bound variable names are replaced by positional references, and free references to other definitions become their hashes before hashing. [web:122] | A definition’s identity commits to its exact transitive code dependency graph: changing a dependency creates a new dependency hash and therefore new hashes for dependent definitions when they are updated. | This is structural/intensional identity, not proof of behavioral equivalence. `List.head (sort xs)` and a hypothetical `minimum xs` need not share a hash even if extensionally equivalent. Compiler normalization and builtin identity form part of the trusted format. |
| **Codebase manager (`ucm`)** | `.u` files are transient scratch/editing surfaces. UCM watches them, parses and typechecks on save, and `add` or `update` stores definitions and changes namespace mappings in the codebase. `update` attempts to rewrite compatible dependents and returns unresolved ones to the scratch file. [web:376][web:377] | The authoritative working object is a structured codebase database/history rather than a directory of independently authoritative source files. | Conventional Unix/Git tooling—text grep, patches, line-oriented merge drivers, code-host diffs, and ad hoc generators—does not automatically understand that database. Text export/transcripts and Unison-aware hosting/tooling are therefore important. |
| **“No builds”** | A stored hash denotes an immutable, already parsed and typechecked definition. Its cached typechecking/compilation result never needs invalidation for that hash. [web:388] | There is no ordinary “rebuild every file and relink the application” phase after a rename or unrelated edit. Tests and compiled forms can be cached by immutable dependency hash. | New scratch code still has to be parsed and typechecked; new definitions and updated transitive dependents still require work. A runtime still has to interpret or compile code for its target once. “No builds” means a persistent, perfect-keyed shared cache—not zero compilation. |
| **Renames are free** | Namespace names are separate mappings from human-readable names to immutable hashes. | Renaming changes metadata, not the referenced term, its dependents, or compiled artifact. Name collisions can be disambiguated with hash prefixes. [web:124][web:128] | Documentation, serialized external protocols, UI strings, reflection-like name use, deployment configuration, and foreign systems can still care about names. A semantic API replacement is not a rename. |
| **No dependency conflicts** | Two versions of a definition have different hashes and can coexist in one codebase; dependencies point directly to the chosen hash rather than competing to own one global package/name slot. [web:122] | The classic diamond conflict where two libraries require incompatible versions of the same named package need not be resolved at link time. | Version multiplicity still has costs: duplicated code/data, incompatible values crossing a boundary, competing database schemas, singleton resources, protocol versions, security patch rollout, and type incompatibility. Content addressing removes *name-resolution conflict*, not all semantic or operational dependency conflict. |
| **Structural sharing** | The codebase is an immutable Merkle-style DAG. Unchanged definitions and namespace/history substructures retain the same hashes and are shared across branches and updates. [web:100] | Branches and versions store common definitions once; distribution can send only missing hashes. | Complete history and reachability metadata still need indexing, compaction, backup, and garbage-collection policies. Large namespaces have historically exposed search and definition-loading performance problems. [web:103] |
| **Distributed execution** | A serialized computation contains references to immutable code hashes. A remote runtime can request only missing definitions, cache them, and execute after reconstructing the dependency closure. Unison’s `Remote` ability exposes operations for moving or starting computations. [web:122][web:126] | Code mobility becomes content synchronization rather than packaging and deploying a filesystem artifact. Hash equality gives a strong cache and integrity key. | The hash says *which bytes/AST*, not whether code is authorized, safe, resource-bounded, compatible with the host, or entitled to capabilities. Effects, credentials, network partitions, schema evolution, native dependencies, observability, and live state remain deployment concerns. |
| **Collision resistance and integrity** | Full identities use SHA3-512; short textual prefixes are expanded in codebase context. [web:100][web:104] | Accidental full-hash collision is practically irrelevant, and transferred definitions can be checked against their expected identity. | Hashes are not signatures: they establish integrity relative to an expected digest, not author identity or trust. Prefixes can become ambiguous as a codebase grows. |
| **Ecosystem/interop** | Unison supplies its own language, effect system, runtime, codebase format, package workflow, and cloud model. | The design can make content identity pervasive instead of being bolted onto a conventional linker. | Adoption cost is substantial. The library ecosystem is much smaller than mainstream ecosystems, and reporting in 2024 noted the lack of a stable FFI, requiring reimplementation of functionality readily available elsewhere. [web:101] |
| **Debugging and evolution** | Every historical definition remains addressable, and UCM can display names or hash-qualified names. | Old references do not become dangling merely because the preferred namespace changed. | Stack traces, profiling, source mapping, audit review, and operational diagnosis need historical name/source metadata. Updating one function creates a new immutable world rather than mutating the old one; UCM must propagate or mediate that change through dependents. |

**Assessment:** Unison’s strongest contribution is not “hashing source.” Build systems already do that. It makes **resolved, alpha-normalized semantic definitions and their exact dependencies the unit of identity**, turning the codebase into a persistent Merkle DAG. The trade is equally fundamental: ordinary text files and package-version names cease to be the primary code identity, requiring an integrated database, editor workflow, history model, runtime, registry, and debugging ecosystem.

---

## 3. Fine-grained incrementality below the file level

### 3.1 Why file-level tracking is too coarse

Suppose `util.ts`, `Utils.kt`, or `util.hpp` defines 500 independent declarations and one private function body changes. A file graph records:

```text
file A changed → invalidate every file that imported A
```

A definition graph can instead distinguish:

```text
body(f) changed
public_signature(f) unchanged
constant_value(c) unchanged
layout(Type) unchanged
```

Only analyses or artifacts that read `body(f)` need invalidation. The ideal node is therefore not necessarily a file, but a semantic fact:

```text
parse subtree
definition header
inferred type
method-resolution result
constant value
type layout
inline body
macro expansion
effect set
optimized machine-code fragment
```

Finer granularity reduces false dependencies but increases graph nodes, edges, hash/equality work, memory, serialization, scheduling, and the risk of missing a dependency. The useful granularity is where the cost of an edge is lower than the expected avoided recomputation.

### 3.2 Approaches

| Approach | Mechanism | Benefit over file-level tracking | Residual problem |
|---|---|---|---|
| **Symbol/definition dependency graph** | Record that a particular function body, type inference, or declaration lookup consumed a particular symbol’s signature, body, layout, constant, or implementation. | A private body edit need not invalidate callers that depend only on the signature; an unrelated declaration in the same file remains reusable. Rust queries, Kotlin lookup storage, Swift provided/required names, and Roslyn generator models all approximate this at different stages. | Overloads, wildcard imports, extension methods, trait/instance search, negative lookup, macros, reflection, and “all implementations of trait X” create nonlocal dependencies. The graph must record failed searches and open-world sets, not just successful symbol references. |
| **Interface or ABI hashing** | Project each module/class into a compact public signature and hash it. | Implementation-only edits stop at the interface firewall, giving Gradle compilation avoidance, TypeScript `.d.ts` boundaries, and coarse Kotlin classpath snapshots. | Inline functions, templates/generics, constants, layout, annotations, macro definitions, default arguments, and optimization summaries may be semantically part of the interface even when not obvious in a nominal ABI. |
| **Persistent syntax trees** | Re-lex and reparse the smallest safe region; reuse immutable unaffected subtrees and rebuild only ancestors to the root. | Typing in one method does not allocate or parse the remainder of a large file again. Roslyn’s green nodes can move when text is inserted before them because they store width but not absolute position. [web:20] | Parsing context can propagate farther than the edit: unmatched delimiters, indentation sensitivity, preprocessor conditionals, lexer modes, contextual keywords, and error recovery may force a large reparse. Semantic invalidation is separate from syntax reuse. |
| **Per-definition content addressing** | Hash normalized definitions and direct dependency identities, as in Unison. | Equality and cache identity survive path moves and renames; identical definitions are naturally deduplicated. | A dependency update changes all updated dependent hashes, creating a Merkle cascade. Fine identity does not by itself supply efficient reverse-dependency indexes or state migration. |
| **C/C++ textual-header scanning** | The preprocessor records every included file, usually into `.d` files. Any changed included header invalidates the translation unit. | Soundly captures the actual include closure under a particular macro/configuration environment. | It remains extremely coarse: one character in a widely included header can rebuild thousands of translation units. Macro state means a header is not a context-independent module. |
| **Include What You Use (`IWYU`)** | Analyze every used symbol and require the source/header to include the header that exports it directly; remove unnecessary transitive includes and use forward declarations when suitable. [web:166] | Shrinks include closures, makes dependencies explicit, and reduces fan-out when unrelated headers change. | It improves the graph rather than changing the compilation model. Macros, template completeness requirements, platform mappings, private/public re-export policy, generated headers, and build-specific flags require configuration and judgment. |
| **Clang dependency scanning** | `clang-scan-deps` rapidly preprocesses commands from a compilation database, discovers header and module dependencies, and can emit full module graphs or C++ P1689 dependency information. [web:136][web:139] | Build systems learn BMI/module ordering before compilation, can build modules explicitly, and can create hermetic cache keys from the discovered inputs. | Dependency discovery is itself work and is command-line/macro-context sensitive. The scan must exactly model compilation or risk missing dependencies; generated sources and header maps complicate scheduling. |
| **C++20 named modules** | Module interface units explicitly `export` declarations and compile to compiler-specific binary module interfaces (BMIs). Imports replace repeated textual inclusion for module clients. | A module’s interface can be parsed once and imported; implementation partitions and non-exported changes need not force all clients to preprocess a giant header closure. | BMI formats and command lines are compiler/version/flag sensitive. Build systems must first discover imports and construct an acyclic schedule; header units and legacy includes preserve many preprocessor complications. |
| **Clang modules and module maps** | A `module.modulemap` declares how existing headers map into logical modules, including umbrella, private, excluded, and textual headers. [web:137] | Legacy header libraries gain module-like caching without rewriting every header as a C++20 module. | This is the **module-map problem**: the build/toolchain supplies a second description of library ownership and visibility that can disagree with installation layout, conditional headers, framework maps, re-exports, and the headers’ actual macro behavior. Different maps can assign a header to different modules. Textual or non-modular headers break isolation, and implicit module discovery makes cache keys and build order harder to make hermetic. |
| **CAS-backed explicit modules** | Scan once, place discovered headers/module maps and outputs into a content-addressed store, then run explicit compiler commands against CAS inputs. | Separates expensive filesystem/module discovery from compilation and makes remote reuse practical. LLVM work describes dependency scanning ingesting include trees into CAS so compilation no longer searches for headers or parses module maps. [web:148] | Correctness moves into the scanner and CAS key: command-line flags, VFS overlays, generated files, target, SDK, compiler revision, environment, and macro definitions must all be represented. |

A particularly important distinction is:

```text
fine-grained invalidation ≠ fine-grained emission
```

rustc may retain item-level type/MIR results yet regenerate a whole dirty CGU. Swift can track declaration-name dependencies but schedule whole source files. A language server may recompute one function’s inference but still publish diagnostics as a complete document set.

---

## 4. Hot code reloading and live runtime updates

Compilation incrementality answers, “What must be recomputed?” Hot reload adds three harder questions:

1. How are future calls redirected to new code?
2. What happens to calls already executing old code?
3. How are old heap objects, actor state, resources, and persistent data transformed?

### 4.1 Runtime systems

| System | Code replacement mechanism | Treatment of live state/execution | Principal constraints |
|---|---|---|---|
| **Erlang/OTP hot code loading** | BEAM can keep current and old versions of a module. External calls go to current code, while a process executing old code can continue until it makes a fully qualified external call. | OTP behaviors expose `code_change/3`; for `gen_server`, it receives the old version and current process state and returns migrated state. [web:186] | Only a bounded number of module versions coexist; lingering processes must leave old code before it can be purged. Message formats, ETS tables, external services, and distributed rolling-version compatibility need explicit design. Core emulator changes can require restart. |
| **Erlang `.appup` / `relup`** | Each changed application supplies an `.appup` describing upgrade and downgrade instructions. Release tooling combines them into `relup` for the whole release. [web:181] | Instructions suspend/resume processes, load modules, call state-change functions, restart components, and sequence dependencies. | Release scripts are operational programs that must cover both directions and all supported source versions. ERTS, Kernel, STDLIB, and SASL upgrades may require `restart_new_emulator`, so “hot” is not universal. [web:188] |
| **Smalltalk images** | Methods and classes are objects in a live reflective system; recompiling a method updates method dictionaries used by subsequent sends. The image stores a snapshot of the object graph. | Existing objects remain in the image; systems provide class reshaping or migration mechanisms when instance variables change. Development, debugger, browser, application, and heap occupy one live world. [web:211][web:217] | Old activations may keep old compiled methods; class-layout evolution can require migrating every instance. Reproducible source builds, deployment hygiene, image drift, native plugins, and external resources remain separate concerns. |
| **Common Lisp images** | Compiling/loading a new `defun` changes a symbol’s global function definition, so later indirect calls use the new function. SLIME commonly compiles forms directly into the running Lisp process. | The heap and process stay alive. Programmers can write explicit object reinitialization or class-update protocols, depending on the object system. | Inlined or statically resolved calls may retain old code; active frames continue old instructions. Changed class layouts, generic-function protocol, threads, native code, and serialized data need coordination. |
| **JVM HotSwap** | Debugger/JVMTI class redefinition replaces method bodies and associated bytecode/constant-pool information. Future entries use new code; an existing frame generally remains obsolete until it returns. | Existing object layout remains unchanged, which avoids automatic heap migration. | Standard HotSwap generally cannot add/remove fields or methods, change signatures, or alter inheritance. [web:198][web:199] Compiler-generated synthetic members can make seemingly small source edits structural. |
| **DCEVM** | Patches HotSpot to permit enhanced class redefinition, including many structural and hierarchy changes. | The VM redirects/redefines metadata and deoptimizes compiled code so execution returns through updated representations. | Deoptimization can purge large portions of the code cache and temporarily hurt performance; support depends on a modified VM and not every transformation or object-state migration is transparent. [web:197] |
| **JRebel** | A Java agent watches changed class/resource files and uses class transformation, indirection, and framework-specific integration to emulate broader reloadability than standard HotSwap. | Existing objects are generally preserved; added members may be represented through auxiliary state rather than physically changing every old object. | Commercial tooling, agent/runtime overhead, framework integration, and semantic limits remain. It cannot infer arbitrary migrations or undo already executed initialization and side effects. |
| **.NET Hot Reload / Edit and Continue** | Roslyn compares old and new compilations, classifies semantic edits, and emits IL, metadata, and PDB deltas. The runtime applies them to the loaded assembly. [web:243][web:245] | Existing heap objects remain. New method bodies affect subsequent execution; supported additions can extend metadata, while closure/async state-machine mappings impose constraints. | Unsupported changes are **rude edits** and require restart. Support depends on runtime, target, debugger, framework, generic context, active statement, lambda capture, iterator, and async transformation. [web:241][web:242] |
| **Rust: Dioxus/Subsecond** | Instrumented `subsecond::call` sites dispatch through a jump table. ThinLink and the Dioxus CLI compile/link changed Rust functions and install a new function-address table in the process. [web:259][web:270] | The surrounding application process and Dioxus state remain. Calls made through patch points reach new functions; one-time startup code does not rerun automatically. | Experimental and integration-dependent. Function ABI, types, monomorphizations, vtables, statics, allocator ownership, active frames, and already-created values constrain safe replacement. Only calls crossing a patchable indirection are guaranteed to update. |
| **Rust: `hot-lib-reloader`** | A procedural macro maintains a `LibReloader`, watches a dynamic-library file, reloads it, and updates access to exported symbols. [web:257] | The host should own persistent state or explicitly transfer it across library versions. Reload notifications allow hooks around the transition. | Rust has no stable native ABI. Exported interfaces usually need C ABI or carefully synchronized compiler/crate versions. Trait objects, generics, unwinding, allocators, TLS, statics, and types crossing the dylib boundary are hazardous. |
| **C/C++ shared-library reload** | Compile reloadable logic as `.so`/DLL; watch the artifact; load with `dlopen`/`LoadLibrary`; obtain functions using `dlsym`/`GetProcAddress`; atomically replace a dispatch table; unload old code after quiescence. | Keep durable state in the host, expose it through opaque pointers, or load old and new DLLs simultaneously and run an explicit state-transfer function. [web:226][web:240] | No thread may be executing old code at unload. No remaining function pointer, vtable, exception-unwind record, TLS destructor, callback, or object virtual method may refer to it. C++ ABI and layout changes make arbitrary class reload particularly fragile. |
| **Ksplice** | Derives replacement object code by comparing pre/post kernel builds, checks that affected code is safe to replace, and redirects execution without reboot. | Traditionally uses global quiescence/stack safety before switching affected functions. | Intended mainly for bounded security and correctness patches, not arbitrary kernel redesign or data-structure migration. Compiler differences can make binary differencing difficult. |
| **kpatch** | Builds livepatch modules containing changed functions and redirects function entries, historically using stop-machine/stack checks and later integrating with the upstream livepatch model. | Tasks must not be executing an unsafe old/new mixture. | Long-running functions or persistent stacks can delay transition; data layout changes require special handling. |
| **kGraft** | Introduced per-task consistency: each task sees one coherent code universe and switches at safe kernel/user boundaries rather than stopping all execution globally. | Tasks transition independently after leaving affected old execution. | Old and new function sets can coexist during transition, so shared-data invariants must be compatible with both versions. |
| **Linux kernel livepatch** | Upstream livepatch redirects patched function entries, normally using ftrace. Its consistency model combines kGraft per-task/syscall-boundary switching with kpatch stack inspection. [web:286][web:288] | A task switches only when no affected old function is on its stack or when it crosses an accepted safe boundary. Shadow variables can attach new state to existing kernel objects without changing their layout. [web:295][web:299] | Suitable for targeted fixes, not arbitrary release upgrades. Assembly, inlining, changed data semantics, callbacks, persistent kernel objects, and functions that never leave stacks complicate safety. |
| **eBPF** | Userspace loads verified bytecode with `BPF_PROG_LOAD`, creates maps, and attaches programs to tracepoints, networking hooks, cgroups, LSM hooks, kprobes/uprobes, or other targets. `BPF_LINK_UPDATE` can replace the program behind a link. [web:302][web:304] | Maps live independently of one invocation and can be pinned or reused by a replacement program, providing an explicit state boundary. | New and old programs must agree on map key/value layouts and semantics. The verifier, helper availability, hook ABI/BTF/CO-RE assumptions, bounded execution, privileges, and kernel version limit expressiveness. This is dynamic extension, not arbitrary replacement of all kernel code. |
| **WebAssembly core dynamic linking** | Emscripten/toolchain ABIs can place modules in shared linear memory/table and resolve symbols similarly to native shared libraries. | State can remain in shared memory while a table entry or module instance changes. | Modules must agree on a low-level ABI, memory/table ownership, allocator, relocation model, and toolchain conventions. Shared memory makes isolation and migration harder. |
| **WebAssembly Component Model** | Components expose typed WIT imports/exports. The Canonical ABI lowers/lifts WIT values across core Wasm memories. Composition wires imports to dependency exports; children may be embedded or imported. [web:275][web:278] | Shared-nothing components can be instantiated, called, and destroyed behind typed capability boundaries, making replacement cleaner than sharing arbitrary pointers. | The MVP’s “dynamic linking” often means modules remain separate but the dependency set is declared before execution, allowing AOT compilation; it is not automatically arbitrary hot replacement. [web:271] Long-lived resources, handles, streams, component state, and interface-version adaptation still require host protocols. |

### 4.2 The fundamental state-migration problem

Code replacement is comparatively easy when the ABI is unchanged:

```text
dispatch["f"] = address(new_f)
```

A real update may change:

```text
StateV1 { users: Map<Id, Name> }
       ↓
StateV2 { users: Map<Id, User>, revision: u64 }
```

A safe live update must address all of the following:

| State category | Required migration decision |
|---|---|
| **Active stack frames** | Finish old code, deoptimize/restart a frame, transform locals and program counter, or retain old and new code until quiescence. |
| **Heap objects** | Eagerly transform all objects, lazily upgrade on access, add side tables/shadow fields, preserve old layouts, or maintain versioned accessors. |
| **Function pointers/vtables/callbacks** | Rebind every indirect reference or retain old code as long as any reference remains. |
| **Actors and queues** | Convert process state and support both old and new message formats during rolling transition. |
| **Persistent storage** | Apply forward/backward schema migrations and coordinate code deployment with database compatibility windows. |
| **External resources** | Decide whether sockets, file descriptors, GPU state, locks, transactions, credentials, and remote leases can transfer or must be recreated. |
| **Already executed effects** | Initialization, emails, payments, writes, registrations, and spawned threads cannot generally be “hot reloaded away”; updates must be idempotent or compensated. |
| **Distributed protocol state** | Mixed-version nodes need compatible wire formats, feature negotiation, rollback rules, and consensus over irreversible changes. |
| **Type/ABI evolution** | Supply explicit converters and define what happens when conversion fails. A type-correct new program does not imply a migration from every valid old state. |

This explains the recurring design pattern across Erlang, C DLL reloaders, livepatch, eBPF, and component runtimes:

> **Keep code replaceable through indirection, keep persistent state behind a narrow versioned boundary, and make migration an explicit operation.**

No hash, dependency graph, incremental compiler, or dynamic linker can generally infer the intended relationship between two arbitrary state schemas.

---

## 5. Language servers as incremental compilers

The LSP wire protocol itself merely standardizes JSON-RPC messages between an editor and a language server for features such as completion, definitions, references, and diagnostics. It does not prescribe how analysis is computed or cached. [web:353][web:356]

Operationally, however, a good language server is a persistent, speculative, error-tolerant incremental compiler front end:

```text
didOpen/didChange
      ↓
persistent text + syntax database
      ↓
on-demand name resolution/type inference/control-flow queries
      ↓
completion / hover / references / diagnostics / rename
```

| Requirement | Why it makes an LSP an incremental compiler |
|---|---|
| **Persistent process** | It retains parsed trees, indexes, module graphs, type facts, and caches across thousands of keystrokes rather than starting from zero. |
| **Demand-driven semantics** | Completion at one cursor needs scope, expected type, candidates, and perhaps trait solving—but not optimized code generation for the whole workspace. |
| **Fine-grained cancellation** | A result for revision `N` must be abandoned or rebased when revision `N+1` arrives; long queries need cancellation points and immutable snapshots. |
| **Error tolerance** | Analysis must produce useful syntax, types, and names for incomplete, temporarily invalid code that a batch compiler could reject immediately. |
| **Multiple configurations** | Unsaved buffers, generated files, conditional compilation, target flags, features, macros, and build-script outputs must be represented as inputs. |
| **Low-latency early cutoff** | A comment or local body edit should not re-index dependencies or recompute unrelated modules. Durability and firewall queries are especially valuable. |
| **Partial outputs** | Hover may require one expression’s inferred type; references need a global index; diagnostics may be computed in background priority order. |

### 5.1 Why the LSP/compiler split duplicates work

| Duplication | Cause | Consequence | Better architecture |
|---|---|---|---|
| **Lexer/parser and syntax trees** | Batch compilers historically optimize for one complete build; IDEs require lossless, editable, error-tolerant persistent trees. | Two ASTs, two parsers, different recovery behavior, and conversion layers. | Share a lossless parser/green tree, or define stable syntax interfaces usable by both batch and IDE front ends. |
| **Name resolution and imports** | IDEs need approximate answers in broken code and incremental updates; compilers traditionally assume a coherent compilation unit. | Go-to-definition or completion can disagree with actual compilation. | Express resolution as fine-grained queries with explicit strict and recovery modes. |
| **Type inference and trait/overload solving** | Compiler solvers may depend on batch-global arenas and lifetimes; language servers need cancellable per-body work. | Duplicated algorithms drift in diagnostics and accepted programs. | Extract solver IR and algorithms behind database-independent traits. rust-analyzer now shares several `rustc_*` crates, including type IR and the next trait solver, although coercion and some obligation logic remain duplicated. [web:331][web:348] |
| **Macro/build-system evaluation** | LSP runs continuously and may sandbox or approximate arbitrary build scripts/procedural macros. | Editor view differs from the exact configured build; macro expansion can dominate latency. | Treat build outputs, target configuration, environment, and macro-server results as explicit durable inputs with content-addressed caches. |
| **Dependency graph and cache** | The compiler may persist a rustc-style on-disk graph while the LSP separately maintains Salsa/workspace caches. | The same source is parsed and typed once by the language server and again by the build. Memory and implementation effort double. | Use one shared query engine or share stable semantic artifacts/CAS objects between processes. |
| **Diagnostics** | IDE diagnostics prioritize speed and tolerance; build diagnostics prioritize completeness and exact source configuration. | Duplicate messages, flicker, conflicting spans, and work repeated when `cargo check`, `dotnet build`, or `tsc` runs. | Schedule strict compiler queries in the same persistent service and publish revision-stamped results. |
| **Indexes and metadata** | LSP needs reverse-reference/search indexes; compilers primarily need forward dependencies for emission. | Both systems separately walk symbols and serialize similar facts. | Generate forward and reverse indexes from one semantic query graph or immutable definition store. |

Rust is the clearest example. rust-analyzer “effectively reimplements the Rust compiler in a fully incremental, on-demand style,” while the long-term plan is library-ification so rustc and rust-analyzer become separate front ends over shared compiler components. [web:332][web:347] rust-analyzer already performs parsing, AST/HIR lowering, inference, limited MIR construction, and constant evaluation—enough that it is accurately described as a compiler front end. [web:331]

Roslyn demonstrates the more integrated alternative: compiler APIs, immutable syntax trees, semantic models, analyzers, generators, Edit and Continue, and IDE services share the same object model. Even there, however, a persistent IDE workspace, compiler-server process, MSBuild task graph, command-line invocation, generator driver, and runtime Hot Reload session have different cache lifetimes and invalidation requirements.

## Final synthesis

| Layer | Best reusable identity | Main invalidation question | Hardest unresolved issue |
|---|---|---|---|
| **Text/syntax** | File version plus persistent subtree identity | Which tokens/subtrees changed? | Context-sensitive lexing, parsing, and recovery |
| **Semantic analysis** | `(query, stable definition key)` plus dependency graph | Which facts actually consumed the changed fact? | Complete tracking of open-world lookup, macros, environment, and negative dependencies |
| **Artifact generation** | Interface/body fingerprints, CGU/module/CAS key | Which object/BMI/generated output remains valid? | Granularity versus optimization, storage, and remote-cache economics |
| **Content-addressed code** | Hash of normalized AST plus dependency hashes | Does an identical immutable definition already exist? | Tooling/database adoption, interop, semantic upgrades, and operational policy |
| **Language server** | Revisioned immutable workspace plus demand-driven query keys | What must be recomputed for the current user request? | Sharing exact compiler semantics without sacrificing latency and error tolerance |
| **Runtime update** | Versioned entry point plus explicit state schema | Which future calls use new code, and when is old code quiescent? | Migrating active frames, heap/external state, effects, and distributed protocols |

The practical 2024–2026 direction is therefore a convergence of:

```text
stable semantic identities
+ immutable/persistent IR
+ dynamically recorded fine-grained dependencies
+ value-sensitive early cutoff
+ durability or interface firewalls
+ content-addressed local/remote artifact stores
+ explicit runtime indirection
+ explicit versioned state migration
```

Query systems and content addressing can make recomputation nearly optimal. They cannot remove semantic change itself: once an edit changes an interface, data model, effect, or protocol, the system must either propagate that change, preserve both versions, adapt between them, or stop and restart.

Citations:
[31] https://rustc-dev-guide.rust-lang.org/queries/incremental-compilation-in-detail.html
[32] https://rustc-dev-guide.rust-lang.org/queries/incremental-compilation.html
[33] https://deepwiki.com/rust-lang/rustc-dev-guide/3.2-compiletest-reference
[34] https://doc.rust-lang.org/stable/nightly-rustc/rustc_monomorphize/partitioning/index.html
[35] https://doc.rust-lang.org/nightly/nightly-rustc/rustc_monomorphize/partitioning/index.html
[36] https://doc.rust-lang.org/stable/nightly-rustc/rustc_middle/dep_graph/dep_node_key/trait.DepNodeKey.html
[37] https://github.com/nikomatsakis/rustc-on-demand-incremental-design-doc/blob/master/0000-rustc-on-demand-and-incremental.md
[38] https://deepwiki.com/rust-lang/rustc-dev-guide/6.2-monomorphization
[39] https://deepwiki.com/rust-lang/rustc-dev-guide/4.7-the-query-system
[40] https://rustc-dev-guide.rust-lang.org/backend/monomorph.html
[41] https://rust-lang-rust-73.mintlify.app/compiler/incremental
[42] https://blog.rust-lang.org/2021/05/10/Rust-1.52.1/
[43] https://nnethercote.github.io/2023/07/11/back-end-parallelism-in-the-rust-compiler.html
[44] https://doc.rust-lang.org/beta/nightly-rustc/rustc_incremental/persist/index.html
[45] https://deepwiki.com/rust-lang/rustc-dev-guide/6-code-generation
[1] https://rust-analyzer.github.io/blog/2023/07/24/durable-incrementality.html
[2] https://deepwiki.com/rust-lang/rust-analyzer/2.2-salsa-and-incremental-computation
[3] https://deepwiki.com/that-in-rust/transfiguration-cozo-graph-compiler/5.3-reference-implementations
[4] https://docs.rs/rust-analyzer-salsa/latest/salsa/
[5] https://github.com/salsa-rs/salsa
[6] https://docs.rs/salsa/latest/salsa/
[7] https://docs.rs/rust-analyzer-salsa/latest/src/salsa/lib.rs.html
[8] https://rust-analyzer.github.io/book/contributing/guide.html
[9] https://sparkles-docs.pages.dev/research/parsing/rust-analyzer
[10] https://rustc-dev-guide.rust-lang.org/queries/salsa.html
[11] https://deepwiki.com/that-in-rust/transfiguration-cozo-graph-compiler/6.2-phase-2:-salsa-+-redb-hybrid
[12] https://docs.rs/salsa
[13] https://github.com/rust-analyzer
[14] https://salsa-rs.github.io/salsa/reference/algorithm.html
[15] https://tessl.io/registry/skills/github/joshuadavidthomas/agent-skills/salsa
[16] https://deepwiki.com/dotnet/roslyn/3-compiler-architecture
[17] https://github.com/dotnet/roslyn/blob/main/docs/features/incremental-generators.cookbook.md
[18] https://deepwiki.com/dotnet/roslyn/3.2.1-syntax-trees-and-parsing
[19] https://roxeem.com/2025/12/15/the-net-pipeline-that-makes-source-generators-feel-instant/
[20] https://github.com/dotnet/roslyn/blob/main/docs/compilers/Design/Red-Green%20Trees.md
[21] https://github.com/dotnet/roslyn/blob/e091728607ca0fc9efca55ccfb3e59259c6b5a0a/src/Compilers/Core/Portable/Syntax/SyntaxTree.cs
[22] https://github.com/dotnet/roslyn/blob/main/docs/features/source-generators.cookbook.md
[23] https://stackoverflow.com/questions/25963328/how-does-the-release-version-of-roslyn-implement-immutable-trees
[24] https://langdev.stackexchange.com/questions/2876/what-are-some-techniques-for-faster-fine-grained-incremental-compilation-and-st
[25] https://github.com/dotnet/roslyn/discussions/71441
[26] https://www.thinktecture.com/en/net/roslyn-source-generators-performance/
[27] https://stackoverflow.com/questions/51737748/are-roslyns-code-generation-apis-immutable
[28] https://deepwiki.com/dotnet/roslyn/3.1-compilation-pipeline
[29] https://stackoverflow.com/questions/71607413/how-does-the-compiler-pipeline-correspond-to-incremental-source-generation
[30] https://github.com/riok/mapperly/issues/72
[61] https://download.swift.org/docs/assets/generics.pdf
[62] https://github.com/swiftlang/swift/blob/main/docs/Driver.md
[63] https://swiftdeps.com/
[64] https://forums.swift.org/t/rfc-file-hashes-for-swift-incremental-compilation/80334
[65] https://deepwiki.com/swiftlang/swift-driver
[66] https://github.com/swiftlang/swift/blob/master/docs/CompilerPerformance.md
[67] https://forums.swift.org/t/detecting-symbol-dependency-cycles-in-order-to-minimize-them-for-simpler-incremental-builds/39236
[68] https://swiftpackageindex.com/simonbs/dependency-graph
[69] https://github.com/swiftlang/swift/blob/main/docs/DriverInternals.md
[70] https://www.linkedin.com/posts/antoniopicone_github-colbymchenrycodegraph-pre-indexed-activity-7457154284732739585-Bry-
[71] https://forums.swift.org/t/where-can-i-find-detailed-documentation-about-how-swift-projects-are-compiled/34587
[72] https://forums.swift.org/t/swift-incremental-build-external-dependencies/37271
[73] https://lists.swift.org/pipermail/swift-dev/Week-of-Mon-20160404/001654.html
[74] https://www.dcard.tw/f/tech_job/p/258525159
[75] https://github.com/reaktor/pelam-swift-dep-checker
[76] https://kotlinlang.org/docs/gradle-compilation-and-caches.html
[77] https://blog.jetbrains.com/kotlin/2022/07/a-new-approach-to-incremental-compilation-in-kotlin/
[78] https://docs.gradle.org/current/userguide/incremental_build.html
[79] https://docs.gradle.org/current/userguide/performance.html
[80] https://blog.gradle.org/introducing-incremental-build-support
[81] https://docs.gradle.org/current/userguide/gradle_optimizations.html
[82] https://blog.gradle.org/compilation-avoidance
[83] https://discuss.gradle.org/t/compile-avoidance-in-custom-tasks/32171
[84] https://blog.gradle.org/incremental-compiler-avoidance
[85] https://kotlin.openaidoc.org/ja/gradle-compilation-and-caches/
[86] https://jfokus.se/jfokus17/preso/Gradle-3-0-and-Beyond.pdf
[87] https://docs.gradle.org/3.4/release-notes.html
[88] https://stackoverflow.com/questions/27987810/kotlin-incremental-compilation-with-gradle/27991481
[89] https://blog.gradle.org/our-approach-to-faster-compilation
[90] https://zerobug.club/en/android/incremental-build/
[46] https://www.typescriptlang.org/docs/handbook/project-references.html
[47] https://www.typescriptlang.org/docs/handbook/release-notes/typescript-3-4.html
[48] https://dev.to/gabrielanhaia/typescript-project-references-11-minute-builds-down-to-3-2kif
[49] https://www.typescriptlang.org/tsconfig/incremental.html
[50] https://stackoverflow.com/questions/67093879/are-typescript-project-references-helpful-with-noemit
[51] https://jsguides.dev/guides/ts-project-references/
[52] https://typescript.dev.org.tw/docs/handbook/project-references.html
[53] https://deepwiki.com/microsoft/TypeScript/8-incremental-and-project-builds
[54] https://mimizou.mamansoft.net/it-note/languages/typescript/releases/3.4/
[55] https://www.typescriptlang.org/docs/handbook/release-notes/typescript-3-0.html
[56] https://gibbok.github.io/myvar/typescript/faster-subsequent-builds/
[57] https://moonrepo.dev/docs/guides/javascript/typescript-project-refs
[58] https://jsmanifest.com/typescript-project-references-scale
[59] https://stackoverflow.com/questions/69157126/in-tsc-build-how-to-overcome-incremental-and-tsbuildinfofile
[60] https://runebook.dev/en/docs/typescript/tsconfig/index/references-config
[106] https://www.cs.tufts.edu/~jfoster/papers/cs-tr-5027.pdf
[107] https://arxiv.org/abs/1503.07792
[108] http://reports-archive.adm.cs.cmu.edu/anon/2005/abstracts/05-129.html
[109] http://matthewhammer.org/adapton/adapton-slides.pdf
[110] https://dl.acm.org/doi/10.1145/2594291.2594324
[111] https://software.imdea.org/~rleywild/publications/popl09/popl09.pdf
[112] https://arxiv.org/pdf/1106.0478
[113] https://www.alphaxiv.org/@michael-w-hicks
[114] http://www.cs.cmu.edu/~blelloch/papers/3409964.3461799.pdf
[115] https://people.cs.uchicago.edu/~duru/papers/traceable-pldi10.pdf
[116] https://docs.rs/adapton
[117] https://docs.racket-lang.org/adapton/index.html
[118] https://plum-umd.github.io/adapton/
[119] https://dl.acm.org/doi/10.1145/2967973.2968610
[120] https://dl.acm.org/doi/10.1145/2889160.2889229
[121] https://news.ycombinator.com/item?id=22156370
[122] https://www.unison-lang.org/docs/the-big-idea/
[123] https://renato.athaydes.com/posts/unison-revolution.html
[124] https://softwaremill.com/trying-out-unison-part-1-code-as-hashes/
[125] https://devblogs.sh/posts/unison-a-new-distributed-programming-language-by-paul-chiusano
[126] https://softwaremill.com/trying-out-unison-part-4-from-the-edge-to-the-cloud/
[127] https://www.joyfulprogramming.com/p/the-unison-edition
[128] https://lwn.net/Articles/978955/
[129] https://univalence.io/blog/articles/unison-langage/
[130] https://sparkles-docs.pages.dev/research/algebraic-effects/unison
[131] https://rosettalens.com/s/ko/docs-the-big-idea-simplifying-distributed-programming
[132] https://www.unison-lang.org/docs/tour/_big-technical-idea/
[133] https://www.unison-lang.org/blog/where-unison-is-headed/
[134] https://github.com/unisonweb/unison/blob/trunk/README.md
[135] https://scrapbox.io/mrsekut-p/Unison
[91] https://cohost.org/typesanitizer/post/346051-some-downsides-of-co
[92] https://www.reddit.com/r/programming/comments/d55jr3/unison_a_new_distributed_programming_language/
[93] https://news.ycombinator.com/item?id=22156370
[94] https://www.unison-lang.org/docs/the-big-idea/
[95] https://news.ycombinator.com/item?id=34307552
[96] https://www.reddit.com/r/programming/comments/o9679a/unison_a_new_programming_language_with_immutable/
[97] https://www.reddit.com/r/ProgrammingLanguages/comments/umq1hv/the_unison_language_thoughts/
[98] https://www.reddit.com/r/ProgrammingLanguages/comments/o967gi/unison_a_new_programming_language_with_immutable/
[99] https://www.joyfulprogramming.com/p/the-unison-edition
[100] https://www.unison-lang.org/docs/usage-topics/general-faqs/
[101] https://lwn.net/Articles/978955/
[102] https://composure.systems/catalog/related-work/unison
[103] https://github.com/unisonweb/unison/issues/2045
[104] https://www.unison-lang.org/docs/language-reference/hashes/
[105] https://sparkles-docs.pages.dev/research/algebraic-effects/unison
[151] https://apps.dtic.mil/sti/tr/pdf/ADA228726.pdf
[152] https://www.ncameron.org/rfcs/1298
[153] https://forums.swift.org/t/detecting-symbol-dependency-cycles-in-order-to-minimize-them-for-simpler-incremental-builds/39236
[154] https://clef-lang.com/spec/draft/incremental-computation/
[155] https://langdev.stackexchange.com/questions/2876/what-are-some-techniques-for-faster-fine-grained-incremental-compilation-and-st
[156] https://www.pl.informatik.uni-mainz.de/files/2019/04/pluto-incremental-build.pdf
[157] https://dev.to/simmypeet/building-a-query-based-incremental-compilation-engine-in-rust-nj6
[158] https://softwarepreservation.computerhistory.org/interactive_c/bib/Cooper-1997.pdf
[159] https://dziban.net/posts/incremental-computation-graph/
[160] https://conf.researchr.org/details/cgo-2024/cgo-2024-main-conference/27/Enabling-Fine-Grained-Incremental-Builds-by-Making-Compiler-Stateful
[161] https://rust-lang-rust-73.mintlify.app/compiler/incremental
[162] https://blog.ezyang.com/2016/08/optimizing-incremental-compilation/
[163] https://blog.goose.love/posts/improving-the-incremental-system-in-the-rust-compiler/
[164] https://www.softwarepreservation.org/projects/interactive_c/bib/Fritzson-1983.pdf
[165] https://rustc-dev-guide.rust-lang.org/queries/incremental-compilation-in-detail.html
[136] https://clang.llvm.org/docs/StandardCPlusPlusModules.html
[137] https://clang.llvm.org/docs/Modules.html
[138] https://llvm.org/devmtg/2019-04/slides/TechTalk-Lorenz-clang-scan-deps_Fast_dependency_scanning_for_explicit_modules.pdf
[139] https://clang.llvm.org/doxygen/namespaceclang_1_1tooling_1_1dependencies.html
[140] https://clang.llvm.org/doxygen/namespaceclang_1_1dependencies.html
[141] https://github.com/llvm/llvm-project/blob/main/clang/tools/clang-scan-deps/ClangScanDeps.cpp
[142] https://clang.llvm.org/doxygen/classclang_1_1tooling_1_1DependencyScanningTool.html
[143] https://clang.llvm.org/doxygen/classclang_1_1tooling_1_1dependencies_1_1DependencyScanningTool.html
[144] https://www.ibm.com/docs/en/SSRZSMX_17.1.1/pdf/Clang_document.pdf
[145] https://discourse.llvm.org/t/clang-gsoc-2025-support-simple-c-20-modules-use-from-the-clang-driver-without-a-build-system/84511
[146] https://discourse.llvm.org/t/rfc-modules-support-simple-c-20-modules-use-from-the-clang-driver-without-a-build-system/86456
[147] https://clang.llvm.org/doxygen/DependencyScanningTool_8cpp_source.html
[148] https://llvm.org/devmtg/2023-10/slides/quicktalks/Blangmuir-clang-cache-modules.pdf
[149] https://github.com/llvm/llvm-project/pull/66462
[150] https://clang.llvm.org/doxygen/classclang_1_1ModuleMap.html
[166] https://github.com/include-what-you-use/include-what-you-use
[167] https://github.com/include-what-you-use/include-what-you-use/blob/master/README.md
[168] https://manpages.opensuse.org/Tumbleweed/include-what-you-use/include-what-you-use.1.en.html
[169] https://www.fluentcpp.com/2021/01/01/include-what-you-use/
[170] https://include-what-you-use.org/
[171] https://www.incredibuild.com/blog/include-what-you-use-how-to-best-utilize-this-tool-and-avoid-common-issues
[172] https://llvm.org/devmtg/2010-11/Silverstein-IncludeWhatYouUse.pdf
[173] https://discourse.llvm.org/t/clang-c-20-modules-round-table-meeting-notes/88744
[174] https://github.com/notdanhan/include-what-you-use/blob/master/README.md
[175] https://github.com/notdanhan/include-what-you-use
[176] https://github.com/kadircet/include-what-you-use
[177] https://clang.llvm.org/doxygen/ModuleDepCollector_8cpp_source.html
[178] https://reviews.llvm.org/D120465
[179] https://clang.llvm.org/extra/modularize.html
[180] https://discourse.llvm.org/t/modules-silent-textual-inclusion-when-files-not-found/36895
[181] https://www.erlang.org/doc/system/release_handling.html
[182] https://www.erlang.org/docs/17/design_principles/release_handling
[183] https://www.erlang.org/docs/29/system/release_handling
[184] http://www.erlang-factory.com/static/upload/media/1498593698072703luisrascaoletsmakereleaseupgradesgreatagain.pdf
[185] https://www.erlang.org/docs/21/design_principles/release_handling
[186] https://www.erlang.org/doc/apps/stdlib/gen_server.html
[187] https://www.erlang.org/docs/18/design_principles/release_handling
[188] https://www.erlang.org/docs/17/system_principles/upgrade
[189] https://www.metabrew.com/article/erlangotp-releases-rebar-release_handler-appup-etc
[190] https://www.erlang.org/docs/18/man/release_handler
[191] https://learnyousomeerlang.com/relups
[192] https://www.erlang.org/docs/21/system_principles/upgrade
[193] https://blog.appsignal.com/2018/10/16/elixir-alchemy-hot-code-reloading-in-elixir.html
[194] https://www.erlang.org/docs/20/man/gen_server.html
[195] https://stackoverflow.com/questions/1840717/achieving-code-swapping-in-erlangs-gen-server
[211] https://scg.unibe.ch/download/lectures/p2-2016/P2-12-Smalltalk.pdf
[212] https://www.instantiations.com/vast-support/documentation/922/sv/server19.html
[213] https://en.wikipedia.org/wiki/Smalltalk
[214] https://stackoverflow.com/questions/8874615/how-to-replace-a-running-function-in-common-lisp
[215] https://www.oscar.nierstrasz.org/files/slides/SmalltalkIntroSlideshow/
[216] https://squeak.org/documentation/
[217] https://www.gnu.org/software/smalltalk/manual/gst.html
[218] https://stackoverflow.com/questions/3074812/common-lisp-redefine-an-existing-function-within-a-scope
[219] https://w140.com/tekwiki/images/7/7c/070-5606-00.pdf
[220] https://stackoverflow.com/questions/56406426/how-do-i-use-the-live-code-feature-in-sbcl
[221] https://stackoverflow.com/questions/57697459/how-to-do-image-based-development-in-common-lisp
[222] https://www.reddit.com/r/Common_Lisp/comments/9q6bum/how_does_common_lisp_implement_hot_code_reloading/
[223] https://www.instantiations.com/vast-support/documentation/1000/sv/server19.html
[224] https://crystal.uta.edu/~cli/cse3302/fall2007/slides/cse3302-fall07-14.pdf
[225] https://scg.unibe.ch/download/lectures/sma/SMA-02-Smalltalk-Gt.pdf
[196] http://dcevm.github.io/
[197] https://github.com/dcevm/dcevm/blob/master/README.md
[198] https://www.jetbrains.com/help/fleet/hotswap-java.html
[199] https://openjdk.org/jeps/159
[200] https://www.jrebel.com/blog/java-hotswap-guide
[201] https://wiki.openjdk.org/display/mlvm/HotSwap
[202] https://www.jrebel.com/blog/how-to-reload-java-classes-at-runtime
[203] https://github.com/dcevm/dcevm
[204] https://people.cs.vt.edu/tilevich/papers/HotSWUp08.pdf
[205] https://docs.oracle.com/javase/8/docs/technotes/guides/jpda/enhancements1.4.html
[206] https://wiki.openjdk.org/download/attachments/7897093/wuerthinger-hotswap-20090401.pdf?version=1&modificationDate=1252783203000&api=v2
[207] https://wiki.openjdk.org/spaces/mlvm/pages/7897093/HotSwap
[208] https://manuals.jrebel.com/jrebel/ide/index.html
[209] https://people.cs.vt.edu/tilevich/papers/ccgrid09.pdf
[210] https://segmentfault.com/a/1190000011174467
[241] https://learn.microsoft.com/en-us/aspnet/core/test/hot-reload?view=aspnetcore-10.0
[242] https://github.com/dotnet/roslyn/blob/main/docs/wiki/EnC-Supported-Edits.md
[243] https://deepwiki.com/dotnet/roslyn/5.4-edit-and-continue
[244] https://devblogs.microsoft.com/dotnet/hot-reload-generics/
[245] https://blog.jetbrains.com/dotnet/2021/12/02/how-rider-hot-reload-works-under-the-hood/
[246] https://deepwiki.com/dotnet/roslyn/5.4.1-enc-analysis-and-delta-emission
[247] https://devblogs.microsoft.com/dotnet/introducing-net-hot-reload/
[248] https://learn.microsoft.com/ms-my/aspnet/core/test/hot-reload?view=aspnetcore-7.0&viewFallbackFrom=aspnetcore-5.0
[249] https://github.com/dotnet/docs/blob/main/docs/core/compatibility/sdk/9.0/dotnet-watch.md
[250] https://learn.microsoft.com/sr-latn-rs/aspnet/core/test/hot-reload?view=aspnetcore-7.0&viewFallbackFrom=aspnetcore-5.0
[251] https://learn.microsoft.com/en-sg/aspnet/core/test/hot-reload?view=aspnetcore-9.0
[252] https://blog.jetbrains.com/dotnet/2021/10/25/hot-reload-for-net-6-in-rider-2021-3/
[253] https://github.com/MicrosoftDocs/visualstudio-docs/blob/main/docs/debugger/hot-reload.md
[254] https://devblogs.microsoft.com/dotnet/update-on-net-hot-reload-progress-and-visual-studio-2022-highlights/
[255] https://learn.microsoft.com/zh-tw/dotnet/core/compatibility/sdk/9.0/dotnet-watch
[256] https://dioxuslabs.com/blog/release-070/
[257] https://docs.rs/hot-lib-reloader/latest/hot_lib_reloader/attr.hot_module.html
[258] https://docs.rs/hot-lib-reloader/latest/src/hot_lib_reloader/lib.rs.html
[259] https://docs.rs/crate/subsecond/0.7.0-alpha.1
[260] https://github.com/DioxusLabs/dioxus/discussions/4219
[261] https://docs.rs/subsecond/latest/subsecond/
[262] https://jmmut.github.io/2023/03/17/Hot-reloading-Rust-and-Macroquad.html
[263] https://github.com/jkelleyrtp/ipbp
[264] https://rygoldstein.com/posts/hot-reloading-rust
[265] https://github.com/DioxusLabs/dioxus/actions/runs/14486680551
[266] https://news.ycombinator.com/item?id=44369642
[267] https://www.reddit.com/r/rust/comments/1j8z3yb/media_dioxus_subsecond_rust_hotpatch_engine/
[268] https://docs.rs/hot-lib-reloader/latest/x86_64-pc-windows-msvc/src/hot_lib_reloader/lib.rs.html
[269] https://dioxuslabs.com/learn/0.7/essentials/fullstack/
[270] https://docs.rs/subsecond/0.7.10/subsecond/
[226] https://ruby0x1.github.io/machinery_blog_archive/post/dll-hot-reloading-in-theory-and-practice/index.html
[227] https://thenumb.at/Hot-Reloading-in-Exile/
[228] https://stackoverflow.com/questions/49280813/runtime-library-reloading-using-dlopen
[229] https://www.reddit.com/r/C_Programming/comments/1irziyf/very_simple_hot_code_reloading_example_in_c/
[230] https://gamedev.stackexchange.com/questions/221/how-can-one-implement-hot-swappable-c-modules
[231] https://codefade.com/posts/94386_hot_code_reloading_in_c/
[232] https://rouvik.github.io/blogs/pages/hot_code_reloading/hot_reload.html
[233] https://ayazhafiz.com/articles/21/hot-code-reloading
[234] https://github.com/ddovod/jet-live
[235] https://blog.joncodesthings.net/2019/02/23/hot-reloading-c-code-or-how-i-learned-to-stop-worrying-and-loath-the-linker/
[236] https://stackoverflow.com/questions/21887861/dynamically-load-a-class-from-a-library-and-use-it-after-closing-the-library
[237] https://sonictk.github.io/maya_hot_reload_example_public/getting_started/
[238] https://www.reddit.com/r/cpp_questions/comments/1os3b1x/hot_reloadable_c_library_feedback/
[239] https://www.reddit.com/r/cpp/comments/1vqwvrg/built_a_lightweight_zerodependency_c17_live_dll/
[240] https://github.com/seletz/raylib-hot-code-reload-c-example
[286] https://docs.kernel.org/livepatch/livepatch.html
[287] https://docs.kernel.org/6.2/livepatch/livepatch.html
[288] https://docs.kernel.org/6.15/livepatch/livepatch.html
[289] https://docs.kernel.org/6.1/livepatch/livepatch.html
[290] https://events.linuxfoundation.org/wp-content/uploads/2024/05/Linux-Livepatch_-Introduction-Mentorship-Webinar-5-22-24.pdf
[291] https://courses.cs.washington.edu/courses/cse451/18au/notes/adv-rebootless.pdf
[292] https://kernel-internals.org/livepatch/klp-consistency/
[293] https://ruffell.nz/programming/writeups/2020/04/20/everything-you-wanted-to-know-about-kernel-livepatch-in-ubuntu.html
[294] https://es.slideshare.net/slideshow/live-patching-technology/49395091
[295] https://docs.kernel.org/livepatch/api.html
[296] http://events17.linuxfoundation.org/sites/events/files/slides/LinuxConNA-kpatch-without-stopmachine_fixed.pdf
[297] https://links.imagerelay.com/cdn/3404/ql/2504696a706d4fe19f748d5326777157/TUT-1376-SD20_Kernel_live_patching_methodology.pdf
[298] https://doc.liz6.com/linux-kernel/11-kernel-development-and-build/03-Live-Patching
[299] https://docs.kernel.org/livepatch/shadow-vars.html
[300] https://kernel-internals.org/livepatch/klp/
[301] https://arxiv.org/html/2410.00026v2
[302] https://docs.kernel.org/userspace-api/ebpf/syscall.html
[303] https://docs.ebpf.io/ebpf-library/libbpf/concepts/
[304] https://docs.ebpf.io/linux/syscall/
[305] https://pkg.go.dev/github.com/1715173329/ebpf-go
[306] https://www.kernel.org/doc/html//v5.19/bpf/maps.html
[307] https://source.android.com/docs/core/architecture/kernel/bpf
[308] https://docs.ebpf.io/linux/
[309] https://man.freebsd.org/cgi/man.cgi?query=bpf&sektion=2&manpath=Debian+9.13.0
[310] https://www.kernel.org/doc/html/v5.18/bpf/maps.html
[311] https://docs.kernel.org/bpf/prog_lsm.html
[312] https://docs.ebpf.io/linux/map-type/
[313] https://pkg.go.dev/github.com/cilium/ebpf
[314] https://docs.ebpf.io/linux/concepts/maps/
[315] https://ebpf.io/what-is-ebpf/
[271] https://github.com/WebAssembly/component-model/blob/main/design/mvp/Linking.md
[272] https://github.com/webassembly/component-model
[273] https://wasmcloud.com/community/2025-03-19-community-meeting-transcript/
[274] https://github.com/WebAssembly/component-model/blob/main/design/mvp/examples/SharedEverythingDynamicLinking.md
[275] https://github.com/WebAssembly/component-model/blob/main/design/mvp/Explainer.md
[276] https://github.com/webassembly/shared-everything-threads
[277] https://forums.swift.org/t/pitch-a-vision-for-webassembly-support-in-swift/79060
[278] https://component-model.bytecodealliance.org/creating-and-consuming/composing.html
[279] https://hackmd.io/@NXXwAQZbQ8CTpJXGoJcJ8w/BJe5Id6Qn
[280] https://apis.io/apis/component-model/specification/
[281] https://github.com/WebAssembly/wasi-threads/
[282] https://github.com/WebAssembly/component-model/blob/main/design/high-level/UseCases.md
[283] https://github.com/WebAssembly/shared-everything-threads/blob/main/proposals/shared-everything-threads/Overview.md
[284] https://www.youngju.dev/transcribe/devops/2026-03-14-webassembly-component-model-wasm-first-class-web.en
[285] https://morphir.finos.org/docs/spec/webassembly/wasm-component-model/
[331] https://rustc-dev-guide.rust-lang.org/solve/sharing-crates-with-rust-analyzer.html
[332] https://rust-lang.github.io/rfcs/2912-rust-analyzer.html
[333] https://langdev.stackexchange.com/questions/915/how-would-you-design-a-compiler-upfront-to-be-both-a-compiler-and-a-lsp-server
[334] https://github.com/rust-lang/rust-analyzer
[335] https://deepwiki.com/dotnet/roslyn/3-compiler-architecture
[336] https://github.com/dotnet/roslyn/blob/main/docs/compilers/Compiler%20Server.md
[337] https://raw.githubusercontent.com/dotnet/roslyn/refs/heads/main/docs/features/incremental-generators.cookbook.md
[338] https://www.reddit.com/r/ProgrammingLanguages/comments/lz8skk/language_server_built_into_the_compiler/
[339] https://stackoverflow.com/questions/71607413/how-does-the-compiler-pipeline-correspond-to-incremental-source-generation
[340] https://ferrous-systems.com/blog/rust-analyzer-status-opencollective/
[341] https://news.ycombinator.com/item?id=42405357
[342] https://news.ycombinator.com/item?id=46719899
[343] https://github.com/dotnet/roslyn/blob/main/docs/features/source-generators.md
[344] https://news.ycombinator.com/item?id=22215520
[345] https://rust-analyzer.github.io/blog/2020/04/20/first-release.html
[346] https://www.reddit.com/r/rust/comments/12cukw3/why_doesnt_rustanalyzer_reuse_infrastructures_of/
[347] https://github.com/rust-lang/rfcs/blob/master/text/2912-rust-analyzer.md
[348] https://rustc-dev-guide.rust-lang.org/solve/sharing-crates-with-rust-analyzer.html
[349] https://microsoft.github.io/language-server-protocol/specifications/specification-3-16/
[350] https://en.wikipedia.org/wiki/Language_Server_Protocol
[351] https://microsoft.github.io/language-server-protocol/specifications/specification-3-15/
[352] https://microsoft.github.io/language-server-protocol/specifications/specification-3-14/
[353] https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/
[354] https://deepwiki.com/microsoft/language-server-protocol
[355] https://microsoft.github.io/language-server-protocol/specifications/lsp/3.18/specification/
[356] https://microsoft.github.io/language-server-protocol/
[357] https://github.com/BlankRain/language-server-protocol
[358] https://www.emergentmind.com/topics/language-server-protocol-lsp
[359] https://medium.com/@selfint/lsp-outside-the-editor-431f77a9a4be
[360] https://news.ycombinator.com/item?id=42405357
[316] https://doc.rust-lang.org/cargo/reference/profiles.html
[317] https://releases.rs/docs/1.52.1/
[318] https://blog.rust-lang.org/2021/05/10/Rust-1.52.1/
[319] https://rust-for-c-programmers.com/ch23/23_6_build_profiles.html
[320] https://users.rust-lang.org/t/cargo-book-and-incremental-compilation/125303
[321] https://github.com/rust-lang/compiler-team/issues/245
[322] https://rustwiki.org/blog/rust-1-52-1/
[323] https://github.com/rust-lang/cargo/pull/4817
[324] https://github.com/lineality/rust_compile_optimizations_cheatsheet
[325] https://users.rust-lang.org/t/enable-thin-local-lto-or-codegen-units-to-1-which-runtime-performs-better/106398
[326] https://internals.rust-lang.org/t/incremental-compilation-beta/4721?page=2
[327] https://rust-lang-rust-73.mintlify.app/compiler/incremental
[328] https://users.rust-lang.org/t/usable-stable-releases/73026
[329] https://doc.rust-lang.org/stable/cargo/reference/profiles.html?highlight=profile
[330] https://android.git.googlesource.com/toolchain/rustc/+/62a5f3ea9c7ba3a36c37c0084009905baeb23989/compiler/rustc_incremental/messages.ftl
[376] https://www.unison-lang.org/docs/tour/_scratch-files/
[377] https://www.unison-lang.org/docs/ucm-commands/
[378] https://www.unison-lang.org/docs/tour/
[379] https://www.unison-lang.org/docs/usage-topics/workflow-how-tos/update-code/
[380] https://www.unison-lang.org/docs/
[381] https://www.unison-lang.org/docs/usage-topics/resetting-codebase-state/
[382] https://exercism.org/docs/tracks/unison/tests
[383] https://zenn.dev/mizchi/articles/think-next-language-with-unison?locale=en
[384] https://github.com/unisonweb/unison
[385] https://github.com/unisonweb/unison/issues/3373
[386] https://dev.to/zelenya/unison-from-0-to-cloud-54g1
[387] https://dev.to/petets/palindromes-with-unison-5h9o
[388] https://www.unison-lang.org/docs/the-big-idea/
[389] https://dev.to/petets/unison-io-3k9
[390] https://dev.to/petets/make-your-blogs-run-1opb
[391] https://rustc-dev-guide.rust-lang.org/query.html
[392] https://doc.rust-lang.org/beta/nightly-rustc/rustc_middle/query/index.html
[393] https://rustc-dev-guide.rust-lang.org/queries/query-evaluation-model-in-detail.html
[394] https://deepwiki.com/rust-lang/rustc-dev-guide/4.7-the-query-system
[395] https://doc.rust-lang.org/stable/nightly-rustc/rustc_query_system/query/index.html
[396] https://ferrous-systems.com/blog/query-cycles-a-compiler-murder-mystery/
[397] https://doc.rust-lang.org/beta/nightly-rustc/src/rustc_query_system/dep_graph/graph.rs.html
[398] https://doc.rust-lang.org/beta/nightly-rustc/rustc_query_system/cache/struct.Cache.html
[399] https://github.com/rust-lang/rust/blob/master/compiler/rustc_query_system/src/dep_graph/graph.rs
[400] https://doc.rust-lang.org/stable/nightly-rustc/rustc_query_system/index.html
[401] https://doc.rust-lang.org/beta/nightly-rustc/rustc_query_system/dep_graph/dep_node/index.html
[402] https://github.com/rust-lang/rust/blob/master/compiler/rustc_middle/src/query/mod.rs
[403] https://rustc-dev-guide.rust-lang.org/solve/caching.html
[404] https://deepwiki.com/rust-lang/rustc-dev-guide/4.8-incremental-compilation
[405] https://github.com/rust-lang/rustc-dev-guide/blob/master/src/queries/incremental-compilation-in-detail.md
[361] https://rust-analyzer.github.io/blog/2023/07/24/durable-incrementality.html
[362] https://github.com/rust-lang/rust-analyzer/pull/20106
[363] https://github.com/rust-lang/rust-analyzer/issues/19042
[364] https://docs.rs/rust-analyzer-salsa/latest/salsa/trait.Query.html
[365] https://docs.rs/rust-analyzer-salsa/latest/src/salsa/lib.rs.html
[366] https://docs.rs/rust-analyzer-salsa
[367] https://lobehub.com/skills/joshuadavidthomas-agentkit-salsa-query-pipeline
[368] https://simmypeet.github.io/qbice/introduction.html
[369] https://docs.rs/rust-analyzer-salsa/latest/salsa/
[370] https://lib.rs/crates/rust-analyzer-salsa
[371] https://github.com/rust-analyzer/rust-analyzer/issues/6475
[372] https://deepwiki.com/rust-lang/rust-analyzer/2.2-salsa-and-incremental-computation
[373] https://hackmd.io/@salsa/B19OUlA71l
[374] https://news.ycombinator.com/item?id=42408598
[375] https://users.rust-lang.org/t/rust-analyzer-not-working-behind-firewall/105745
[406] https://kotlinlang.org/docs/ksp-incremental.html
[407] https://slack-chats.kotlinlang.org/t/32756930/it-looks-like-ic-is-working-differently-depending-on-the-kgp
[408] https://www.fengguang.me/kotlin-incremental-build-strategy
[409] https://github.com/JetBrains/kotlin/blob/master/compiler/incremental-compilation-impl/src/org/jetbrains/kotlin/incremental/IncrementalJvmCompilerRunner.kt
[410] https://deepwiki.com/JetBrains/kotlin/12.2-incremental-compilation
[411] https://kotlinlang.org/docs/gradle-compilation-and-caches.html
[412] https://slack-chats.kotlinlang.org/t/518342/i-m-having-an-issue-with-incremental-compilation-i-ve-read-t
[413] https://kotlinlang.org/docs/gradle.html
[414] https://discuss.kotlinlang.org/t/how-to-ensure-that-incremental-compilation-works-for-me/1790
[415] https://pigweed.googlesource.com/third_party/github/JetBrains/kotlin/+/ee3119e9d2423595a931301b4fc601eaa7d73937%5E!/
[416] https://kt.academy/article/ak-ksp
[417] https://www.fengguang.me/kotlin-incremental-build-caching-design-doc
[418] https://blog.jetbrains.com/kotlin/2022/07/a-new-approach-to-incremental-compilation-in-kotlin/
[419] https://kotlinlang.org/docs/ksp-faq.html
[420] https://kotlinlang.org/docs/js-ir-compiler.html
