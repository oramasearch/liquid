# Editors and IDE architecture

> Evidence: [`sources.md`](sources.md)
>
> Context for **[liquify](../../features/21-liquify-ide.md)**.

## The finding that should change the plan

**Xi failed, and it failed by attempting exactly what liquify proposes.**

Xi combined Rust, a rope buffer, multiprocess frontend/core separation, asynchronous
plugins, JSON-RPC and CRDT-based concurrent edits. It is discontinued. Its author's
retrospective is unusually candid: the design accumulated too many **"novelty points"**
and became far more complex than necessary.

The specific failures:

- **Asynchronous ordering races** — user edits, plugin edits, selection changes, syntax
  updates and layout updates arriving in different orders.
- **Word-wrap and viewport tearing** — rewrap racing with editing across a process
  boundary.
- **Plugin edit reconciliation** — an async formatter computing an edit against a
  document the user has since changed.
- **CRDT complexity without a sufficiently valuable product payoff.**

The conclusion:

> **Process separation is most successful when the boundaries are stable and the
> protocol is narrow. Xi attempted to make editing, plugins, layout, and collaboration
> asynchronous at the same time, creating a distributed-systems problem inside the text
> editor.**

Now compare liquid's stack: C++26, a CRDT datastore, a persistent daemon, a virtual
filesystem, async everywhere, incremental compilation, multiplayer collaboration, and a
plugin ABI. **That is the Xi novelty-point stack with more items on it.**

This does not mean the architecture is wrong — VS Code proves multiprocess editors work.
It means the *boundary* has to be narrow and stable, and the editor must not be where
all the novelty converges at once. It is the strongest available argument for the
staged sequencing already in the feature doc, and it should be treated as a warning
rather than a footnote.

## A correction to liquid's own claim

liquid's documents assert that a language server duplicates the batch compiler's work.
The research sharpens this, and the sharper version is more defensible:

> LSP does not necessarily duplicate the compiler's *implementation*, but it frequently
> duplicates the compiler's **analysis pipeline and state**, because interactive editing
> is a different workload from batch compilation.

The two workloads genuinely differ:

| Batch compiler | Interactive language server |
|---|---|
| Starts from a build entry point | Must answer cursor-centric requests |
| Can reject malformed input | Must analyze incomplete, invalid input |
| Can discard syntax details after lowering | Needs source ranges, trivia, recoverable trees |
| May rebuild whole units | Must update incrementally after edits |
| Optimizes final compilation | Optimizes latency and partial answers |

So "one graph serves both" is not free. A build-oriented graph does not automatically
answer cursor-centric queries over invalid input. rust-analyzer internally separates
compiler, HIR and IDE layers *even though it is one program* — because those layers do
different jobs, not because a protocol forced them apart.

**liquid's advantage is real but narrower than stated**: it removes the *serialization
boundary, the second text model and the duplicated index*, not the need for
interactive-specific analysis. That is still worth a great deal. It is not "the language
server disappears."

## VS Code: the moat is not the editor

The desktop product is Electron main process, Chromium renderer/workbench, shared
services, and a separate **extension host** — moved into an Electron utility process for
isolation. Worth noting: this is isolation for *stability*, **not a security sandbox**.
The extension host has substantial access to the user's files, and extensions should be
treated like installed programs.

Extension hosts come in three flavours — local Node, browser Web Worker
(`vscode.dev`), and remote Node on SSH/container/Codespace — which is what makes VS
Code's remote development story work: the workbench stays local while the server,
language servers, compilers and SDKs sit next to the source.

**The moat is the ecosystem.** The editor shell is reproducible — Electron, Monaco,
tree-sitter, LSP, Git integration are all available. What is not reproducible is
thousands of extensions plus ratings, install counts, update infrastructure and
publisher trust. One 2024 study crawled **52,880 third-party extensions**.

And the moat is partly *legal*: Microsoft's Marketplace terms restrict use to
Microsoft's Visual Studio family, which has been read as barring non-Microsoft forks.
Cursor, Windsurf and Trae must use Open VSX or their own gallery, and may not get
Microsoft-published extensions. **Source-code compatibility is not ecosystem rights.**

## JetBrains: why they never adopted LSP

The IntelliJ Platform's advantage is **PSI** — a semantic object model, not a syntax
tree — integrated with indexes, caches, reference resolution and refactoring. **Stub
trees** are the scaling mechanism: a compact serialized subset of a PSI tree containing
declarations and signatures, loadable from disk without fully parsing the file.

JetBrains' documentation states plainly that canonical IntelliJ language support gives
wider IDE integration than merely handling data from a language server, and positions
LSP as *additional value rather than a replacement*.

The reason is architectural: adopting LSP for Java or Kotlin would put a protocol
boundary between the language engine and features designed to query PSI, indexes and
project models directly.

**This is the closest existing precedent for what liquify wants** — a shared semantic
substrate rather than a protocol — and it is worth noting that JetBrains needed two
decades and, by rough estimate, 1,000–3,000 person-years for the platform alone.

## Zed: the traction model to copy

Rust, GPUI (hybrid immediate/retained GPU-accelerated UI), open-sourced January 2024,
CRDT multiplayer with presence.

Its strategy was explicitly **not** to reproduce VS Code's marketplace: make latency
visibly better, ship multiplayer without external tools, integrate AI natively,
open-source it, and keep the product surface coherent.

> A new editor can gain traction without matching VS Code feature-for-feature if it
> offers a conspicuous advantage in a **high-frequency activity** — typing latency,
> startup, collaboration, terminal workflow, or AI — and keeps its initial product
> surface coherent.

Helix did the same from the terminal: tree-sitter and LSP built in, coherent defaults,
no plugin assembly required.

## CRDT collaboration: what it does not solve

liquid gets multiplayer "for free" from its datastore. The research is clear that this
phrasing is too generous:

- **Interleaving.** Concurrent insertion of `hello` and `world` can converge to
  `hweolrllod`. *"Conflict-free" means replicas converge; it does not mean the merged
  text matches human intent.* Fugue-style algorithms (Loro) explicitly target this;
  RGA — which Automerge uses for text — is the family with the problem.
- **Cursors are not offsets.** Once concurrent edits exist, offset 100 changes meaning.
  Positions must anchor to stable character identities. Selections are worse: two moving
  endpoints, direction, affinity.
- **Undo must be selective.** Local undo should reverse *the user's own intent*, not the
  most recent remote operation. This is treated in the literature as a separate design
  problem, not a trivial inverse.
- **Tombstones accumulate**, and GC requires knowing every replica, backup and offline
  client has advanced past the deleted content.
- **Large documents need the right structure.** Naive linked-list CRDTs make positional
  operations expensive; Diamond Types uses a range tree with character counts for
  logarithmic positional access.

## The economics

Rough estimates, explicitly not reported metrics: VS Code core **300–800 person-years**;
with first-party remote, web, marketplace and major extensions **1,000–3,000**;
IntelliJ Platform core **1,000–3,000**, plus several thousand more for the language and
product layer.

40 engineers over 10 years is 400 person-years before QA, design, docs, security and
support.

## Implications for liquid

1. **Take the Xi warning seriously and narrow the boundary.** The editor must not be
   where CRDT, async, multiprocess, layout and plugin concerns all meet at once. Keep
   the editor↔graph interface **narrow, synchronous where it can be, and stable** — and
   do not make editing, layout and collaboration simultaneously asynchronous. This is
   the single most actionable finding in this document.

2. **Restate the LSP claim accurately.** liquid removes the serialization boundary, the
   duplicate text model and the second index — not the need for interactive-specific
   analysis over invalid input. Overstating this invites an easy rebuttal from anyone
   who has built a language server.

3. **Build the semantic substrate, not a protocol — but budget for it.** PSI is the
   precedent for what liquid's query graph must become, and it took JetBrains two
   decades. liquid's graph must serve cursor-centric queries over incomplete code, not
   just build artifacts ([08-incremental-compilation](../08-incremental-compilation/)).

4. **Adopt stub trees.** JetBrains' compact serialized declaration subset is exactly
   what liquid needs to answer cross-file queries without materialising full trees —
   and it composes naturally with content addressing, since a stub is derivable and
   cacheable per blob.

5. **Ship into VS Code first, and expect the marketplace fight if it becomes a fork.**
   An extension avoids the licensing problem entirely; a fork inherits it. This is a
   product-strategy fact, not a technical one.

6. **Pick one conspicuous advantage.** Zed chose latency and multiplayer. liquify's
   candidate is *instant open of any repository at any size, with real semantics and no
   indexing wait* — which follows from the virtual filesystem and the warm graph, and
   which no other editor can offer.

7. **Do not claim multiplayer is free.** Interleaving, cursor anchoring, selective undo
   and tombstone GC are real work on top of the CRDT. Automerge uses RGA for text, which
   is the family with known interleaving anomalies
   ([17-implementation](../../features/17-implementation.md) already flags this).

8. **tree-sitter is the syntax substrate, not the semantic one.** It gives fast
   incremental parsing with error recovery — which is exactly what interactive editing
   over invalid code needs — but not types, macro expansion or build configuration.
