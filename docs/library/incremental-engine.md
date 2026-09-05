# The incremental engine: the query graph, the daemon, and liquify on the graph

> Phase 4 of the plan ([`action-plan.md`](https://github.com/oramasearch/liquid-bridge/blob/main/docs/action-plan.md),
> epics https://github.com/oramasearch/liquid/issues/36 and https://github.com/oramasearch/liquify/issues/13).
> Specification: [`features/04-build-engine.md`](../features/04-build-engine.md) (the model, early
> cutoff, durability tiers, granularity, compile as you type, the daemon, correctness),
> [`features/21-liquify-ide.md`](../features/21-liquify-ide.md) (the one architectural argument,
> stub trees, design position), [`features/22-change-capture.md`](../features/22-change-capture.md)
> (the fan-out), [`features/12-search.md`](../features/12-search.md) (the planner, symbols).
> Decisions that shape this page (bridge `docs/decisions.md`): §14 a clean native plugin API, §15
> a retained GPU text layer inside Dear ImGui.

**Status:** the demand-driven query graph in #20, per-checkout daemon in #21, and per-definition
frontend granularity in #65 are implemented, including persisted red-green validation,
durability-filtered invalidation, cancellation, action-query integration, ordered cursor fan-out,
bounded resident graph values, module loading, unit-keyed actions, and translation-unit assembly.
The clean-rebuild scheduler and planner remain with their listed issues. Symbol records are
implemented by #66.

Phases 2 and 3 built actions: pure functions from an input tree, a command and an environment to
outputs, cached by digest, observed by the mount, verified by re-execution, served locally and
remotely ([`build-engine.md`](build-engine.md), [`distribution.md`](distribution.md)). Phase 4 puts
the same mechanism under a demand-driven graph that stays warm, so a keystroke recomputes only what
that keystroke changed, and gives liquify that graph in-process.

**Exit for the phase:** a one-line change in a large C++ project recompiles one translation unit
and relinks; the editor shows the diagnostic before the build finishes; a comment-only edit may
re-run the affected compile query but does not re-run the link when the object fingerprint is
unchanged.

## Queries and actions are one mechanism (#20)

A **query** is a memoised function from inputs to a value, keyed by a query kind and its
arguments. An **action** is the coarse end of the same spectrum: its inputs are a tree and a
command, its value is a result record. The engine does not distinguish them: `execute` from Phase 2
becomes the `action_result` query, `wrap`'s per-tool actions become nodes, and every derived thing
liquify asks for (a parse, a stub, a symbol table, a diagnostic list, a completion) is a query too.

Every query value carries a **fingerprint**: the digest of its canonical encoding. Every query
records the queries and inputs it read while computing (the observed-dependency principle from the
sandbox, applied inside the process: reads go through one accessor that records them). An **input**
is a leaf: a blob at a path in the working tree, an editor overlay buffer, an environment value,
the toolchain digest.

**Validation is red-green.** When an input changes, the engine does not recompute dependents; it
marks them *maybe stale* and, on the next demand, walks each dependency in the order it was read
the last time: if a dependency's fingerprint is unchanged the walk continues; if one changed, the
query re-executes. **Early cutoff**: when a re-executed query produces the same fingerprint, its
dependents are marked green without re-executing. A comment-only edit therefore re-runs the
compile query, produces the same object fingerprint, and does not execute the link query.

**Durability tiers.** Each input carries a durability (volatile, low, medium, high, as the
specification tables them) and each query the minimum durability of everything it read. A change
to a volatile input (the buffer being typed in) marks stale only queries whose durability is
volatile; the rest of the graph is not visited. The tier of a path is configured per repository
(`config.build.durability`) when #21 adds daemon configuration. The #20 API takes the tier at each
input change; build integration assigns the working tree low, overlays volatile, dependencies
medium, and the toolchain tree high.

**Persistence.** The graph is a record set in the store. A `query-graph/1` root references
`query-node/1` records containing canonical keys, fingerprints, durability, dependency lists in
read order, and a value digest. Values are separate store objects and load only on demand. `ROOT`
is replaced atomically after each completed query; an interrupted computation can leave
unreachable objects but cannot replace the last complete root. Opening reads the root and node
metadata, marks loaded nodes maybe stale, and leaves values shallow. The next demand validates
against current inputs by the same red-green walk, so a restart costs fingerprint reads rather
than query execution.

The reproducible `liquid-graph-measure` target populated one query per regular file in liquid's
checkout, excluding `.git`, `.liquid-build`, and build directories, then reopened and validated
the graph. On an Apple M3 Max macOS host on 2026-09-04:

```text
graph cold-start validation: files=3656 fingerprint_reads=3656 executions=0 hits=3656 elapsed_ms=3038
```

Run it with `cmake --build build --target liquid-graph-measure`, followed by
`build/tests/liquid-graph-measure . <empty-state-directory-outside-checkout>`.

**Cancellation.** A keystroke cancels in-flight queries that depend on the changed volatile input;
queries are Capy coroutines, and cancellation is cooperative at every dependency read. Results of
cancelled computations are discarded, never stored.

`query_registry` canonicalises arguments before constructing a key. All input and query reads go
through `query_context::read`, which records the dependency and fingerprint. Working-tree blobs,
overlay buffers, environment values, and toolchain digests are the supported leaves. Public
`graph::recorded` returns a query's canonical key, value and fingerprint, plus the recorded inputs
in read order; #23 uses this comparison hook without accessing graph internals.

`build::execute` demands an `action_result` query before disk lookup or execution. `wrap` makes
the driver and each discovered tool action an `action_result` node, primes those nodes after
publication, and demands them on the next invocation. When an upstream action is invalidated,
the wrapper conditionally retains a downstream candidate and the child hook compares the recorded
input bytes after the producer finishes. Equal object bytes cut off the linker; changed bytes make
the linker execute. The existing result validation still verifies signatures, manifests,
referenced objects, and divergent-result quarantine before a graph hit is reported; a failed check
forces the action query to recompute.

## The daemon (#21)

**Implementation status: implemented by liquid #21 on macOS and Linux.** `daemon::start` hosts the
capture owner and public `liquid::graph` in the current process; `daemon::attach` connects another
client.
The CLI starts a detached host on its first daemon-backed command. The current Corosio revision has
Unix local streams but no Windows named-pipe transport, so every daemon entry point returns the
documented `errc::unsupported` result on Windows. The daemon opens #20's graph with the checkout as
its working tree and feeds changed paths into `graph::mark_changed` at low durability.

One long-lived process per checkout: started on first use by the CLI or liquify, single instance
enforced by an advisory lock in `.liquid/daemon/`, idle exit after a configured period, shutdown
that persists the graph. The earlier `LIQUID_REPO/daemon/` design path cannot be used because
`LIQUID_REPO` is the repository checkpoint file, not a directory. It owns the capture subscription
([`capture.md`](capture.md)): a drip arrives as a journal cursor advance; the daemon maps changed
paths to inputs, marks the minimum stale, and fans out to pluggable targets in order: the build
graph (demand what the last build demanded), the local search index (#22), collaborators and lake
(the sync client from
[`sync.md`](sync.md)), each with its own cursor, at-least-once and idempotent, as
`features/22` requires. `liquid status` shows what the last drip triggered and how far each target
is behind.

**Memory.** The graph has a ceiling (`config.build.graph_memory`); resident values are evicted
least recently demanded first (they are recomputable, and results are in the store anyway). Keys,
fingerprints, dependency lists, and stale state remain after eviction. The public graph accounts
resident value bytes and reports the key count, resident value count, use, and ceiling in status.

**Clients** talk to the daemon over one local endpoint: the CLI over a Unix socket with the same
one-byte type, four-byte big-endian length, and canonical-CBOR payload shape the sync protocol
uses. The socket has a deterministic checkout-derived name in the platform temporary directory so
macOS's short Unix-socket path limit does not reject deep checkouts; its lock, cursor state, and
graph remain under `.liquid/daemon/`. Windows returns `errc::unsupported` until Corosio supplies
named pipes. liquify links libliquid and hosts the graph in-process (liquify hosts the daemon
service when it is the first client, and the CLI attaches to it), which is the whole point of the
architecture and is the only way to avoid a serialisation boundary.

The built-in target order is build, search, collaborators, then lake. The build target marks only
working-tree inputs named by the drip operations changed through #20's public graph. The remaining
built-ins are idempotent acceptance hooks until #22 and the collaborator work bind their
implementations.
Callers can instead pass ordered `daemon_target` implementations. Every target persists its own
journal cursor only after successful delivery. Replayed or duplicate cursor values are ignored;
a failed target keeps its old cursor and error, so restart retries it. `liquid status` reports the
last drip, changed paths, delivery count, cursor, error, and cursor distance for each target.

## Continuous clean-rebuild validation (#23)

Phase 2's sampled re-execution verifies actions; Phase 4 verifies the graph: on idle capacity the
daemon picks sampled targets, rebuilds them from clean in a fresh sandbox, and compares outputs
with the incremental result. A divergence is reported with the query that was wrong and its
recorded inputs, as a critical alert and an audit-able record, and the offending value is
evicted so the next demand recomputes it. Zero divergences on the reference projects (liquid
itself from #59, the wrap fixtures) is the acceptance.

## Granularity: per translation unit, then per definition (#65)

Per translation unit stays the unit for linking. Per definition is a frontend's job: the versioned
C ABI in [`frontend_abi.h`](../../include/liquid/frontend_abi.h) registers languages and exchanges
canonical CBOR for units, compiler actions, unit outputs, and assembly results. Its complete
contract and measured proof are in [`frontends.md`](frontends.md). The tree-sitter fallback yields
C and C++ declarations and definitions without compiling. The Clang C++ module uses the configured
toolchain compiler to produce one action per definition and assembles the resulting objects into a
relocatable translation-unit object. The module is replaceable and lives outside libliquid's core
under `modules/frontends/`, so its parser and compiler dependencies never enter libliquid.

Unit fingerprints cover kind, name, and source while excluding the absolute range. Definition
action queries record quoted-header reads and assembly queries depend on their ordered action
outputs. A body edit therefore creates one action key and reassembles; a header edit invalidates the
definitions that recorded it.

## Symbols and the search planner (#66, #67, #22)

Compiler-derived symbols are produced by a query over a recorded compile action: the derived action
uses the compile's input and toolchain trees and replaces its object-output arguments with AST
extraction arguments. It emits one SCIP-equivalent, content-addressed symbol record per
translation unit, with tree-sitter's syntactic symbols as the fallback. The `symbols` query
returns the canonical record for its source blob; [`symbols.md`](symbols.md) fixes its public
shape. Compile and relink do not demand the symbols query, so extraction remains off the edit
path. The local search index (#22) is oramacore-cpp
BM25F and lake's trigram index run in the daemon, updated per drip from the fan-out, indexed by
blob once. The **planner** (#67) chooses per query among BM25F, the trigram index and a brute-force
scan over blobs by cost (index availability, term selectivity, corpus size), and explains its
choice the way `liquid why` explains a cache miss.

**Measurement.** On the Apple M3 Max macOS host on 2026-09-04, at
`sysctl -n vm.loadavg` = `{ 110.96 68.28 57.47 }`, the Clang frontend extracted 41 occurrences
from liquid's `src/store/faults.cpp` in one translation unit:

```text
symbol extraction: file=src/store/faults.cpp units=1 occurrences=41 elapsed_ms=14039 per_unit_ms=14039
frontend one-function edit: symbols=disabled elapsed_ms=766 symbols=enabled elapsed_ms=713 delta_ms=-53
```

The edit measurements were taken in the same session; the symbol demand followed the edit. The
small delta confirms that compile and relink do not demand extraction.

## liquify on the graph

liquify (its epic https://github.com/oramasearch/liquify/issues/13) is a client of this engine
in-process:

- **Overlay inputs.** An open buffer is a volatile input backed by memory; the graph reads it
  through the same accessor as a blob; a save turns it into a drip and the overlay into the blob.
- **Interactive analysis (liquify #5).** Cursor-centric queries (the node at a position, its
  definition, references, hover, completion) run over error-tolerant parse trees with source ranges
  and trivia produced by the frontend module in interactive mode; these are queries like any other,
  cancelled on the next keystroke.
- **Stub trees (liquify #4).** A stub per blob (declarations, names, signatures) is an action
  output, content-addressed and cached; cross-file resolution reads stubs and only open files hold
  full trees.
- **Diagnostics from the graph.** The build populates diagnostics as query values; the editor
  shows them as soon as the query completes, before the build's link step, which is the exit
  criterion.
- **Inline build, test and deploy (liquify #6).** The editor demands the same queries the CLI does;
  there is no second path.
- **Rendering (decision §15).** The buffer is a retained GPU text layer inside Dear ImGui with a
  per-blob cached line layout; panels and the shell stay ImGui.
- **The plugin ABI (decision §14).** One C ABI (query kinds registered by name, values as
  canonical CBOR, callbacks for progress and cancellation) that frontend modules, format codecs and
  editor plugins share; no VS Code compatibility in this phase.

## Waves

1. liquid #20 (the graph, persistence, cancellation) with #23's validation hook designed in;
   liquid #21 (the daemon, fan-out, memory ceiling); liquify #4 (stub trees, against #20's
   query interface) and liquify #1's shell work that does not need the graph (the retained text
   layer).
2. liquid #65 (**implemented:** frontend module boundary and C++ per definition, with C/C++ syntax
   fallback), #66 (symbols), #22 (local index on drip), #23 (validation on the reference projects);
   liquify #5 (interactive analysis)
   and #1 (the editor on the graph).
3. liquid #67 (the planner); liquify #6 (inline build, test, deploy).

## How to verify

`QueryGraph` tests cover read-order red-green validation, early cutoff, exact volatile
invalidation walks, cancellation by a second demand, interrupted-process recovery, shallow value
loading, fingerprint-only restart validation, and the #23 retrieval hook. The mount-backed CMake
fixture proves a comment edit executes one compile process with identical object and link results,
while a one-line semantic edit executes one compile and the link and changes the link result.
`FrontendModule` and `FrontendGraph` prove dynamic and static registration, C fallback units, exact
definition actions, one action plus assembly after a body edit, and every recorded reader plus
assembly after a header edit. `liquid-frontend-measure` records the own-tree edit result in
[`frontends.md`](frontends.md). Remaining phase exit measurements belong to the issues that add the
daemon and editor diagnostics.

## How to change

An issue that finds this contract wrong changes it in the same commit as the code and says why in
the commit body.
