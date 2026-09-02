# Package managers

> Evidence: [`sources.md`](sources.md)
>
> **Steve's position:** Spack's dependency management and resolution are
> exceptionally good and are the model to beat. Nix is reputed to be good but he
> has not used it. This document takes that seriously: Spack's solver is treated
> as the reference design, and Nix is assessed on what it actually provides.

## Spack: why the resolution model is the right one

Spack's core insight is that in native/scientific software, **a dependency is not a
version — it is a configured build**. The same library at the same version, built
with a different compiler, a different MPI implementation, or a different set of
optional features, is a *different artifact* that may or may not be ABI-compatible
with its siblings. Spack makes that explicit and then solves it.

**The spec language** is the user-facing part, and it is genuinely excellent:

```
hdf5 @1.14.3 +mpi ~fortran %gcc@13.2 ^openmpi@5 target=zen4
```

Version constraint, boolean variants, compiler *as a dependency*, transitive
dependency constraints (`^`), and microarchitecture — in one string. Almost
nothing else lets a user express the actual shape of a scientific build this
compactly.

**The concretiser** is the part liquid should study hardest. Spack moved from a
hand-written greedy resolver to **Answer Set Programming** (Clingo/potassco). The
package repository is compiled into ASP facts; the rules of the dependency
model — version compatibility, variant propagation, virtual providers, compiler
constraints, target compatibility — are declarative ASP rules; and Clingo solves
for a stable model with an optimisation objective (prefer newer versions, prefer
preinstalled specs, minimise deviation from defaults).

Why this matters more than SAT/PubGrub:

- **It handles a multi-dimensional problem natively.** Version solving is only one
  dimension; variants, compilers, targets and virtual providers are others. SAT
  solvers can encode this, but ASP expresses it directly.
- **It optimises rather than merely satisfying.** "Any valid solution" is a poor
  answer when some solutions require rebuilding 400 packages and others require
  none. Optimisation criteria let the solver prefer what is already built.
- **The model is declarative and separable from the solver.** The rules of
  dependency resolution live in ASP, not in imperative resolver code — so they can
  be audited, extended and reasoned about.
- **Unsatisfiable cores give real error messages.** The solver can say *which
  constraints conflict*, which is the single biggest usability failure of most
  dependency resolvers.

**Virtual dependencies** (`mpi`, `blas`, `lapack`) are the other feature worth
stealing: an abstract interface that multiple concrete packages provide, resolved
during concretisation. This is how one build description works across OpenMPI,
MPICH and a vendor MPI.

**What HPC users complain about:**

1. **Concretisation is slow** on large environments — the solve itself can take
   minutes.
2. **Combinatorial explosion.** The variant space means binary caches miss
   constantly; your exact spec has never been built by anyone.
3. **Long builds** follow directly from cache misses.
4. **Binary relocation is fragile.** Spack patches RPATHs to relocate a binary
   built for one prefix into another, which works until it does not (embedded
   paths in scripts, `.pc` files, hard-coded strings).
5. **Reproducibility is weaker than Nix's** — builds are not sandboxed by default,
   so the host leaks in.

## Nix: the store model, honestly assessed

**What Nix gets right, and it is fundamental:**

- **The store.** `/nix/store/<hash>-name` where the hash covers the complete build
  inputs. Two builds with different inputs cannot collide, so multiple versions
  and configurations coexist by construction. This is the same content-addressing
  insight as Git's, applied to built artifacts.
- **Derivations as data.** A `.drv` is a serialised, hashable description of a
  build action. Everything else — caching, distribution, remote building — follows.
- **Sandboxed builds.** Network off, filesystem restricted, environment fixed. This
  is why Nix's reproducibility is materially better than Spack's.
- **Atomic upgrades and rollbacks.** A profile is a symlink; switching is atomic
  and reversible. NixOS extends this to the whole system.
- **Dev shells.** `nix develop` giving an exact toolchain per project is the
  feature that wins individual developers over.

**What people actually hate:**

1. **The language.** Untyped, lazy, with error messages that report a failure far
   from its cause. Universally cited as the barrier to entry.
2. **Documentation.** Long-standing, widely acknowledged weakness.
3. **The mass-rebuild problem.** Because derivations are **input-addressed**, a
   change to glibc changes its store path, which changes every dependent's inputs,
   which changes their paths — *even if every rebuilt artifact is byte-identical*.
   The entire package set rebuilds. `staging-next` exists purely to manage this.
   **This is Steve's "build the world" complaint in its purest form.**
4. **No incrementality inside a derivation.** A derivation is atomic: change one
   source file in a 2-hour build and you pay 2 hours again. Nix has excellent
   coarse-grained caching and *no* fine-grained incrementality.
5. **Closure sizes and disk usage.** A closure drags in everything transitively.
6. **`stdenv` is opaque.** Debugging a build failure means reading generated bash.
7. **Flakes.** Genuinely better UX, still nominally experimental years on, and the
   surrounding governance conflict is itself a reason people hesitate.
8. **IFD (import-from-derivation)** breaks evaluation-time/build-time separation
   and is a common performance cliff.

**Content-addressed derivations (RFC 062)** are Nix's own answer to the mass
rebuild: address a derivation by the hash of its *output*, so an input change that
produces identical output terminates the cascade. It is the right fix. It has been
partially implemented and non-default for years, because retrofitting it onto an
input-addressed world is extremely hard.

**Guix** is Nix's model with Scheme instead of a bespoke DSL — a real improvement
in language quality — plus the full-source bootstrap, which is the strongest
existing answer to the trusting-trust problem. Grafts let it patch a dependency
without a full rebuild, an explicit acknowledgement of the mass-rebuild pain.

## SAT, ASP, and PubGrub

- **libsolv** (rpm/dnf, conda) — SAT-based, mature, fast, with weak error messages.
- **PubGrub** (Dart, uv, Cargo-adjacent) — version solving with *explainable*
  conflicts, and its core contribution is precisely that: when it fails, it can
  narrate why in terms a human understands.
- **ASP/Clingo** (Spack) — the most expressive, because the problem genuinely is
  multi-dimensional optimisation rather than boolean satisfaction.

The trend across all of them is that **the quality of the failure message is as
important as the speed of the success path.**

## The tension liquid must resolve

Input-addressing and content-addressing pull in opposite directions:

| | Input-addressed (Nix today, Spack) | Content-addressed (Nix CA, Bazel outputs) |
|---|---|---|
| Path known before build | Yes — you can plan the whole graph up front | No — must build to learn the output hash |
| Early cutoff | **No** — this causes mass rebuilds | **Yes** — identical output stops the cascade |
| Trust model | The path *is* the provenance claim | Requires signatures or rebuild agreement to know *who* produced these bytes |
| Substitution | Trivial | Needs a mapping from input hash → output hash, and that mapping is a trust statement |

Content addressing is clearly correct for avoiding rebuilds. Its cost is that
"which bytes should this input produce?" becomes a claim someone must vouch for —
which is exactly the problem
[13-supply-chain-security](../13-supply-chain-security/) solves with signed
attestations and reproducibility.

## Implications for liquid

1. **Take Spack's resolution model wholesale, and improve on it.** An ASP or
   equivalently expressive constraint solver, with variants, virtual providers,
   compilers-as-dependencies, and target microarchitecture as first-class
   dimensions. Optimise for "minimise rebuilds and maximise cache reuse," not just
   satisfiability. Steve's judgement that Spack is exceptionally good here matches
   the evidence; this is the least risky major design decision in the project.

2. **Adopt Spack's spec syntax, or something very close.** It is compact, precise
   and already familiar to the HPC community. Do not invent a worse one.

3. **Invest disproportionately in solver error messages.** Unsat cores plus
   PubGrub-style narration. "Cannot satisfy" with no explanation is the most
   common reason people abandon a package manager.

4. **Content-address outputs from day one.** Do not repeat Nix's mistake of
   building an input-addressed world and retrofitting CA. This is the direct fix
   for "build the world" and it must be the default, not an opt-in. It requires
   reproducibility as a hard requirement
   ([13](../13-supply-chain-security/)).

5. **Fix the granularity failure both Nix and Spack share.** A package must not be
   the unit of rebuild. liquid's actions are per-compile or finer
   ([04](../04-build-systems/), [08](../08-incremental-compilation/)), so editing
   one file in a large package rebuilds one object, not the package. **This is the
   single biggest practical advantage liquid can have over both Nix and Spack**,
   and it follows from unifying the build system and the package manager rather
   than layering one on the other.

6. **Sandbox by default, like Nix, not like Spack.** Reproducibility is not
   achievable when the host environment leaks in, and everything downstream —
   caching, distribution, provenance — depends on it.

7. **Do not invent a new configuration language if it can be avoided.** The Nix
   language is the most-cited reason people bounce off Nix. Prefer a small,
   typed, declarative format with real error messages, and make the escape hatch
   an ordinary program rather than a lazy functional DSL.

8. **Solve relocation properly.** Spack's RPATH patching is fragile. liquid should
   either build position-independent-by-construction artifacts, or own the
   relocation problem with a real model of embedded paths — not `sed` over
   binaries.
