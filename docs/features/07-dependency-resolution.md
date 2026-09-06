# Dependency resolution

> Research: [09-package-managers](../research/09-package-managers/)
>
> **Position:** Spack's resolution model is the best in existence and is the model
> liquid follows. Nix's store model is the other half.
>
> **Status:** implemented by issue #25: the public spec parser, canonical package and
> concretised-spec records, package-universe loading, CDCL concretizer, lexicographic optimisation,
> minimal attributed conflict cores, explanations, and `liquid resolve`. Issue #27 supplies the ABI
> compatibility facts accepted by `resolve_options`.

## A dependency is a configured build, not a version

The insight liquid takes from Spack: in native and scientific software, the same
library at the same version built with a different compiler, a different MPI, or
different optional features is a **different artifact** that may or may not be
compatible with its siblings. Resolution must model that, not paper over it.

## The spec language

liquid adopts Spack-style specs, because they are compact, precise and already
familiar to the people with the hardest dependency problems:

```
hdf5 @1.14.3 +mpi ~fortran %gcc@13.2 ^openmpi@5 target=zen4
```

- `@` version constraints
- `+` / `~` boolean variants
- `%` **compiler as a dependency**
- `^` transitive dependency constraints
- `target=` microarchitecture

## The solver

A declarative package encoding over liquid's own **conflict-driven clause-learning solver**,
following the capabilities of Spack's move from a hand-written greedy resolver to Clingo without
vendoring Clingo.

Why an ASP-inspired declarative model rather than a greedy resolver or PubGrub alone:

- The problem is genuinely **multi-dimensional** — versions, variants, compilers, targets and
  virtual providers. Liquid grounds those dimensions to Boolean clauses without reducing the
  public model to a version-only vocabulary.
- It **optimises rather than merely satisfies**. "Any valid solution" is a bad answer
  when one requires rebuilding 400 packages and another requires none.
- The **model is separable from the solver**. Resolution rules compile to clauses through a
  private concretizer interface, keeping package policy auditable independently of CDCL search.
- **Unsatisfiable cores** give real explanations.

### Optimisation objective

liquid optimises for **minimum work**, which existing tools cannot do because they do
not know what is already built:

1. Prefer specs already present in the local or remote cache.
2. Prefer specs whose content-addressed outputs are known to be identical.
3. Prefer newer versions.
4. Minimise deviation from declared defaults.

Because outputs are content-addressed ([01](01-object-model-and-storage.md)), the
solver can prefer a configuration that produces *byte-identical* results to something
already built, even if the inputs differ.

## Virtual dependencies

Abstract interfaces (`mpi`, `blas`, `lapack`, `libc`) with multiple concrete
providers, resolved during concretisation. One build description works across
OpenMPI, MPICH and a vendor MPI.

## Error messages

Disproportionate investment here, deliberately. "Cannot satisfy constraints" with no
explanation is the most common reason people abandon a package manager.

liquid reports the **unsatisfiable core** — the minimal conflicting set — narrated in
PubGrub style: which constraint came from where, and what would have to change.

## Content-addressed outputs

**Decided from day one, not retrofitted.** Nix has spent years unable to migrate an
input-addressed world to content-addressed derivations (RFC 062), and its mass-rebuild
problem is the direct consequence.

In liquid, a rebuild producing identical bytes terminates the cascade. A glibc patch
that changes nothing observable rebuilds nothing downstream.

This requires reproducibility as a hard, enforced property
([10](10-supply-chain.md)) and makes "which bytes should this input produce?" a
claim someone vouches for — solved by signed attestations and independent rebuild
agreement.

## The granularity fix

**A package is not the unit of rebuild.** This is liquid's single biggest practical
advantage over both Nix and Spack, and it follows from unifying the build system and
the package manager rather than layering one on the other.

Editing one file in a large package recompiles one object
([04](04-build-engine.md)), not the package. Nix and Spack cannot do this because a
derivation is atomic: change one source file in a two-hour build and you pay two
hours again.

## Sandboxing

Sandboxed by default, like Nix and unlike Spack. Reproducibility is unachievable when
the host environment leaks in, and caching, substitution and provenance all depend on
it.

## ABI as a checkable property

liquid extracts the ABI surface of every library it builds and diffs it against the
previous version. That answers, mechanically, whether a rebuilt library can substitute
for an installed one — the question underneath soname bumps, `Breaks` relationships,
and safe content-addressed substitution.

One extraction, four consumers: resolution, packaging ([08](08-packaging-and-images.md)),
substitution, and hot-update classification ([14](14-hot-update.md)).

## Relocation

Solved by construction, not by patching binaries afterwards. Spack's RPATH rewriting
is fragile — embedded paths survive in scripts, `.pc` files and hard-coded strings.
liquid either builds position-independent-by-construction artifacts or uses a fixed
content-addressed store path, and decides this before building rather than after.

## Configuration language

Not a new lazy functional DSL. The Nix language is the most-cited reason people
bounce off Nix. liquid uses a small, typed, declarative format with real error
messages, and the escape hatch is an ordinary program rather than a language feature.

## Open questions

- ~~**Concretisation latency.**~~ Measured on the generated 500-package fan-in fixture on an Apple
  Silicon macOS host: 80 ms cold and 81 ms for a one-root solve with all prior concretisations
  offered for reuse, at load averages `{ 36.85 42.59 47.28 }`. The implementation currently
  grounds each call independently; [`resolution.md`](../library/resolution.md) records the method.
- ~~**Whether to vendor Clingo or implement the solver.**~~ **Resolved by Steve on 2026-09-05
  (liquid-bridge decision 20): implement liquid's own CDCL solver behind the internal concretizer
  interface; do not vendor Clingo.**
- **How to seed the package repository.** A resolver with nothing to resolve is
  useless; importing from Spack, nixpkgs or conda-forge is the likely bootstrap.
