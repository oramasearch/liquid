<!-- Raw research source. Generated via Perplexity `perplexity_reason` (web-grounded)
     on 2026-09-01 for the liquid project. Lightly reformatted only. Note: gathered
     with the `reason` tier after the deeper `research` tier timed out; still
     web-grounded and cited.
     Treat as evidence, not as liquid design decisions - see README.md in this
     directory for the curated analysis and the implications drawn for liquid. -->

# Code editor and IDE architecture, 2024-2026

## Executive summary

The modern editor market has separated into three architectural strategies:

1. **VS Code:** a general-purpose, web-based workbench whose primary innovation is not the text buffer but the **extension boundary, remote execution model, and distribution ecosystem**.
2. **JetBrains:** a deeply integrated IDE platform whose competitive advantage is a shared semantic model - **PSI, indexes, stubs, inspections, refactorings, and project-wide analysis** - rather than protocol compatibility.
3. **Native editors:** smaller, more opinionated systems - Zed, Helix, Lapce, Neovim - that optimize for latency, keyboard-driven workflows, native rendering, or composability. They generally reuse LSP and tree-sitter instead of rebuilding complete language-specific IDE engines.

The key architectural tradeoff:

> **LSP maximizes interoperability by reducing editor/language integration to a common protocol; PSI-style IDEs maximize semantic depth by keeping the language model inside the IDE platform.**

Neither approach eliminates the compiler. LSP usually causes a language server to reuse some compiler components, but it also requires an interactive, incremental, error-tolerant analysis service and a protocol-facing presentation layer. That is why language servers commonly duplicate - or partially fork - the work of batch compilers.

---

## 1. VS Code

### 1.1 Electron and the workbench

The desktop product is an Electron application:

- **Electron main process:** application lifecycle, windows, native integration, IPC, update and process management.
- **Chromium renderer/workbench:** the UI, editor widgets, menus, panels, command palette, layout.
- **Shared services/processes:** file watching, search, extension management, other cross-window services.
- **Extension host:** a separate Node.js process that executes extension code.

VS Code's architecture is therefore not "a JavaScript editor in one process." It is a distributed set of cooperating processes. Microsoft moved the extension host away from the renderer into an Electron utility process to improve process isolation and sandboxing boundaries. [web:57][web:51]

Microsoft's stated reason for a separate extension host is to prevent a slow or malfunctioning extension from affecting startup and UI stability. Extensions also receive a Node.js runtime and can reuse the Node ecosystem. [web:46][web:50]

**This is isolation for stability, not a strong security sandbox.** The desktop extension host has substantial access to the user's files and machine; third-party extensions should be treated more like installed programs than browser plug-ins. [web:49]

### 1.2 Why extensions run out of process

| Problem | In-process plug-in model | VS Code extension-host model |
|---|---|---|
| UI responsiveness | Extension work can block the UI thread | Work is moved to another process |
| Crash containment | Plug-in crash can take down editor | Extension host can restart independently |
| API stability | Plug-ins depend on internal UI objects | Extensions use a versioned VS Code API |
| Node/native integration | Difficult to mix with browser renderer | Node.js and child processes are natural |

The cost is RPC overhead and a deliberately restricted API. Extensions cannot manipulate the workbench DOM; they contribute commands, views, decorations, language features, file-system providers, debuggers and tasks through the extension API.

There can be multiple extension hosts:

| Host | Runtime | Typical location |
|---|---|---|
| Local | Node.js | User's workstation |
| Web | Browser/Web Worker | `vscode.dev` or another browser session |
| Remote | Node.js | SSH host, container, WSL, Codespace, tunnel endpoint |

The host selected depends on the extension's declared capabilities, where it is installed, and whether it is UI-oriented or workspace-oriented. [web:50]

### 1.3 LSP: protocol, synchronization, tradeoffs

Microsoft describes LSP as a standardization of the messages exchanged between a development tool and a language server; the server runs as a separate process and communicates over JSON-RPC. [web:59]

```text
VS Code editor
    │  JSON-RPC over stdio, pipes, sockets, or WebSocket
    ▼
Language client extension
    ▼
Language server
    ├── parser
    ├── name resolver
    ├── type checker
    ├── index
    └── compiler or compiler-derived analysis
```

LSP uses JSON-RPC 2.0 requests/responses/notifications and cancellation; capability negotiation at initialization; document identifiers and version numbers; open/change/close notifications; requests for completion, hover, definition, references, symbols, formatting, code actions, rename; and asynchronously published diagnostics.

The protocol supports both **full and incremental** document synchronization. The specification defines `Full` and `Incremental` modes explicitly. [web:53]

```json
{
  "method": "textDocument/didChange",
  "params": {
    "textDocument": { "uri": "...", "version": 42 },
    "contentChanges": [
      { "range": { "start": {"line":10,"character":4},
                   "end":   {"line":10,"character":7} },
        "text": "value" }
    ]
  }
}
```

**Benefits.** M x N reduction - M editors and N languages need many integrations without LSP; LSP reduces that to roughly M clients plus N servers. [web:186] Language tooling can be implemented in the language's native ecosystem. The server can run close to the source tree, compiler, SDK and build environment. Multiple editors share one server. Remote development becomes natural.

**Costs.**

1. **Protocol overhead.** JSON serialization, process boundaries, document synchronization, range conversion, cancellation and asynchronous state add latency and failure modes.
2. **Text-model duplication.** The editor owns a live buffer while the server maintains its own representation. Even with incremental updates, both sides must maintain synchronized versions.
3. **Presentation-oriented API.** LSP largely describes what the editor should *display* rather than exposing a rich semantic object model. Rust-analyzer internally has compiler, HIR and IDE layers, while LSP is primarily the final presentation boundary. [web:182]
4. **Lowest-common-denominator pressure.** Language-specific refactorings, framework semantics, macro systems and build graphs do not fit equally well into a generic protocol. Eclipse documents both the genericity and the data-transfer/performance costs. [web:183]
5. **Compiler duplication.** Batch and interactive workloads differ fundamentally:

   | Batch compiler | Interactive language server |
   |---|---|
   | Starts from a build entry point | Must answer cursor-centric requests |
   | Can reject malformed input | Must analyze incomplete, invalid input |
   | Can discard syntax details after lowering | Needs source ranges, trivia, recoverable trees |
   | May rebuild or process whole units | Must update incrementally after edits |
   | Optimizes for final compilation | Optimizes for latency and partial answers |

   A server may reuse the compiler's lexer, parser, type system or IR, but usually needs additional incremental state, recovery logic, indexing, source mapping and IDE-specific query code. **LSP does not magically turn a batch compiler into an IDE.** [web:186][web:188]

The precise statement:

> LSP does not necessarily duplicate the compiler's *implementation*, but it frequently duplicates the compiler's *analysis pipeline and state*, because interactive editing is a different workload from batch compilation.

### 1.4 Remote development

The local workbench stays on the user's machine while a small VS Code Server and workspace-oriented extensions run near the source. Targets: SSH hosts, WSL, dev containers, remote tunnels, GitHub Codespaces. Microsoft describes the model as transparently moving command execution to a remote server. [web:97][web:99]

```text
Local machine                    Remote host or container
  ├── Electron workbench           ├── VS Code Server
  └── local/UI extensions   ───→   ├── workspace extensions
                                   ├── language servers
                                   ├── compilers and SDKs
                                   ├── debugger
                                   └── source tree
```

This avoids copying large repositories, SDKs, language-server indexes and build artifacts to the laptop. [web:95][web:97]

### 1.5 `vscode.dev`

The browser version, described by Microsoft as a zero-install VS Code experience running entirely in the browser. [web:94] Extensions must provide a browser entry point; Node.js APIs and arbitrary child processes are unavailable; file access may be virtualized; compute-heavy language servers may need a remote backend.

### 1.6 The marketplace is the real moat

The editor itself is reproducible - Electron, Monaco, tree-sitter, LSP, Git integration, terminals, much of the workbench. The difficult-to-reproduce asset is the **installed ecosystem**: language support, debuggers, formatters, linters, cloud and database integrations, framework tooling, authentication, GitHub/Azure/Kubernetes/Docker/CI integrations, themes, keymaps, plus documentation, ratings, install counts, update infrastructure and publisher trust.

Microsoft describes the Marketplace as hosting thousands of extensions across hundreds of languages and tasks. [web:123] A 2024 academic security study crawled **52,880 third-party extensions**, illustrating ecosystem scale. [web:126]

```text
More users → more attractive distribution for extension authors
          → more extensions and better integrations
          → higher switching cost for users
          → more users
```

A fork can reproduce the editor shell in months but still feel incomplete for years.

### 1.7 Forks: Cursor, Windsurf, Trae

Their strategic problem is that **source-code compatibility is not the same as ecosystem rights.**

Microsoft's Marketplace terms restrict use of the Marketplace and its services to Microsoft's Visual Studio family, including Microsoft-provided VS Code. The terms have been interpreted as preventing non-Microsoft forks from directly using the official Marketplace. [web:107][web:113]

Consequences: forks may need Open VSX or their own gallery; Microsoft-published extensions may be unavailable; extensions can contain product checks or restrictive licenses; manual VSIX installation does not necessarily resolve license restrictions; the fork must reproduce marketplace metadata, search, download, update, trust and compatibility infrastructure.

This is an effective moat because it is not purely technical. The situation is nuanced - many extensions are separately licensed, some MIT or open source, and forks can use Open VSX. Microsoft's documentation lists different licenses and source-availability statuses for its own extensions. [web:130]

---

## 2. JetBrains and the IntelliJ Platform

### 2.1 The platform

More than an editor widget: project model, virtual file system, indexing, PSI, inspections, refactoring framework, intentions and quick fixes, navigation, run/debug infrastructure, code completion, build-system integration, plugin APIs, caches and persistent indexes. Closer to a language-aware application framework than a thin editor shell.

### 2.2 PSI: Program Structure Interface

JetBrains describes PSI as the layer responsible for parsing files and creating the syntactic and semantic code model used by platform features. [web:20]

```text
Source file → Lexer → Parser → AST / PSI tree
    → References, types, scopes, symbols, inspections
    → Refactorings, navigation, completion, intentions
```

PSI elements expose a stable object model - `PsiClass`, `PsiMethod`, `PsiField`, references and resolve results, imports, expressions, annotations. Features ask semantic questions directly: what does this reference resolve to? what inherits from this class? what usages should be renamed? what type is expected here?

The PSI API is not merely a syntax tree - it integrates with indexes, caches, reference resolution, language plugins, project models and refactoring operations. The PSI cookbook exposes operations for references, renaming, inheritors, qualified-name lookup and overriding methods. [web:21]

### 2.3 Stubs and indexes

Keeping a complete PSI/AST tree for every file in a large project is too expensive. A **stub tree** is a compact serialized subset of a PSI tree, normally containing declarations and metadata needed to resolve symbols from other files, loadable from disk without fully parsing the source. [web:16][web:17]

```text
Full source      → full AST/PSI: detailed syntax and source text
Stub             → declarations, names, modifiers, signatures
Indexes          → class name → stub, method name → stub,
                   file name → files, package → files
```

This yields fast project-wide lookup through indexes and stubs, full detail when a file is opened, compact persistent caches surviving restarts, and platform-wide reuse. PSI has explicit time-space compromises: many objects are kept compact and data may be recomputed through accessors rather than stored redundantly. [web:193]

### 2.4 Why JetBrains did not adopt LSP for its primary languages

JetBrains supports LSP as an integration option, but its documentation explicitly says canonical IntelliJ language support provides wider IDE integration than merely handling data from a language server; LSP is described as an **additional value rather than a replacement** for the existing language API. [web:192]

| LSP-oriented integration | IntelliJ-native integration |
|---|---|
| Server returns presentation objects | Features query a shared semantic model |
| Completion, diagnostics, locations | PSI elements, references, types, indexes |
| Generic protocol boundary | Deep platform integration |
| Language server owns much state | IDE owns project and language model |
| Refactoring is protocol-defined | Refactoring operates on PSI and indexes |
| Easier portability | More semantic depth and consistency |

Adopting LSP for Java or Kotlin would place a protocol boundary between the language engine and features designed to operate directly on PSI, indexes, project models and refactoring APIs - losing information or requiring large-scale reconstruction on the IDE side.

### 2.5 Fleet's frontend/backend split

| Component | Responsibility |
|---|---|
| Frontend | UI, file parsing, limited highlighting, user interaction |
| Workspace | Shared state and coordination among frontends |
| Backend | Indexing, static analysis, search, navigation, language intelligence |
| Fleet System Daemon | Build, run, terminal, SDK, file-system and environment operations |

A Fleet backend can be a headless IntelliJ IDEA backend **or** a language server. Multiple frontends can attach to one workspace, supporting collaboration. [web:18][web:24]

---

## 3. Native editors

| Editor | Core architecture | Primary differentiator | Language intelligence |
|---|---|---|---|
| **Zed** | Rust, GPUI, native GPU-backed UI | Low latency, collaboration, AI | LSP plus native editor subsystems |
| **Helix** | Rust terminal editor, modal and selection-first | Built-in tree-sitter and LSP with minimal configuration | Native LSP client and tree-sitter |
| **Lapce** | Rust native GUI | Fast startup and responsiveness | LSP and tree-sitter |
| **Neovim** | C core, Lua extensibility, terminal/UI clients | Composability, modal editing, ecosystem | Built-in LSP and tree-sitter |
| **Xi** | Rust core, rope, multiprocess async plugins | Experimental high-performance architecture | Plugin/RPC architecture; **discontinued** |

### 3.1 Zed

Written from scratch in Rust using GPUI, a hybrid immediate/retained-mode GPU-accelerated UI framework. [web:41][web:196]

Aims: use multiple CPU cores; render through the GPU rather than a browser DOM; avoid Electron/Chromium overhead; keep editing and rendering responsive; make collaboration first-class; integrate AI agents directly.

Open-sourced January 2024. The editor is GPL-licensed, server-side collaboration components AGPL, GPUI Apache. [web:196]

Multiplayer uses CRDT-style conflict-free synchronization, with the stated goal of local responsiveness and eventual convergence; collaboration also includes presence, audio, screen sharing and channels. [web:202][web:205]

Traction strategy was **not** "rebuild VS Code's marketplace": make latency visibly better; provide multiplayer without external tools; integrate AI natively; open-source the product and UI framework; leverage the founding team's Atom/tree-sitter credibility; offer a smaller, more opinionated product.

### 3.2 Helix

Modal terminal editor with built-in multiple selections, LSP, tree-sitter and DAP support. [web:36] Selection-first editing rather than Vim's operator-first model; tree-sitter integrated rather than added afterwards; LSP without a large graphical framework.

Gained traction by offering a coherent default experience - users do not assemble dozens of plugins before obtaining modern syntax-aware editing.

### 3.3 Lapce

Rust-native, focused on startup time, keystroke responsiveness and GPU-accelerated native UI, bundling tree-sitter and LSP. [web:40] The constraint: native performance alone is not enough. Without VS Code's catalog or JetBrains' semantic depth, a native editor must be sharply differentiated or accept a narrower market.

### 3.4 Neovim

C editor core; Lua APIs and configuration; external UI clients; asynchronous jobs and RPC; plugins runnable outside the core; built-in LSP client (`vim.lsp`) [web:140][web:138] and tree-sitter integration for incremental buffer parsing. [web:136]

Its moat is not a centralized marketplace but a long-lived configuration and plugin culture plus a large installed Vim base.

### 3.5 Why Xi failed

Xi combined Rust, a rope text buffer, multiprocess frontend/core separation, asynchronous plugins, JSON-RPC and CRDT-based concurrent edits. **The project is discontinued.** [web:160][web:165]

The retrospective is unusually explicit. Its author wrote that the design accumulated too many **"novelty points"** and became substantially more complex than necessary. Problems:

1. **Too many independently difficult subsystems.** Rust, ropes, multiprocess architecture, async design and CRDTs were each reasonable, but their interactions multiplied complexity.
2. **Asynchronous ordering races.** User edits, plugin edits, selection changes, syntax updates and layout updates could arrive in different orders.
3. **Word-wrap and viewport tearing.** Live resizing caused rewrap updates to race with editing actions across the process boundary.
4. **Protocol and serialization cost.** JSON and a hand-rolled JSON-RPC layer; serializing large structured values increased binary size and communication overhead.
5. **Plugin edit reconciliation.** An asynchronous indentation or formatting plugin might calculate an edit against an old document while the user continued typing.
6. **CRDT complexity without a sufficiently valuable product payoff.**

The retrospective states that asynchronous design made the system more complicated and that races between editing actions and word-wrap updates were particularly difficult. [web:151] Xi's plugin documentation identifies the fundamental problem: plugin-generated edits race with user edits and must be reconciled. [web:157]

The lesson is not "multiprocess editors do not work" - VS Code demonstrates they can:

> **Process separation is most successful when the boundaries are stable and the protocol is narrow. Xi attempted to make editing, plugins, layout, and collaboration asynchronous at the same time, creating a distributed-systems problem inside the text editor.**

---

## 4. tree-sitter

Both a parser generator and an incremental parsing library, building a concrete syntax tree and efficiently updating it as source changes. [web:65][web:70]

### 4.1 Incremental parsing

On an edit the application supplies old and new byte ranges, old and new row/column positions, and the previous tree. tree-sitter marks affected regions, reuses unchanged subtrees, and reparses only where grammar context may have changed. Applications can query changed ranges between trees. [web:62]

Not every edit affects only a local node - a change to a delimiter, string, comment, preprocessor directive or indentation-sensitive construct can force a larger reparse. But unchanged structure is reusable in the common case.

### 4.2 Error recovery

Editors must parse code while it is temporarily invalid. tree-sitter produces a usable partial tree containing error nodes and, where appropriate, missing nodes, so highlighting, folding, structural selection and navigation continue while typing. [web:75][web:71] This is a major distinction from a batch compiler that reports an error and stops.

### 4.3 Query language

S-expression queries matching node types, fields, captures, wildcards, repetitions and predicates:

```scheme
(function_declaration
  name: (identifier) @function.name
  parameters: (parameter_list) @function.parameters)
```

Captures drive highlighting, text objects, structural selection, folding, indentation, language injection, symbol extraction, navigation and repository analysis. Patterns compile into an immutable query object executed through a cursor. [web:74]

### 4.4 Adoption

Fast enough for keystroke-level updates; tolerant of incomplete code; extensible through grammars; usable from C, Rust, JavaScript; structurally richer than regex highlighting. Used by Zed, Helix, Neovim, Lapce and many analysis tools. [web:136]

**tree-sitter is not a replacement for a compiler or language server.** It provides syntax structure, not complete type semantics, build configuration, macro expansion or framework knowledge.

---

## 5. Collaborative text editing

### 5.1 OT and CRDT

| Approach | Basic idea | Strength | Main difficulty |
|---|---|---|---|
| Operational Transformation | Transform concurrent operations against one another | Compact operations, established centralized designs | Transformation correctness, intention preservation, concurrency cases |
| CRDT | Represent edits so replicas merge deterministically | Offline operation, eventual convergence, peer-friendly sync | Metadata, tombstones, ordering anomalies, cursor and undo semantics |

Yjs summarizes the distinction: OT transforms index positions so clients converge; CRDTs use models such as linked lists with stable identities rather than relying primarily on index transformation. [web:79]

### 5.2 Main text-CRDT families

| System | Core model | Characteristics |
|---|---|---|
| **RGA** | Replicated Growable Array | Each item has identity and causal relationship; conceptually simple but can produce interleaving and metadata costs |
| **Yjs** | YATA, an RGA-like sequence CRDT | High-performance JavaScript; shared maps, arrays, text, awareness, many editor bindings [web:77] |
| **Automerge** | Change-based CRDT; RGA-like text plus rich JSON structures | Strong history, branching, syncing, maps, lists, counters, local-first document model |
| **Loro** | Rust CRDT, Fugue-style text and movable structures | Rich text, non-interleaving behavior, compact encoding, Rust/WASM |
| **Diamond Types** | Range-tree/event-graph text CRDT | Optimizes large plain-text workloads and positional operations |

### 5.3 RGA and interleaving

Concurrent insertions can interleave in surprising ways. If one user inserts `hello` and another inserts `world`, depending on anchors and ordering the result may be:

```text
hweolrllod       instead of       helloworld
```

This is convergence, but not user-intended grouping. YATA and newer algorithms improve ordering; Fugue-style approaches explicitly target non-interleaving.

> **"Conflict-free" means replicas converge; it does not automatically mean the merged text matches human intent.**

### 5.4 Zed's collaboration model

CRDT-backed collaboration, eventual consistency, shared presence, low-latency editing. [web:202][web:196] The user must not wait for a round trip to see their own keystroke; remote operations merge into the local document, while presence and cursor data synchronize separately from durable text operations.

### 5.5 Hard problems

**Interleaving and intent preservation.** Convergent but visually unnatural results; run-based insertion, causal ordering and left/right insertion policies all have edge cases.

**Cursor and selection synchronization.** A cursor is not a character offset once concurrent edits are possible:

```text
Absolute offset: 100                            ← fragile
Relative position: after character ID 7f2a...   ← stable under edits
```

Selections are harder - two moving endpoints, direction, affinity, possibly multiple ranges. Presence data has different durability and privacy requirements from document data.

**Undo.** Local undo should undo **the current user's own intent**, not blindly reverse the most recent remote operation. Requires operation grouping, author identity, causal context and selective undo:

```text
A inserts X;  B inserts Y;  A presses undo
→ A should remove X while preserving Y
```

Research on collaborative CRDT undo treats selective undo as a separate design problem, not a trivial inverse. [web:81]

**Deletes and tombstones.** CRDTs often retain deleted items as tombstones so late operations remain interpretable. Long-lived documents accumulate history. GC is possible only when the system knows all replicas, backups, offline clients and snapshots have advanced past the deleted content.

**Large-document performance.** A naive linked-list CRDT makes positional lookup and insertion expensive. Diamond Types uses a range-tree where internal nodes store character counts, allowing position-based operations in logarithmic time. [web:90]

Performance dimensions: visible text size, operation count, user count, causal-history size, tombstone count, random vs append-heavy editing, serialization/sync bandwidth, checkpointing and GC, and rendering/parsing after remote edits.

There is no universally best CRDT. Yjs suits JavaScript and shared data types; Automerge rich history-oriented local-first documents; Loro Rust/WASM and newer text semantics; Diamond Types high-performance plain text.

---

## 6. Economics and engineering scale

### 6.1 Person-year estimates

**There are no authoritative public figures.** The following are engineering estimates, not reported company metrics.

| System | Plausible cumulative effort through 2026 | Confidence | Basis |
|---|---:|---|---|
| **VS Code core/workbench** | ~**300-800 person-years** | Medium-low | A decade-plus of development, large repository, desktop/web/remote architecture, accessibility, security, extension API, release engineering |
| **VS Code + first-party remote, web, marketplace, major extensions** | ~**1,000-3,000 person-years** | Low-medium | Includes what makes the ecosystem commercially useful, not merely the shell |
| **IntelliJ Platform core** | ~**1,000-3,000 person-years** | Low-medium | Two decades, PSI, indexing, caches, refactoring, plugin compatibility, multiple products |
| **JetBrains primary-language and product layer** | Several additional **thousand person-years** | Low | Java/Kotlin, Python, JS, C#, database, framework, build, debugger integrations |

```text
person-years ≈ average engineers × years of active development
```

40 engineers over 10 years is already 400 person-years before QA, release engineering, design, documentation, security, support and management.

Public evidence establishes scale but not allocation: JetBrains reported more than 1,250 employees in 2020 and more than 2,600 in 2025-2026, and a JetBrains case study refers to infrastructure supporting 700-800 developers across IntelliJ-related development. [web:172][web:169][web:174] These are company-wide or development-organization figures, **not** a direct count of platform engineers.

### 6.2 Why the ecosystem matters more than the editor

The cost of building a basic editor is falling - native UI frameworks, mature Electron/web technologies, tree-sitter, LSP, reusable Git/terminal/debugger components, AI APIs.

The cost of a complete **developer environment** is much higher:

```text
Editor + language support + debuggers + build systems + test runners
      + containers + cloud integrations + authentication + security review
      + marketplace/discovery + updates + documentation + enterprise policy
      + remote execution
```

Developers choose VS Code partly because their required tools already exist; extension authors choose VS Code because users already exist. **A new editor must overcome both sides simultaneously.**

This explains why Microsoft can make the editor source available while retaining leverage through branded distribution, first-party extensions, Marketplace access, remote-development services, GitHub/Codespaces integration, telemetry and discovery, and enterprise support.

### 6.3 What Zed and Helix did differently

| Editor | Traction mechanism |
|---|---|
| **Zed** | Felt performance advantage; Rust and GPU rendering; built-in multiplayer; native AI; open source; strong founding team from Atom/tree-sitter; focused product identity |
| **Helix** | Terminal-native speed; modal selection-first editing; tree-sitter and LSP by default; minimal configuration; appeal to Vim/Neovim/Rust users |
| **Lapce** | Native Rust GUI; fast startup and input; tree-sitter and LSP included; familiar workflow |
| **Neovim** | Vim skill compatibility; Lua extensibility; composable plugins; built-in LSP and tree-sitter; terminal ubiquity |

> **A new editor can gain traction without matching VS Code feature-for-feature if it offers a conspicuous advantage in a high-frequency activity - typing latency, startup, collaboration, terminal workflow, or AI - and keeps its initial product surface coherent.**

The long-term challenge remains ecosystem depth. Replacing VS Code or JetBrains for mainstream teams requires years of language integrations, enterprise features, remote workflows, debugging support and extension coverage.

## Bottom line

- **VS Code's moat is distribution plus extensions plus remote/cloud integration**, not Electron itself.
- **LSP is an excellent interoperability boundary**, but it is a presentation protocol, not a complete semantic model; language servers still need compiler-derived and IDE-specific analysis.
- **JetBrains' moat is the integrated semantic substrate**: PSI, stubs, indexes, project models, inspections, refactorings.
- **tree-sitter supplies fast structural syntax**, not full type or framework semantics.
- **CRDT collaboration solves convergence and offline responsiveness**, not human intent, cursor semantics, undo, garbage collection or large-document efficiency.
- **Native editors gain traction by being dramatically better at something**, not by reproducing the entire VS Code platform on day one.
- **Xi demonstrates the danger of combining too many ambitious architectural boundaries at once**: asynchronous plugins, multiprocess editing, CRDTs, layout and RPC can turn an editor into a distributed system before the basic product is complete.

## Citations

[16] https://plugins.jetbrains.com/docs/intellij/stub-indexes.html
[17] https://plugins.jetbrains.com/docs/intellij/indexing-and-psi-stubs.html
[18] https://www.jetbrains.com/help/fleet/architecture-overview.html
[20] https://plugins.jetbrains.com/docs/intellij/psi.html
[21] https://plugins.jetbrains.com/docs/intellij/psi-cookbook.html
[24] https://blog.jetbrains.com/blog/2021/11/29/welcome-to-fleet/
[36] https://helix-editor.com/news/release-22-03-highlights/
[40] https://lap.dev/lapce/
[41] https://github.com/zed-industries/zed/tree/main/crates/gpui
[46] https://vscode-docs.readthedocs.io/en/stable/extensions/our-approach/
[49] https://arxiv.org/html/2411.07479v1
[50] https://code.visualstudio.com/api/advanced-topics/extension-host
[51] https://github.com/microsoft/vscode/wiki/source-code-organization
[53] https://github.com/Microsoft/language-server-protocol/blob/gh-pages/_specifications/lsp/3.17/specification.md
[57] https://code.visualstudio.com/blogs/2022/11/28/vscode-sandbox
[59] https://learn.microsoft.com/en-us/visualstudio/extensibility/language-server-protocol?view=vs-2022
[62] https://deepwiki.com/tree-sitter/tree-sitter/2.3-incremental-parsing
[65] https://github.com/tree-sitter
[70] https://github.com/tree-sitter/tree-sitter
[71] https://wippy.ai/en/lua/text/treesitter
[74] https://tree-sitter.github.io/tree-sitter/using-parsers/queries/4-api.html
[75] https://tree-sitter-tree-sitter.mintlify.app/introduction
[77] https://docs.yjs.dev/api/internals
[79] https://github.com/yjs/yjs
[81] https://dl.acm.org/doi/10.1007/978-3-319-19129-4_16
[90] https://josephg.com/blog/crdts-go-brrr/
[94] https://code.visualstudio.com/docs/remote/vscode-web
[95] https://code.visualstudio.com/remote/advancedcontainers/develop-remote-host
[97] https://code.visualstudio.com/docs/remote/remote-overview
[99] https://code.visualstudio.com/docs/remote/faq
[107] https://tessl.io/blog/microsofts-going-to-war
[113] https://blog.curiousbox.ai/p/the-fork-in-the-road-why-vs-code
[123] https://code.visualstudio.com/docs/configure/extensions/extension-marketplace
[126] https://arxiv.org/html/2411.07479v1
[130] https://code.visualstudio.com/docs/supporting/oss-extensions
[136] https://neovim.io/doc/user/treesitter/
[138] https://neo.vimhelp.org/lsp.txt.html
[140] https://www.andersevenrud.net/neovim.github.io/documentation/lsp/
[151] https://raphlinus.github.io/xi/2020/06/27/xi-retrospective.html
[157] https://xi-editor.io/docs/plugin.html
[160] https://github.com/xi-editor/xi-editor/blob/master/README.md
[165] https://xi-editor.io/2022/12/09/status-update.html
[169] https://www.jetbrains.com/lp/annualreport-2026/
[172] https://blog.jetbrains.com/blog/2020/02/05/20-years-of-jetbrains/
[174] https://blog.jetbrains.com/teamcity/2025/03/how-intellij-uses-teamcity/
[182] https://matklad.github.io/2023/10/12/lsp-could-have-been-better.html
[183] https://www.eclipse.org/community/eclipse_newsletter/2017/april/article5.php
[186] https://matklad.github.io/2022/04/25/why-lsp.html
[188] https://isaac-hung.com/blog/modern-compiler-design/
[192] https://plugins.jetbrains.com/docs/intellij/language-server-protocol.html
[193] https://plugins.jetbrains.com/docs/intellij/psi-performance.html
[196] https://zed.dev/blog/zed-is-now-open-source
[202] https://zed.dev/blog/zed-is-our-office
[205] https://zed.dev/blog/beta
