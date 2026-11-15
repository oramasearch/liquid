# Dependency resolution

**What.** The public package records, spec language, universe layout, and implementation boundary
for liquid's dependency resolver. The resolver binds versions, variants, virtual providers,
compiler dependencies, target microarchitectures, conflicts, and patches into content-addressed
`concretised_spec/1` records.

**Why.** Native package identity is a configured build rather than a name and version. A complete
identity must include every configuration dimension and every selected dependency, while the
selection policy must prefer work liquid can avoid.

## Public model

`include/liquid/resolve.hpp` contains only standard-library values, liquid digests, `result`, and
the Capy task alias inherited through `runtime.hpp`. `parse_spec` and both record codecs are pure
and synchronous. Directory and repository universe loading and `resolve` may wait, so they return
`runtime::task<T>`.

The accepted spec form is the Spack-style grammar fixed by the feature specification:

```text
hdf5@1.14.3+mpi~fortran%gcc@13.2 target=zen4 mode=threaded ^openmpi@5
```

The parser reports one-based columns and the expected token. `to_string` emits a deterministic
canonical spelling; parsing that spelling reproduces the same `spec` value.

## Records

`package/1` is canonical CBOR with these fields:

- `name` and one or more `versions`, each with an optional source digest;
- boolean or multi-valued `variants`, defaults, and optional `when` conditions;
- concrete or virtual `dependencies`, version constraints, conditions, and build/link/run type;
- versioned virtual `provides`, conflicts with their declared origin text, compiler and target
  constraints, and conditional patch digests.

`concretised_spec/1` binds the package name, version, every variant value, compiler record digest,
target, patch digests, and typed dependency edges to other concretised record digests. Its own
identifier is the BLAKE3 digest of its canonical bytes; the `id` field is derived and is not
encoded into itself. Selection reasons are stable projections of the solve and are deliberately
not encoded: changing explanatory wording cannot change package identity.

Both records use RFC 8949 canonical ordering through liquid's existing CBOR codec. Malformed,
incomplete, or unsupported records return `errc::corrupt`; an invalid value supplied for encoding
returns `errc::invalid_argument`.

## Package-universe layout

A filesystem universe is a directory, or its `packages/` child when present. Every regular
`.liquid` file below that root is parsed, checked, and lowered in memory; every `.cbor` file
remains one canonical `package/1` record. Paths are sorted before loading, and package names must
be unique across both formats. A `.cbor` record takes precedence over a sibling `.liquid` file
with the same stem, preserving the paired output of the Spack importer. Remove or regenerate the
record when editing that declaration. Includes retain the configuration language's local-path checks.
The same source declaration discovered through an include and through the scan is loaded once;
different declarations with the same package name remain an error.

A repository universe uses the same visible layout in its working tree:

```text
packages/<namespace>/<package>.liquid
packages/<namespace>/<package>.cbor
```

The loader walks tree records and reads package blobs through the public repository API.
Declaration parsing and lowering run off the coroutine executor; includes resolve against the
repository working directory. Generator declarations are refused with an instruction to use
`config lower --output-record` first. Loading never starts a generator or fetches sources.

`liquid config lower <file> --output-record <file|->` emits a single canonical `package/1`
record to a file or binary stdout. Select one package from a multi-package document with
`--package <name>`. The [executable guide](../guides/resolve-packages.md) resolves a two-package
dependency graph from declarations and verifies that the emitted records produce identical JSON.

When the loader returns no packages, the CLI reports `No candidates were loaded from <path>;
supply .liquid declarations or canonical .cbor records` before calling the solver. Empty and
unsupported-only directories receive this diagnostic even with `--json`; it is a loading error
on stderr with exit status 1. Invalid supported inputs retain their checker or codec errors.
The solver's semantics, attributed conflict core, and JSON remain unchanged.

The CLI additionally reads `<universe>/reuse.txt` when `--reuse` is present. Each non-comment line
is one already-built concretised-spec digest. Library callers instead pass those digests directly
in `resolve_options::reuse`; repository result discovery remains outside the resolver.

## Constraint model and engine

Grounding expands each package version, valid variant combination, and allowed target into a
candidate variable. Clauses enforce at most one candidate per concrete package, root constraints,
conditional concrete and virtual dependencies, compiler requirements, target compatibility, and
conflicts. Compiler candidates are ordinary package candidates and appear as typed build edges in
the completed graph. A virtual edge binds one concrete package whose `provides` range satisfies the
requested interface range.

Version constraints accept exact or prefix versions, comma intersections, comparisons such as
`>=1.14`, and inclusive colon ranges such as `1.12:1.14`. A requested microarchitecture can select
itself or a declared family fallback. The current named families cover x86-64 and Zen, AArch64 and
Neoverse, and Apple AArch64; otherwise only an exact target or `generic` satisfies the request.

The private `concretizer` interface separates `ground` from `solve`. The in-tree engine performs
Boolean propagation, decisions, conflict-clause learning, and decision-level backtracking. It
enumerates satisfying selected sets when an external preference can change the winner. Every
graph is sorted before hashing, and a digest comparison breaks a fully equal objective, so host
iteration order cannot affect the result. `resolve` runs grounding and solving as separate
`runtime::offload` operations and checks the caller's stop token before, between, and during those
phases.

Graph construction is iterative. Once a model is chosen, the builder walks the selected packages in
postorder with an explicit frame stack: a frame opens when a package is first reached and plans its
dependency and compiler edges, and closes once every edge target has a node, binding the edge
digests, hashing the canonical record, and publishing the node. A package holds at most one open
frame, so the stack never exceeds the number of selected packages and every frame lives on the heap.
Native recursion depth is constant whatever the dependency-chain length. The bound matters because
`runtime::offload` runs the solver on a runtime pool thread, whose stack on macOS is 512 KiB rather
than the main thread's 8 MiB.

## Optimisation

Models are compared lexicographically, never by a blended score:

1. maximise concretised specs present in `resolve_options::reuse`, including a candidate accepted
   by a compatible `abi_compatibility/1` fact whose built subject is reusable;
2. maximise candidates in `resolve_options::known_outputs`;
3. prefer newer versions across the graph;
4. minimise variant values that differ from package defaults;
5. use the graph digest as a deterministic final tie-break.

The first two inputs are caller-owned sets. An empty known-output set is the expected state until
issue #26 supplies content-identity observations. ABI compatibility facts are accepted now so issue
#27 does not need to change the solver interface.

## Explanations

Each hard constraint carries an origin kind (`root`, `dependency`, `conflict`, `compiler`, or
`target`), package, and detail. When no model exists, deletion minimisation removes every origin
whose absence does not restore satisfiability. The remaining core is sorted by origin and narrated
as one line per conflicting fact followed by the number of facts that cannot coexist.

The JSON form is versioned and stable. An unsatisfiable result has this shape:

```json
{"version":1,"unsatisfiable":true,"core":[{"kind":"root","package":"app","constraint":"root requires app@1","detail":"root requires app@1"}],"narrative":"Resolution is unsatisfiable.\n..."}
```

A successful graph JSON object contains its digest, root digests, and sorted nodes. Each node names
its concretised digest, version, target, variants, optional compiler digest, typed dependency
edges, and selection reasons. `why(graph, package)` returns versioned JSON plus a text narrative
explaining root or dependency selection, version preference, bound variants, and any reuse or
known-output preference that won.

## Solver engine decision

**Decided on 2026-09-05: liquid builds its own CDCL solver.**
Clingo is not vendored. The package encoding talks to the engine through the private
`src/resolve/concretizer.hpp` interface so the constraint model and search machinery remain
independently testable. The decision favours one auditable, coroutine-compatible implementation
and accepts responsibility for the solver engineering required by Spack-grade behaviour.

## Verification

`tests/resolve/test_resolution.cpp` pins parser round trips and error columns; canonical package and
concretised-record round trips; directory and repository loading of declarations and canonical records; include deduplication;
malformed declarations and duplicate names; virtual-provider, compiler,
conflict, target, cancellation, conditional-variant, reuse, compatibility, and known-output
behavior; minimal origins; exact graph and conflict JSON; CLI JSON; and a generated 500-package
graph. `tests/headers/resolve.cpp` compiles the public header in isolation, and the installed
consumer receives it through `liquid/liquid.hpp`.

### Scale measurement

The fixture generates 500 packages with realistic fan-in: each package has two versions, a Boolean
variant, and up to three link dependencies at offsets 1, 3, and 11. It resolves one root, then
resolves that root again with all 500 prior concretisations offered for reuse. On 2026-09-05 on an
Apple Silicon macOS host, a release static build reported:

```text
resolution measurement: packages=500 cold_ms=80 resolve_again_ms=81
vm.loadavg={ 36.85 42.59 47.28 }
```

The measurement includes both offloaded grounding and solving but not fixture construction. The
second call grounds independently and demonstrates reuse preference rather than an incremental
grounder cache.

### Worker-thread stack bound

Issue #263 recorded this fixture crashing with a bus error in Release builds while an unoptimised
build passed. Graph construction then recursed once per dependency edge, and the fixture's offset-1
edges form a 500-package chain, so 500 native frames ran on a pool thread with a 512 KiB stack. At
`-O3` on Apple Silicon each frame was 704 bytes, 344 KiB in total, and the remaining margin
depended on inlining decisions elsewhere in the binary; when it ran out, the fault landed in a
`std::variant` allocation inside version comparison at the deepest level. AddressSanitizer reports
the same failure as `stack-overflow`. Construction is now iterative as described above, and the
suite also resolves a 2,000-package strict chain, whose recursive form would have needed about
1.4 MiB of native stack.

## Open questions

- Package-repository seeding remains issue #75.
- Expanding named target families beyond those listed above belongs with issue #75's imported
  package vocabulary.
