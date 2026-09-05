# Frontend modules and per-definition build granularity

**What.** Liquid #65 adds a versioned C ABI for language frontends, a shared/static module loader,
tree-sitter C and C++ syntax discovery, and a Clang C++ module that turns definitions into compiler
actions and assembles their objects into one relocatable translation-unit object. The query graph
keys compiler work by the canonical unit and action fingerprints, so changing one function body
does not invalidate its neighbours.

**Status.** The ABI, loader, registry, graph coordinator, bundled tree-sitter fallback, and bundled
Clang C++ module are implemented. The CMake fixture compiles two definition objects with the
configured Clang, changes one body, executes one new action, and assembles the translation unit
again. Its header change executes both actions because both units read that header. Liquid #66 adds
the `symbols` ABI operation and graph query; its record is documented in [`symbols.md`](symbols.md).

## ABI v1

[`frontend_abi.h`](../../include/liquid/frontend_abi.h) is valid C11 and contains no C++ or
third-party type. A dynamic module exports exactly `liquid_frontend_get_module_v1`; a statically
linked module passes the same descriptor to `frontend_module::linked`. The descriptor registers a
module name and one or more language names, then supplies these operations:

The format registry follows the same module pattern in the separately versioned
[`format_abi.h`](../../include/liquid/format_abi.h): one C descriptor, canonical-CBOR operation
values, owned bytes, and progress/cancellation callbacks. The two descriptors remain purpose-built
because their operation sets differ.

| Operation | Input | Canonical-CBOR result |
|---|---|---|
| `units` | language, source blob, context | array of unit maps |
| `compile_unit` | one unit map, context | executable frontend-action map |
| `assemble` | array of unit fingerprints and output byte strings, context | translation-unit output map |
| `symbols` | language, source blob, context | derived symbol-action description |
| `symbols_from_output` | language, source blob, symbol-action stdout | per-translation-unit symbol output map |

Every operation receives progress and cancellation callbacks. Return values use caller-independent
owned bytes with an explicit release callback. ABI status values distinguish invalid input,
unsupported operations, cancellation, and internal failure. The loader rejects the wrong ABI
version, an incomplete descriptor, invalid owned bytes, malformed values, and non-canonical CBOR.

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

`frontend_module` exposes the checked C++ wrapper. `frontend_registry` owns modules and resolves a
language registration. `frontend_graph` owns the registered action, assembly, and `symbols` query
kinds, accepts a caller's action executor, and returns each unit action/output, symbol sidecar,
and assembled translation-unit value.
The executor remains injected because the daemon and `build::execute` own scheduling, sandboxing,
toolchain materialisation, and cache policy.

## Bundled modules

The targets are independent of libliquid and install below `libexec/liquid/frontends/`:

| Target | Languages | Behavior |
|---|---|---|
| `liquid-frontend-tree-sitter` | C, C++ | declarations, types, and definition ranges; `compile_unit` is explicitly unsupported |
| `liquid-frontend-clang` | C++ | definition actions using the compiler and arguments from the toolchain context; relocatable-object assembly |
| `liquid-frontend-clang-static` | C++ | the same descriptor and implementation for static registration |

The syntax target privately builds tree-sitter 0.25.9, tree-sitter-c 0.24.1, and tree-sitter-cpp
0.23.4. Those targets never link into libliquid. The shared module export list contains only the
ABI entry point.

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
frontend is available. `frontend_graph::build` does not demand the symbols query: editors request
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

The portable frontend tests prove language registration, static registration, tree-sitter C unit
ranges, canonical action generation, per-definition fingerprints, progress/error ownership, and
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
- The tree-sitter module is syntax-only and never claims compiler equivalence.
