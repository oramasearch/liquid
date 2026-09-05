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
encoded into itself. Reasons are stable explanatory data and therefore are part of the record.

Both records use RFC 8949 canonical ordering through liquid's existing CBOR codec. Malformed,
incomplete, or unsupported records return `errc::corrupt`; an invalid value supplied for encoding
returns `errc::invalid_argument`.

## Package-universe layout

A filesystem universe is either a directory containing `packages/` or the `packages/` directory
itself. Every regular file below that root whose name ends in `.cbor` is one `package/1` record.
Paths are sorted before decoding and package names must be unique.

A repository universe uses the same visible layout in its working tree:

```text
packages/<namespace>/<package>.cbor
```

The loader walks tree records and reads the package blobs through the public repository API. The
layout needs no configuration-language dependency; issue #74 can lower declarations directly to
these records, and issue #75 can populate the same tree.

## Solver engine decision

**Decided by Steve on 2026-09-05 (liquid-bridge decision 20): liquid builds its own CDCL solver.**
Clingo is not vendored. The package encoding talks to the engine through the private
`src/resolve/concretizer.hpp` interface so the constraint model and search machinery remain
independently testable. The decision favours one auditable, coroutine-compatible implementation
and accepts responsibility for the solver engineering required by Spack-grade behaviour.

## Verification

`tests/resolve/test_resolution.cpp` pins parser round trips and error columns plus canonical
package and concretised-record round trips. `tests/headers/resolve.cpp` compiles the public header
in isolation, and the installed consumer receives it through `liquid/liquid.hpp`.

## Open questions

- Measurements and the complete solver verification are recorded with the solver implementation.
- Package-repository seeding remains issue #75.
