# liquify — the IDE

> Research: [24-editors-and-ide-architecture](../research/24-editors-and-ide-architecture/) ·
> [16-monorepo-scale](../research/16-monorepo-scale-and-authoring-loop/) ·
> [08-incremental-compilation](../research/08-incremental-compilation/)
>
> **liquid** is the tool. **lake** is where it collects. **liquify** is where you work.

## The one architectural argument

Every modern editor talks to a language server. A language server is an incremental
compiler. The build system is *also* an incremental compiler. They maintain two
separate universes of parse trees, symbol tables and caches over the same source, and
neither can see the other's work.

The research identifies this as **the largest piece of duplicated effort in modern
toolchains**.

liquify does not run a language server:

```
liquify  ─────────────────────────┐
                                  ├──→  one warm incremental query graph
liquid build ─────────────────────┘         (types, diagnostics, symbols,
                                             objects, packages, images)
```

Completions, diagnostics, go-to-definition and hover are **queries against state the
build already populated** — and the build benefits from state the editor populated.
There is no JSON-RPC hop, no document synchronisation protocol, no second index, and
no "waiting for the language server to start."

**Stated precisely, because the loose version invites an easy rebuttal.** What liquify
removes is the *serialization boundary, the duplicate text model and the second index*.
What it does **not** remove is the need for interactive-specific analysis, because batch
compilation and interactive editing are genuinely different workloads:

| Batch compilation | Interactive editing |
|---|---|
| Starts from a build entry point | Must answer cursor-centric queries |
| Can reject malformed input | Must analyse incomplete, invalid input |
| Can discard syntax after lowering | Needs source ranges, trivia, recoverable trees |
| May rebuild whole units | Must update incrementally per edit |
| Optimises final compilation | Optimises latency and partial answers |

rust-analyzer separates compiler, HIR and IDE layers *inside one program* — because
those layers do different jobs, not because a protocol forced them apart. liquid's graph
must serve both, and that is work
([08](../research/08-incremental-compilation/), [04](04-build-engine.md)).

It is still the reason to build an editor rather than only ship an extension: **no
editor talking to liquid over LSP can have this property.** It is just a narrower claim
than "the language server disappears."

### Stub trees

Borrowed from the IntelliJ Platform, which is the closest existing precedent for a
shared semantic substrate rather than a protocol. A **stub** is a compact serialized
subset of a parse tree — declarations, names, modifiers, signatures — sufficient to
resolve symbols from other files without fully parsing them.

This composes unusually well with liquid: a stub is derivable per blob and therefore
content-addressable and cacheable like any other action output
([05](05-caching.md)). Cross-file resolution reads stubs; only open files need full
trees.

## Drips

**Every write to disk becomes a versioned change. That change is a drip.**

There is no capture step. Saving a file *is* recording history. Drip, drip, drip.

| Unit | What it is | Analogous to |
|---|---|---|
| **Drip** | One captured write. The atom of history. Automatic, continuous, never explicit. | Nothing in git — this granularity does not exist |
| **Change** | A named, reviewable aggregation of drips, with a message and an identity. | A commit or a PR |
| **Branch** | A line of changes. | A branch |

Drips flow into the CRDT datastore ([17](17-implementation.md)) and out again:

- **To the build** — each drip invalidates the minimum part of the query graph
  ([04](04-build-engine.md)). This is what makes drip-by-drip compilation literal
  rather than metaphorical.
- **To history** — every state the file has ever been in is recoverable, at save
  granularity. `liquid undo` is not "undo the commit"; it is "undo anything."
- **To collaborators** — a drip is a CRDT change, so it merges and syncs by the same
  mechanism whether it came from your keyboard or someone else's
  ([15](15-consumer-sync-and-web.md)).
- **To review** — a change is assembled from drips, so the reviewer can see how the
  author actually got there, not just the tidied result.

### What this costs, honestly

Capturing every write raises four real problems, and the design has to answer them:

1. **Volume.** Editors, formatters and build tools write constantly. Only *source*
   is dripped; build outputs, caches and dependency directories are excluded by
   declared rules. Getting this wrong versions `node_modules` forever.
2. **Metadata growth.** Chunk dedup makes the *bytes* nearly free; the CRDT operation
   history is the cost. This is the same open risk flagged in
   [17](17-implementation.md), and drips make it the central one.
3. **Noise.** A history of every keystroke-save is not a reviewable artifact. Hence
   the two-level model: drips are the record, **changes are the narrative.** Drips
   compact once the change containing them is named.
4. **Secrets typed and deleted are still in history.** A credential pasted into a file
   and removed thirty seconds later was still written to disk, and therefore still
   dripped. Secret scanning must run **on the drip**, not on the commit, and the tool
   must offer real redaction. This is a genuine new hazard that git's explicit-commit
   model partly hides, and it must not be discovered by a user in production.

## What liquify does that others cannot

**Open anything instantly.** The virtual filesystem
([01](01-object-model-and-storage.md)) means opening a 200 GB monorepo is a metadata
operation. There is no clone, and disk grows with what you actually read.

**Structural editing.** The AST already exists — liquid parses for incremental
compilation, diff and search. Refactors, structural selection and structural diff
([03](03-diff-and-merge.md)) come from the tree the build is already using.

**Multiplayer without a second system.** The datastore is a CRDT, so convergent
concurrent editing is a property of the storage layer rather than a separate
synchronisation service bolted alongside it. That is a real head start — but see
[Multiplayer is not free](#multiplayer-is-not-free) below, and
[24](24-multiplayer-and-agents.md) for what it takes to make it correct.

**Review in place.** Changes from lake ([18](18-lake-service.md)) are reviewed in the
editor, against real semantics rather than a text diff — and comments anchor to
cursors that survive rebases.

**Build, test, deploy inline.** Same graph. A test result is a query; a deployment
([13](13-deployment-runtime.md)) is an action.

**Search that is the real search.** oramacore and the trigram index
([12](12-search.md)), not a per-editor index that disagrees with CI.

## The risk that should govern the design

**Xi attempted almost exactly this, and it is discontinued.**

Xi combined Rust, a rope buffer, multiprocess frontend/core separation, asynchronous
plugins, JSON-RPC and CRDT concurrent editing. Its author's retrospective is candid:
the design accumulated too many *"novelty points"*. What broke was not any one
component but their interaction — asynchronous ordering races between edits, plugin
edits, selection changes and layout; word-wrap racing with editing across the process
boundary; formatters computing edits against documents the user had already changed.

> *Process separation is most successful when the boundaries are stable and the protocol
> is narrow. Xi attempted to make editing, plugins, layout, and collaboration
> asynchronous at the same time, creating a distributed-systems problem inside the text
> editor.*

liquid's stack is C++26, a CRDT datastore, a persistent daemon, a virtual filesystem,
async throughout, incremental compilation, multiplayer, and a plugin ABI. **That is Xi's
novelty-point list with more items on it.**

VS Code proves multiprocess editors work, so this is not a refutation. It is a
constraint on how the boundary is drawn:

- The editor↔graph interface is **narrow, stable, and synchronous wherever it can be.**
- Editing, layout and collaboration are **not** all made asynchronous at once.
- The editor is not where every novel subsystem first meets. Each is proven elsewhere —
  the graph by the build, the VFS by checkouts, the CRDT by sync — before liquify
  depends on all of them together.

This is the strongest argument for the staged sequencing below, and it is a warning
rather than a footnote.

## Multiplayer is not free

The datastore gives convergence. It does not give the rest, and the feature doc should
not pretend otherwise:

- **Interleaving.** Concurrent insertion of `hello` and `world` can converge to
  `hweolrllod`. Conflict-free means replicas *converge*, not that the result matches
  intent. Automerge uses RGA for text, which is the family with this anomaly.
- **Cursors are not offsets.** Positions must anchor to stable character identities;
  selections have two moving endpoints, direction and affinity.
- **Undo must be selective** — reversing the user's own intent, not the most recent
  remote operation. Treated in the literature as its own design problem.
- **Tombstones accumulate**, and collecting them requires knowing every replica and
  offline client has advanced past the deleted content.

## The honest problem: this is enormous

Rough estimates: VS Code core **300–800 person-years**; with remote, web, marketplace
and first-party extensions **1,000–3,000**; the IntelliJ Platform core **1,000–3,000**,
plus several thousand more for its language layer. The extension ecosystem — one 2024
study crawled **52,880** third-party extensions — not the editor, is the actual moat. A
new editor with no extensions is a text box.

The moat is also partly legal: Microsoft's Marketplace terms restrict use to its own
Visual Studio family, which is why Cursor, Windsurf and Trae use Open VSX or their own
galleries. **An extension sidesteps this entirely; a fork inherits it.**

**Pick one conspicuous advantage.** Zed won attention with latency and multiplayer, not
feature parity. liquify's candidate is *instant open of any repository at any size, with
real semantics and no indexing wait* — which follows from the virtual filesystem and the
warm graph, and which no other editor can offer.

**So the sequencing is deliberately unheroic:**

| Stage | Deliverable | Why |
|---|---|---|
| **1** | A **VS Code extension and an LSP server** backed by liquid's graph | Meets people where they are. Loses the no-LSP advantage, keeps everything else — instant open, real search, unified build. Ships early. |
| **2** | JetBrains plugin | Same, for the other half of the market. |
| **3** | **liquify** native | Built only once the graph, the VFS and the daemon are proven by stages 1–2. |

Building the editor first would be the classic mistake. The editor is the *expression*
of the architecture; the architecture has to exist first.

## Design position

- **Native, not Electron.** liquid is C++26 with a daemon already in-process; the
  editor should share memory with the graph rather than serialise across a process
  boundary. That is the whole point.
- **GPU-accelerated rendering**, following Zed's demonstration that it is achievable
  and worth it.
- **Modal editing available, not mandatory.**
- **A plugin ABI, not a scripting monoculture** — the same stable non-C++ boundary as
  format codecs and language frontends ([17](17-implementation.md)).
- **AI as a first-class but optional surface.** liquid knows the real dependency graph,
  the real symbol table and the real build errors, which is dramatically better context
  than an editor grepping for relevant files. And per the forge research
  ([22](../research/22-forges-and-code-hosting/)), the ability to *turn it off*
  entirely is itself a differentiator in 2026.

## Open questions

- **Whether liquify ships at all**, versus liquid remaining excellent inside VS Code
  and JetBrains forever. Stage 1 must be genuinely good regardless, and it may prove
  sufficient.
- **Drip granularity.** Per save is the obvious default. Per keystroke is possible —
  the datastore supports it and the build engine is designed for it — but the metadata
  cost is unproven and the value over per-save is unclear.
- **Drip compaction policy.** When a change is named, how much of the underlying drip
  history is retained, and for how long?
- **Extension compatibility.** Whether to attempt VS Code extension API compatibility
  (large, thankless, effective) or a clean native API (small, elegant, empty).
- **Marketplace licensing.** Microsoft restricts the VS Code marketplace to VS Code;
  forks have had to build their own registries. Relevant if stage 1 becomes a fork
  rather than an extension.
