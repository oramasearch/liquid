# Frontend modules and per-definition build granularity

**What.** Liquid #65 adds a versioned C ABI for language frontends, a shared/static module loader,
and a Clang C++ module that turns definitions into compiler actions and assembles their objects into
one relocatable translation-unit object. Liquid #72 adds the digest-pinned 49-language tree-sitter
registry and tag-query syntax discovery. The query graph
keys compiler work by the canonical unit and action fingerprints, so changing one function body
does not invalidate its neighbours.

**Status.** The ABI, loader, registry, graph coordinator, bundled tree-sitter fallback, concrete
syntax and canonical parse operations, and bundled Clang C++ module are implemented. The CMake fixture compiles two definition objects with the
configured Clang, changes one body, executes one new action, and assembles the translation unit
again. Its header change executes both actions because both units read that header. Liquid #66 adds
the `symbols` ABI operation and graph query; its record is documented in [`symbols.md`](symbols.md).
Liquid #205 adds the optional `syntax` operation used by structural edits. Liquid #248 adds the
optional versioned presence, lease, and attribution event table to the native plugin descriptor.
Liquid #68 adds the optional `parse` descriptor tail and the digest-keyed `parse_tree` graph query
used by builds, symbols, and structural source diff.
The grammar distribution decision, complete inventory, and maintenance process are in
[`grammars.md`](grammars.md).

## ABI v1

[`frontend_abi.h`](../../include/liquid/frontend_abi.h) is valid C11 and contains no C++ or
third-party type. A dynamic module exports exactly `liquid_frontend_get_module_v1`; a statically
linked module passes the same descriptor to `frontend_module::linked`. The descriptor registers a
module name and one or more language names, then supplies these operations:

The format registry follows the same module pattern in the separately versioned
[`format_abi.h`](../../include/liquid/format_abi.h): one C descriptor, canonical-CBOR operation
values, owned bytes, and progress/cancellation callbacks. The two descriptors remain purpose-built
because their operation sets differ.

The same header defines `liquify_plugin_v1`, the native editor plugin descriptor from bridge
decision 14. It reuses the frontend ABI's byte spans, owned results, errors, progress, cancellation,
and destroy callback instead of creating another set of boundary types. Dynamic plugins export
`liquify_plugin_get_v1`; static plugins pass the same descriptor to liquify's checked host.

| Registration | Descriptor data | Operation |
|---|---|---|
| Query | stable graph-query name | canonical-CBOR arguments to canonical-CBOR result |
| Command | palette name, default binding, canonical-CBOR argument schema | canonical-CBOR arguments to canonical-CBOR result |
| Panel | panel name | canonical-CBOR context to a canonical-CBOR view model |

`struct_size` permits compatible descriptor tails. A plugin may register any combination of the
three kinds, but every advertised row has a non-empty name and operation. The host validates the
ABI version, required descriptor prefix, names, schemas, inputs, and results. Plugins return panel
failures through the ordinary ABI error value; liquify renders that failure in the affected panel
and continues updating the others. Plugins produce data and never receive an ImGui type or context.

### Plugin event table

A plugin subscribes to multiplayer updates by appending `events` to `liquify_plugin_v1` and
pointing it at `liquify_plugin_event_callbacks_v1`. The callback table has its own
`abi_version`, `struct_size`, and three independently optional function pointers:

| Callback | Canonical-CBOR value |
|---|---|
| `presence` | One versioned presence update with actor, session, path, range, activity, and time. |
| `lease` | One versioned lease update with holder, session, claim, authority, times, and active state. |
| `attribution` | One versioned attribution result with path, requested change, and line records. |

The version-1 maps are:

```text
presence: {
  v: 1, actor: text, session: text, path: text,
  range: null | source-range, activity: "editing" | "reading" | "idle", at: integer
}
lease: {
  v: 1, actor: text, session: text,
  claim: { path: text, kind: "file" | "symbol" | "range",
           symbol: null | text, range: null | source-range },
  authority: "advisory" | "lake", since: integer, expires: integer, active: bool
}
attribution: {
  v: 1, path: text, change: text,
  lines: [{ line: unsigned, text: text, actor: text, session: text,
            change: text, agent: bool }]
}
source-range: {
  start: { byte: unsigned, line: unsigned, column: unsigned },
  end: { byte: unsigned, line: unsigned, column: unsigned }
}
```

Times are Unix seconds, sessions are lowercase 32-digit hexadecimal strings, and changes are full
algorithm-prefixed digest strings. Lines and columns are one-based; byte offsets are zero-based and
half-open, matching `frontend_source_range`.

A non-null callback is the subscription. Liquify invokes it with the descriptor's
`plugin_context`; the input span is borrowed and remains valid only for the duration of the call.
The plugin copies any bytes it needs after returning. Event callbacks do not return owned bytes or
errors: rejection and retry belong to the repository or daemon operation that produced the event.
Callbacks are serialized by the host's event dispatcher, so plugins do not create worker threads
to receive them.

Compatibility is checked at both levels. A host first validates the plugin descriptor's original
v1 prefix. It reads `events` only when `liquify_plugin_v1.struct_size` covers that field; a shorter
v1 descriptor is an older compatible plugin with no event subscriptions. For a present table, the
host requires `LIQUIFY_PLUGIN_EVENTS_ABI_VERSION`, validates the callback table prefix covered by
its `struct_size`, and reads each callback only when that field is covered. A null table or null
callback means unsubscribed. Future tables append fields and increase `struct_size`; they do not
reorder or reinterpret the v1 prefix. An incompatible table version disables the table without
changing the compatibility of the plugin's queries, commands, or panels.

| Operation | Input | Canonical-CBOR result |
|---|---|---|
| `units` | language, source blob, context | array of unit maps |
| `compile_unit` | one unit map, context | executable frontend-action map |
| `assemble` | array of unit fingerprints and output byte strings, context | translation-unit output map |
| `symbols` | language, source blob, context | derived symbol-action description |
| `symbols_from_output` | language, source blob, symbol-action stdout | per-translation-unit symbol output map |
| `syntax` | language, source blob, context | concrete syntax tree |
| `parse` | language, source blob, context | compact canonical parse tree plus unit and symbol sidecars |

Every operation receives progress and cancellation callbacks. Return values use caller-independent
owned bytes with an explicit release callback. ABI status values distinguish invalid input,
unsupported operations, cancellation, and internal failure. The loader rejects the wrong ABI
version, an incomplete required descriptor prefix, invalid owned bytes, malformed values, and
non-canonical CBOR. `syntax` and `parse` are appended to the descriptor and are optional: an older
v1 descriptor or a null pointer returns `errc::unsupported`, so ABI v1 remains v1.

The compact parse value uses numeric node-kind IDs against its `kinds` table, half-open byte
ranges, one-based line and column positions, named children, optional field names, and explicit
error and missing flags. `grammar` is the lock digest and `node_types` is the SHA-256 identity of
the grammar's `node-types.json`. `units` and `symbols` are canonical sidecar bytes derived before
the tree-sitter tree is released. [`structural-diff.md`](structural-diff.md) documents its query and
consumers.

A syntax result is:

```text
{
  v: 1,
  language: text,
  root: {
    kind: text,
    named: bool,
    field?: text,
    range: { start: { byte, line, column }, end: { byte, line, column } },
    children: [ node ]
  }
}
```

The tree includes named and anonymous concrete nodes. `decode_syntax_tree` validates canonical
CBOR and child containment. `syntax_node::node_at(byte)` returns the deepest containing node, and
`enclosing(kind)` walks from a node toward the root. The tree-sitter module implements the
operation for every configured grammar, including C and C++; the Clang module returns unsupported.

ABI v1 modules may append `language_metadata` after the original descriptor fields. Each metadata
row identifies a registered language as `frontend` or `grammar`; grammar rows also carry the
canonical lock digest. The loader uses `struct_size`, accepts original ABI-v1 descriptors without
the tail, and validates a present tail before exposing it through the C++ registry.

A unit map is:

```text
{
  kind: 1 | 2 | 3,                 # declaration, definition, type
  name: text,
  range: {
    start: { byte, line, column },
    end:   { byte, line, column }
  },
  source: bytes
}
```

Lines and columns are one-based; byte offsets are zero-based half-open ranges. Libliquid computes
the BLAKE3 unit fingerprint from `kind`, `name`, and `source`, excluding the absolute range. An edit
earlier in the file can therefore move an unchanged function without changing its identity.

`frontend_module` exposes the checked C++ wrapper. Its existing `languages()` method still returns
names, while `language_details()` returns language, extensions, mechanism, and grammar digest.
`frontend_registry` owns modules, resolves a language registration, and aggregates those detailed
rows through `languages()`. `frontend_graph` owns the registered `parse_tree`, action, assembly,
and `symbols` query kinds, accepts a caller's action executor, and returns each canonical parse,
unit action/output, symbol sidecar, and assembled translation-unit value. Parse keys contain the
language and blob digest, so consumers share one value.
The process-global registry also feeds `support_matrix()`. It may contain both a grammar and
compiler module for one language; `languages()` merges their extensions and reports the Tier 3
compiler module, while `find()` selects that stronger module. `liquid tiers` loads both bundled
modules before rendering its matrix.
The executor remains injected because the daemon and `build::execute` own scheduling, sandboxing,
toolchain materialisation, and cache policy.

## Bundled modules

The targets are independent of libliquid and install below `libexec/liquid/frontends/`:

| Target | Languages | Behavior |
|---|---|---|
| `liquid-frontend-tree-sitter` | configured grammar set (49 by default) | parsing for every grammar; tag-query declarations and definitions where `tags.scm` is locked; `compile_unit` is explicitly unsupported |
| `liquid-frontend-clang` | C++ | definition actions using the compiler and arguments from the toolchain context; relocatable-object assembly |
| `liquid-frontend-clang-static` | C++ | the same descriptor and implementation for static registration |

The syntax target privately builds the exact runtime and grammar archives selected by
[`grammars.lock`](../../modules/frontends/grammars/grammars.lock). Archive, parser, scanner, and
query SHA-256 values are enforced; `LIQUID_GRAMMARS` selects a subset and
`LIQUID_GRAMMAR_MIRROR` changes the archive root without relaxing hashes. These inputs never link
into libliquid. The shared module export list contains only the ABI entry point.

For one selected definition, the Clang module keeps its body and replaces other definition bodies
with declarations using tree-sitter's exact body ranges. The resulting action contains the
toolchain compiler, its caller-supplied arguments, the transformed source, and observed quoted
includes. Unit actions produce ordinary object bytes. On macOS and Linux, `assemble` invokes the
same toolchain compiler with `-r` and returns canonical CBOR containing the relocatable object. The
Windows module returns a portable ordered unit-output value; per-definition Clang object assembly
there remains outside the supported target set.

For `symbols`, the Clang module describes an AST-dump action derived from a recorded compile
action. The build-engine adapter runs it through the normal sandboxed action executor and passes
its stdout to `symbols_from_output`, which retains definitions and resolved declaration references
from the compiler AST. The tree-sitter module emits syntax-only occurrences when no compiler
frontend is available. The syntax record carries the complete grammar-lock digest. For languages
other than C and C++, the module uses the locked upstream tag query; an absent query produces an
explicit unsupported response rather than guessed units. `frontend_graph::build` does not demand
the symbols query: editors request
it independently after the compile has completed.

## Graph behavior

`frontend_graph::build` discovers units and derives all action descriptions. Each definition action
is a query keyed by its canonical action value. The compute function records every action `reads`
path through `query_context::read` before invoking the executor. The assembly query depends on the
current action-query keys in unit order.

- A body edit changes that definition's action key. Other transformed sources and keys remain
  unchanged, so their prior query values are hits.
- A header edit calls `mark_changed(path)`. Only action nodes that recorded that path execute; the
  assembly query then validates their new object fingerprints.
- A compile that produces byte-identical output permits the existing graph early cutoff.
- Old fingerprint nodes remain content-addressed history. They are not demanded by the next build.

## Measurement

`liquid-frontend-measure` uses liquid's own `src/store/faults.cpp`, which has two definitions. It
performs an in-memory edit inside `visit_fault`, executes the emitted Clang action, and reassembles
the translation-unit object. It measures the edit once without a symbol demand and once with a
subsequent independent symbol demand in the same session. On an Apple M3 Max macOS host on
2026-09-04, at the lowest available load during concurrent work:

```text
sysctl -n vm.loadavg
{ 110.96 68.28 57.47 }
symbol extraction: file=src/store/faults.cpp units=1 occurrences=41 elapsed_ms=14039 per_unit_ms=14039
frontend one-function edit: symbols=disabled elapsed_ms=766 symbols=enabled elapsed_ms=713 delta_ms=-53
```

Run the measurement from a configured static build with:

```sh
cmake --build build --target liquid-frontend-measure
build/tests/liquid-frontend-measure . build/frontend-measure-state
```

The edit delta is the relevant result: the symbol query does not delay compile or relink. The
symbol value includes the independent compiler AST action and decoding. These local development
measurements are context, not cross-host benchmarks.

## How to verify

The portable frontend tests prove language registration, static registration, all locked samples
without error nodes, selected tag-query units, incompatible grammar-ABI rejection, tree-sitter C
unit ranges, canonical action generation, per-definition fingerprints, progress/error ownership, and
the CMake fixture's body/header invalidation behavior. `public-frontend-c-header` compiles the ABI as
C. The Clang module test loads `$<TARGET_FILE:liquid-frontend-clang>`, which proves the module is a
separate shared object rather than code folded into libliquid.

Run the repository's static, shared, and Linux-container proofs from [`../build/ci.md`](../build/ci.md).

## Limits

- The first Clang target covers C++ definitions whose translation unit remains valid when sibling
  bodies become declarations. Unsupported compiler constructs must fall back to translation-unit
  compilation rather than return a partial object.
- Quoted includes are the v1 read set. Compiler-produced dependency scanning for system and
  generated headers belongs in the next frontend revision.
- The tree-sitter module is syntax-only and never claims compiler equivalence. Languages without a
  locked `tags.scm` support parsing but not unit or symbol discovery.
