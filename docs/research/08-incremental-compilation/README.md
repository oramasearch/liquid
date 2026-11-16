# Incremental compilation

> Evidence: [`sources.md`](sources.md)
>
> This is the "drip by drip" requirement: compile the minimum delta *as* the code
> is being written, not after.

## The architecture: compilation as a memoised query graph

The strongest modern design treats a compiler not as a pipeline of passes but as a
graph of memoised queries. You do not "run the type checker"; you *ask* for the
type of a definition, and the system computes it, records what it read, and caches
the answer.

**rustc's query system** is the reference implementation at scale. Its mechanism:

- Every query execution records the queries it depended on, forming a **dep-graph**.
- Each query result gets a **fingerprint** (a stable hash of the result).
- The **red-green algorithm** validates lazily on the next compile: mark changed
  inputs red; for any node, try to mark it green by checking all its dependencies —
  if every dependency ends up green, the node is reused without re-execution.
- Critically, when a dependency *does* re-execute but produces the **same
  fingerprint**, the dependent can still be marked green. This is **early cutoff**
  at the query level, and it is why a comment change costs nothing.

**Salsa** is the same idea as a reusable Rust framework, and it powers
rust-analyzer. Two refinements matter:

- **Durability.** Inputs are tagged by how often they change — the standard library
  is `HIGH` durability, the file being typed is `LOW`. A change to a low-durability
  input cannot invalidate anything that only depends on high-durability inputs, so
  most of the graph is skipped without even being visited.
- **Firewall queries.** A deliberately coarse query placed between a volatile input
  and expensive consumers, so churn is absorbed at a known boundary.

The theory behind all of this is **self-adjusting computation** (Acar) and
**Adapton**: a general result that any pure computation can be made incremental by
recording a dynamic dependence graph and repairing it after input changes.

Other instances of the same idea: Roslyn's incremental compiler and generators;
Swift's dependency files; TypeScript's project references and `.tsbuildinfo`;
Kotlin/Gradle incremental compilation.

**The recurring caveat:** rustc's incremental compilation has repeatedly shipped
correctness bugs and has been disabled by default at times. Incrementality is
*hard to get right*, and the failure mode — a stale artifact that compiles but is
wrong — is worse than a slow build. Any implementation needs a way to
cross-validate incremental results against a clean build.

## Unison: content-addressed code

Unison takes the idea to its conclusion. Every definition is stored under the hash
of its **dependency-closed AST**. Names are metadata that point at hashes.

The consequences are remarkable:

- **There is no build.** A definition identified by hash has already been
  typechecked and compiled; the result is cached by that hash forever.
- **Renames are free** and can never conflict — a rename edits metadata, not code.
- **No dependency conflicts.** Two versions of a library are two sets of hashes
  that coexist without diamond problems.
- **Perfect structural sharing** between versions.
- **Distributed execution is natural** — you can send a computation to another
  machine by sending hashes.

The limitations are equally real: it requires its own language and semantics; it
stores a codebase rather than files, so the entire external tooling ecosystem
(editors, grep, diff, CI) does not apply; and refactoring that changes a widely-used
definition still propagates new hashes through everything that depends on it. It
also does not solve the *native* compilation problem — separate compilation,
linking, ABI — which is where liquid's hardest cases live.

But the core insight is directly transferable: **the unit of caching should be a
definition, not a file.**

## Granularity: why file-level is not enough

File-level dependency tracking is the standard, and it is too coarse. If a header
declares fifty functions and you change one, every translation unit including that
header rebuilds — even those using none of the changed declarations.

What is needed is **symbol-level or definition-level dependency tracking**: record
that TU `foo.cpp` depends on the declaration of `bar()` specifically, so a change
to `baz()` in the same header does not invalidate it.

For C and C++ the machinery exists but is awkward:

- **`clang-scan-deps`** and the **P1689** format report a TU's module and header
  dependencies before compiling — this is what makes C++20 modules buildable.
- **C++ modules** genuinely fix header fan-out by making the interface explicit and
  compiled once, but tooling maturity in 2026 is still the gating factor
  ([18](../18-implementation-cpp26/)).
- Absent modules, the practical approximation is content-hashing the *preprocessed
  declarations a TU actually used*, which is roughly what ccache's direct mode
  approximates at file granularity
  ([06](../06-build-and-compiler-caching/)).

## Hot code reloading and the runtime ambition

liquid eventually wants incremental builds to update a running system without a
full restart. The prior art divides sharply by how much the runtime was designed
for it.

| Approach | Mechanism | Honest assessment |
|---|---|---|
| **Erlang/OTP** | Two code versions per module coexist; `code_change/3` migrates state; release upgrades scripted via appups/relups | **The gold standard.** Works because the language was designed for it: isolated processes, immutable state, explicit supervised state migration. |
| **Smalltalk / Lisp images** | The running image *is* the program | Excellent interactivity; the image becomes the artifact, which conflicts with reproducibility. |
| **JVM HotSwap / DCEVM / JRebel** | Redefine method bodies | Stock HotSwap cannot add/remove methods or fields. DCEVM and JRebel extend this considerably. |
| **.NET Hot Reload** | Similar, with similar limits | Good for development, not production. |
| **Kernel live patching** (kpatch, livepatch, Ksplice) | Redirect function entry points; wait for a safe quiescent state | Proves this works for native code in the most demanding environment. Constrained: cannot change data structure layouts. |
| **Wasm component model** | Dynamic linking of components | Promising; interface types make the boundary explicit. |
| **Rust/C++ hot reload** (subsecond, hot-lib-reloader) | Reload a dynamic library | Works for isolated pure logic; falls apart on state that outlives the reload. |

**The fundamental obstacle is state migration, not code loading.** Swapping a
function is comparatively easy. Deciding what happens to live data whose type just
changed is the actual problem, and it cannot be solved generically — it requires
either immutable/isolated state (Erlang), or a migration function the developer
writes, or a restriction to changes that do not alter layout (live patching).

## The LSP duplication problem

Language servers are incremental compilers, and they are a *second* incremental
compiler running alongside the batch build. rust-analyzer, clangd, gopls and
tsserver each maintain their own in-memory VFS with unsaved-buffer overlays, their
own parse trees, and their own caches — recomputing on keystroke with
cancellation — and none of that work is shared with `cargo build`.

This is pure waste, and it is the strongest argument for liquid's design: **if the
build system and the editor's analysis engine are the same incremental graph, the
work is done once.** The "compile as you type" ambition is not an extra feature
layered on top; it is what you get for free when you unify them.

## Implications for liquid

1. **The build graph is a memoised query graph, not a task DAG.** Adopt the
   Salsa/rustc model as the core engine: demand-driven queries, recorded
   dependencies, result fingerprints, red-green validation, and early cutoff at
   every node. This is the same mechanism as the action graph in
   [04-build-systems](../04-build-systems/) — actions are just coarse queries — so
   there is one engine, not two.

2. **Early cutoff by fingerprint at every level.** A change that does not alter a
   result's fingerprint stops there. This is the mechanism that makes
   "compile the minimum delta" true rather than aspirational, and it is the same
   property that kills "build the world"
   ([09](../09-package-managers/)).

3. **Durability tiers.** A developer's edits are low durability; their project's
   dependencies are medium; the toolchain and stdlib are high. Most of the graph
   should never be visited on a keystroke. Without this, per-keystroke rebuilds do
   not stay sub-second on a large codebase.

4. **Definition-level granularity where the language permits, TU-level as the
   floor.** Take Unison's insight — cache by the hash of a dependency-closed
   definition — and apply it to languages liquid does not own, using tree-sitter or
   compiler frontends to extract the units. Fall back to per-TU for C/C++ until
   modules make finer granularity practical.

5. **One engine for the editor and the build.** No separate language server
   maintaining a parallel universe. The editor asks the same graph for types and
   diagnostics that the build asks for object files, with an in-memory overlay for
   unsaved buffers and cancellation on keystroke. This is a genuine architectural
   advantage over every current toolchain, and it is the direct route to the
   "drip by drip" goal.

6. **Incremental state must be content-addressed and portable.** rustc's and
   Cargo's incremental directories are machine-local mutable state, which is
   exactly why they conflict with sccache
   ([06](../06-build-and-compiler-caching/)). liquid's incremental state must
   *be* the shared cache, so "incremental" and "distributed" are the same
   mechanism rather than competing ones.

7. **Validate incrementality continuously.** rustc's history of incremental
   correctness bugs is a warning. Periodically rebuild sampled targets from clean
   and compare against the incremental result; a mismatch is a critical bug, and it
   should be detected by the tool rather than by a confused user. This is the same
   sampling infrastructure as remote-execution divergence detection
   ([07](../07-distributed-execution/)).

8. **Defer the runtime hot-update ambition, but do not design it out.** State
   migration is the hard part and it is not solvable generically. Keep the artifact
   model capable of expressing "this new version replaces that one at these
   symbols" — see [17-runtime-hot-update](../17-runtime-hot-update/) — and ship it
   after the build story is solid.
