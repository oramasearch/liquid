# Dependency resolution

> Research: [09-package-managers](../research/09-package-managers/)
>
> **Position:** Spack's resolution model is the best in existence and is the model
> liquid follows. Nix's store model is the other half.
>
> **Status:** issue #27 implements `abi/1` extraction, stored descriptors, deterministic
> compatibility reports, and signed comparisons for built shared libraries. Solver and package
> consumers remain later work.

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

A declarative constraint solver over **Answer Set Programming**, following Spack's
move from a hand-written greedy resolver to Clingo.

Why ASP rather than SAT or PubGrub:

- The problem is genuinely **multi-dimensional** — versions, variants, compilers,
  targets, virtual providers — not boolean satisfaction with a version axis.
- It **optimises rather than merely satisfies**. "Any valid solution" is a bad answer
  when one requires rebuilding 400 packages and another requires none.
- The **model is separable from the solver**. Resolution rules live in ASP where they
  can be audited and extended, not buried in imperative code.
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

- **Concretisation latency.** Spack's solve can take minutes on large environments.
  liquid inherits that risk and will need incremental re-solving.
- **Whether to vendor Clingo or implement the solver.** Clingo is proven; a
  dependency on it is a portability constraint.
- **How to seed the package repository.** A resolver with nothing to resolve is
  useless; importing from Spack, nixpkgs or conda-forge is the likely bootstrap.
