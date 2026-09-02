<!-- Raw research source. Generated via Perplexity `perplexity_research` (web-grounded,
     multi-source) on 2026-09-01 for the liquid project. Lightly reformatted only.
     Treat as evidence, not as liquid design decisions - see README.md in this
     directory for the curated analysis and the implications drawn for liquid. -->

# Deep research report: functional/reproducible and scientific package managers, 2024–2026

**Status date: 2 September 2026.**

## Executive comparison

| System | Primary goal | Identity / store model | Resolver / evaluator | Binary strategy | Main strength | Main recurring cost |
|---|---|---|---|---|---|---|
| **Nix / NixOS** | Functional package and whole-system management | Normally input-addressed `/nix/store/<hash>-<name>` objects; experimental content-addressed derivations | Lazy, dynamically typed Nix language; derivation graph evaluation | Signed binary substitutes, chiefly `cache.nixos.org`, Cachix, Attic | Exact dependency graphs, atomic generations, rollback, reproducible dev and OS configurations | Steep conceptual stack; poor diagnostics; large closures and mass rebuilds |
| **GNU Guix** | Functional package/system management with inspectability and software-freedom emphasis | Input-addressed `/gnu/store/<hash>-<name>` store, Nix-compatible low-level model | Guile Scheme package and system APIs | Substitutes, reproducible-build verification; grafts for urgent updates | Scheme extensibility and exceptionally small, source-auditable bootstrap | Smaller ecosystem; full-distribution rebuild pressure remains, partly mitigated by grafts |
| **Spack** | Source-oriented HPC/scientific software deployment across compilers, CPUs and variants | Hash identifies a fully concrete build DAG/spec | ASP/Clingo concretizer | Relocatable build caches plus ABI-aware splicing | Models compiler, MPI, GPU, architecture and build-option diversity that ordinary package managers avoid | Combinatorial solving, long source builds and low cache hit rates for unusual specs |
| **Conda** | Cross-platform binary scientific environments | Prefix environment populated from exact platform/build packages | Modern Conda defaults to libmamba/libsolv; classic solver is SAT-based | Channel-hosted prebuilt packages | Broad scientific binary ecosystem and approachable environments | Channel/build compatibility, ABI mixing and environment-solving complexity |
| **mamba / micromamba** | Faster Conda-compatible environment management | Conda package/prefix model | libsolv backtracking SAT solver | Conda channels | Fast metadata handling and solving | Fundamentally inherits Conda metadata and binary-compatibility limitations |
| **Pixi** | Project-centric, locked Conda-and-PyPI environments | `pixi.toml` plus multi-platform `pixi.lock` | Conda: rattler/resolvo; PyPI: uv/PubGrub | Conda binaries plus Python wheels/sdists | Fast, lockfile-first workflow spanning Conda and PyPI | Two-stage cross-ecosystem solve and weaker build provenance than Nix/Guix |

---

# 1. Nix in depth

## 1.1 The store model

A normal store object has a path resembling:

```text
/nix/store/7h0v6s4i7j2k...-hello-2.12.1
```

The suffix is descriptive but not the identity. The hash prefix separates objects built from different dependency graphs, options or recipes, allowing several versions and configurations to coexist without overwriting one another.

The important qualification is that **ordinary Nix outputs are input-addressed, not truly content-addressed**:

```text
output-path ≈ H(normalized derivation + transitive build inputs) + name
```

Conceptually, the address commits to:

- source store objects;
- builder executable and arguments;
- environment variables;
- target system;
- dependency derivations and selected outputs;
- patches, flags and other recipe data.

Consequences:

1. Nix can predict an ordinary output path before building it.
2. Changing any hashed input normally changes the output path, even if the resulting bytes would be identical.
3. Different derivations may therefore produce identical contents at different paths.
4. Conversely, the path alone does not prove that the bytes were honestly produced by the claimed derivation; importing such an output requires a trusted signature or a local build.

The store database records references between objects. The **closure** of a root is that object plus every transitively referenced store object. It is the closure—not just the package directory—that must be transferred to run the package elsewhere and that determines its real deployment footprint.

## 1.2 Derivations and `.drv` files

A derivation is a low-level build action, conventionally materialized as:

```text
/nix/store/<hash>-hello-2.12.1.drv
```

A derivation records:

- its name and output specifications;
- target `system`, such as `x86_64-linux`;
- builder executable;
- argument vector;
- environment;
- all declared input store objects and dependency outputs.

The canonical representation has traditionally been the ATerm format, although a JSON representation is under development. A derivation can have multiple outputs, such as `out`, `dev`, `doc` and `debug`. A reference such as:

```text
/nix/store/...-firefox.drv^out
```

means the `out` output of that derivation. This output-reference form is especially important when the eventual path is not yet known, as with content-addressed derivations. [web:391]

A useful distinction is:

```text
Nix expression
    ↓ evaluation
derivation (.drv)
    ↓ realization
store output(s)
```

Evaluation creates a build plan; realization executes that plan or substitutes matching outputs from a cache.

### Reproducibility is not automatic

Input addressing guarantees that the *same declared build plan* gets the same expected path. It does not, by itself, guarantee bit-identical output. Builds can still depend accidentally on timestamps, randomness, network access, CPU behavior or undeclared host state. Rebuilding and comparing NAR hashes is how reproducibility is tested.

## 1.3 Input-addressed versus content-addressed derivations

### Input-addressed derivation

For an ordinary derivation, downstream paths depend recursively on upstream derivation identities:

```text
glibc recipe changes
  → glibc output path changes
  → every dependent derivation changes
  → downstream output paths change
```

This is powerful because the entire graph can be planned before building. It also creates the mass-rebuild problem.

### Content-addressed derivation

RFC 0062 adds opt-in **content-addressed derivations**, currently behind the `ca-derivations` experimental feature. Their final output path is based on the output’s actual contents, so it is not known before the build. [web:246]

RFC 0062 introduces a separation between:

- a **derivation-output identity**, conceptually `(derivation hash, output name)`;
- the actual content-addressed store path;
- a **realisation** mapping the former to the latter.

The rough process is:

```text
1. Realize dependency outputs.
2. Replace symbolic dependency-output references with their actual CA paths.
3. Form a resolved derivation.
4. Check whether this resolved derivation was already realized.
5. If not, build into scratch paths.
6. Hash and rewrite outputs into final content-addressed paths.
7. Register the realization mapping.
```

RFC 0062 deliberately lets input- and content-addressed derivations coexist. It also has to handle self-references: binaries or text often contain their own `/nix/store/...` prefix. The initial design rewrites detectable textual self-references through a temporary “magic” hash; references hidden inside compressed archives or similar formats remain harder. [web:392]

### Early cutoff

The major graph-level benefit is **early cutoff**. Suppose source input changes but a compiler produces exactly the same library:

```text
source A → library bytes X → application
source B → library bytes X → application
```

Under input addressing, `A` and `B` imply different library paths, so the application rebuilds. Under content addressing, both produce the same content path; once Nix observes that equality, downstream work can stop. This is particularly attractive for generated code, metadata-only source changes and intermediate compiler stages. [web:251]

It does not eliminate genuine world rebuilds: if a glibc upgrade actually changes its bytes or ABI, dependants still need rebuilding or an explicitly justified ABI-reuse mechanism.

### Trust trade-off

| Property | Input-addressed output | Content-addressed output |
|---|---|---|
| Final path known before build | Yes | No |
| Path verifies downloaded bytes | No | Yes |
| Same bytes from different recipes deduplicate by identity | Usually no | Yes |
| Needs trusted mapping from recipe to output | Signed cache result | Signed realisation |
| Supports straightforward up-front graph planning | Yes | More complicated |
| Enables output-equivalence early cutoff | No | Yes |

For input-addressed substitutes, a cache signs the association between the expected store path and the serialized NAR content. For a CA object, anyone can verify that its bytes match its path, but clients still need to trust the claim that **this content is the output of that derivation**. RFC 0062 therefore signs realisations. CA moves trust from “are these the bytes named by this path?” to “is this the correct path for this build specification?” [web:250][web:252]

## 1.4 The Nix language

Nix is a small, pure, expression-oriented, **lazy and dynamically typed** language. Values are computed only when demanded, and type errors appear only on evaluated paths. [web:333]

That design supports:

- cheap composition of enormous package sets;
- recursive attribute sets;
- higher-order package functions and overrides;
- generating derivation graphs without performing the builds;
- module systems such as NixOS and Home Manager.

But the same properties explain much of its reputation.

### Recurring criticisms

1. **No static type system.** Misspelled attributes and wrong-shaped values may remain latent until a distant consumer forces them.
2. **Laziness obscures causality.** The expression where an invalid value was created can be far from the expression that finally forces it.
3. **Error traces expose framework internals.** Errors may traverse module merging, overlays, fixed points and `lib` helpers rather than point directly to the user’s declaration.
4. **Several languages must be understood together.** Users encounter Nix expressions, NixOS modules, the nixpkgs package DSL, Bash-based `stdenv`, flakes and the CLI.
5. **Documentation is distributed across references, tutorials, the NixOS manual, nixpkgs manual, wiki, source and Discourse.**

These are not merely anecdotal. In the 2025 community survey, with 3,399 complete responses, the leading improvement requests were flakes at **50.3%**, error messages at **47.6%**, and the reference manual at **41.5%**. Flakes were nevertheless used by **78.9%** of respondents. [web:376][web:377]

## 1.5 `stdenv`: useful abstraction, opaque machinery

Most nixpkgs packages use `stdenv.mkDerivation`. The standard environment provides a Bash driver with phases such as:

```text
unpackPhase
patchPhase
configurePhase
buildPhase
checkPhase
installPhase
fixupPhase
installCheckPhase
```

Dependencies can install **setup hooks** that mutate the build environment or register phase hooks. Merely adding CMake, Meson, Python or a compiler wrapper to `nativeBuildInputs` can therefore alter phase behavior, flags, search paths or fixups. The official documentation describes hooks that set variables, source files and run before or after phases. [web:361][web:362]

Users appreciate the concise recipes this enables, but call it opaque because effective behavior emerges from:

- `genericBuild`;
- sourced Bash;
- dependency traversal order;
- setup hooks;
- compiler and linker wrappers;
- automatic patching and fixup phases;
- platform-specific hooks.

A 2024 bug report, for example, found setup hooks could execute more than once inconsistently depending on dependency-graph traversal order. [web:371]

## 1.6 Flakes and the flakes controversy

A flake supplies:

```text
flake.nix   — declared inputs and standardized output attributes
flake.lock  — pinned transitive input revisions
```

Typical outputs include packages, apps, development shells, NixOS configurations, overlays and modules. Flakes default toward pure evaluation and make repository-based inputs and lock files convenient.

### What flakes solve

- a standard project entry point;
- explicit, lockable external inputs;
- a common CLI for `nix build`, `nix run` and `nix develop`;
- portable references such as `github:NixOS/nixpkgs#hello`;
- evaluation caching and more hermetic source handling.

### Why they remain controversial

As of September 2026, upstream still marks flakes experimental, despite their de facto dominance. The stabilization milestone has had no due date, and official documentation still lists outstanding design and implementation issues. [web:121][web:125]

The controversy has several layers:

1. **Process:** the original RFC was withdrawn and implementation merged experimentally without normal RFC acceptance, leading some contributors to reject the governance precedent. [web:226][web:240]
2. **Bundling:** source locking, registries, pure evaluation, schema conventions, evaluation caching and a new CLI arrived as one large mechanism.
3. **Git semantics:** untracked files are invisible, so beginners frequently ask why a new file is absent until `git add`; the complete flake source tree is copied to the store, which is expensive for large repositories.
4. **Input graphs:** transitive lock files can introduce duplicate nixpkgs revisions and eager fetching. `follows` reduces duplication but becomes boilerplate and is difficult for downstream users to impose on hidden indirect inputs.
5. **Registries:** indirect references can resolve differently across machines and have historically contaminated lock files with local mappings.
6. **Composability:** critics want pure evaluation, locking and caching independently available rather than conditional on adopting the flake schema.
7. **Compatibility trap:** so many projects rely on flakes that changing the design is now risky.

The official flakes documentation itself notes whole-directory copying, Git staging requirements, eager dependencies, duplicated or stale transitive inputs, and unresolved versioning, composability and cross-compilation concerns. [web:393]

**Practical 2026 assessment:** flakes are operationally mainstream but institutionally experimental. New projects commonly use them, while reusable Nix libraries often keep ordinary `.nix` entry points and expose a thin flake wrapper.

## 1.7 Binary caches

Nix calls prebuilt-store-object servers **substituters**.

### `cache.nixos.org`

The default public cache serves builds produced for official Nixpkgs and NixOS. Nix checks it before building a missing path. It does not normally contain private software, arbitrary overlays or locally patched packages. [web:1][web:4]

A cache supplies:

- `.narinfo` metadata;
- compressed NAR archives;
- references and closure metadata;
- hashes and sizes;
- cryptographic signatures.

Because ordinary paths are input-addressed, the trusted signature is critical: the path itself cannot validate the content.

### Cachix

Cachix is hosted Nix-cache infrastructure, widely used for project CI, community overlays and private software. `cachix use <name>` installs the substituter and trusted key; CI can push completed store paths. [web:6]

### Attic

Attic is a self-hosted, multi-tenant cache, commonly backed by S3-compatible storage. It supports global NAR/chunk deduplication, retention policies and garbage collection; `attic watch-store` can upload paths as they appear. [web:5][web:9]

### Cache economics

Cache hits require the exact expected input-addressed path. Changing a compiler, glibc, patch, build flag, nixpkgs revision or recipe can generate a new path even where output differences are insignificant. This strictness buys unambiguous provenance but lowers reuse across forks and revisions.

## 1.8 The mass-rebuild and `staging-next` problem

Because an ordinary output address commits recursively to its input graph, changing a foundational dependency invalidates every reverse dependency:

```text
glibc
 ├─ compiler runtimes
 ├─ shells and core utilities
 ├─ language runtimes
 └─ almost every dynamically linked program
```

Nixpkgs therefore batches changes rebuilding **500 or more packages** in `staging`. Periodically that batch becomes `staging-next`, where a dedicated Hydra jobset builds it and contributors fix regressions before merging it to `master`. A fundamental glibc change can affect **50,000-plus packages**. [web:394]

This is not merely wasted hashing:

- downstream packages may expose previously hidden portability bugs;
- transient failures obscure real regressions;
- Hydra capacity and cache upload bandwidth are consumed;
- users updating before substitutes finish may build locally;
- several large changes are tested together, complicating attribution.

A 2026 Nixpkgs release campaign explicitly warned that full world rebuilds can reintroduce failures and classified changes with roughly 500 or more rebuilds for staging. [web:115]

### CI split

- **Hydra** evaluates jobsets, dispatches large builds/tests across workers and populates the official cache. It is Nix’s continuous-build service. [web:275]
- **OfBorg** is pull-request-oriented automation: it evaluates changes, estimates rebuild impact, assigns labels and builds selected affected packages. It gives reviewers rapid feedback but cannot economically validate every reverse dependency on every PR.
- **`staging-next` Hydra** performs the expensive integrated validation that PR-level CI cannot.

Nixpkgs’ scale explains the arrangement. A 2025 Repology snapshot counted about **107,958 package entries representing roughly 85,600 projects** in unstable. [web:273]

## 1.9 What users love

| Capability users value | Technical reason |
|---|---|
| **Reproducible environments** | Dependency versions, recipes and transitive inputs become explicit graph nodes rather than ambient machine state. |
| **Atomic upgrades and rollback** | New store objects and profile generations are created without overwriting old ones; switching a symlink or boot generation activates the new state. NixOS can revert with `nixos-rebuild --rollback switch`. [web:395][web:401] |
| **Development shells** | `nix develop` or `nix-shell` exposes nearly the same tools and environment as a package build without globally installing them; declarative shell definitions can be version-controlled. [web:358][web:400] |
| **Side-by-side versions** | Distinct store paths avoid global filename ownership conflicts. |
| **NixOS** | One declarative configuration describes packages, kernel, services, users and many generated configuration files; generations support testing before activation and boot-time rollback. |
| **Deployability** | A closure can be copied to another compatible Nix store without recreating an imperative installation history. |
| **Composable package overrides** | Functions, overlays and module options make large-scale customization possible without maintaining traditional downstream patches for every package. |

## 1.10 What users hate

| Complaint | Underlying mechanism / assessment |
|---|---|
| **Learning curve** | Users must learn the store model, derivations, Nix syntax, nixpkgs conventions, modules, `stdenv`, profiles and often flakes simultaneously. |
| **Error messages** | Dynamic typing, laziness and module fixed points delay failures and produce traces through generic library code. The 2025 survey made errors the second-highest improvement request. [web:376] |
| **Documentation gaps** | Conceptual, language, CLI, packaging, NixOS and flakes documentation evolved separately; survey respondents ranked the reference manual third among desired improvements. [web:377] |
| **Disk usage** | Side-by-side dependency versions, old generations, build outputs and GC roots retain whole closures. A 2024 user reported a rebuild exhausting all available space; recommended recovery included deleting generations, GC, checking roots and ensuring build temporary space. [web:429] |
| **Large closure sizes** | Store-path references pull complete transitive runtime closures. Accidental references, unsplit outputs or toolchain leakage can make containers and deployments unexpectedly large. |
| **No transparent incremental build inside one derivation** | Hermetic builds cannot reuse a previous mutable build directory. Official nixpkgs documentation says state cannot normally carry between builds; checkpoint builds must make prior state an explicit derivation output. [web:395] |
| **`stdenv` opacity** | Build phases and setup hooks are Bash conventions whose behavior changes with dependencies, wrappers and platform hooks. |
| **IFD** | Import-from-derivation pauses sequential evaluation, realizes an output, reads it, then resumes. It prevents producing the whole build plan first and serializes discovery of required builds. [web:396] |
| **Cache misses after small changes** | Input-addressing changes the expected path whenever a hashed input changes, even if final bytes would have matched. |
| **Mass rebuilds** | Hash changes propagate through the reverse-dependency graph; foundational updates therefore invalidate tens of thousands of paths. |

### Store maintenance

Garbage collection begins at profiles and other **GC roots**, marks every transitively referenced path live, and deletes unreachable paths. Old generations remain roots until removed. [web:394]

Nix also supports file-level deduplication:

```console
nix-store --optimise
```

It replaces files with identical NAR serialization by hard links and is documented as commonly saving around **25–35%**. [web:211] This is useful but distinct from derivation-level content addressing: two differently addressed outputs remain separate store objects even if many files are hard-linked.

---

# 2. GNU Guix

Guix inherits the functional-store architecture but intentionally replaces most user-facing Nix-language machinery with **Guile Scheme**.

Packages and systems are native Scheme values and modules:

```scheme
(package
  (name "example")
  (version "1.0")
  (source ...)
  (build-system gnu-build-system)
  (inputs ...))
```

Its default store is `/gnu/store`; store paths hash build inputs, and derivations are also serialized as `.drv` files. Guix provides transactional upgrades, rollback, per-user profiles and garbage collection. [web:307][web:308][web:309]

## 2.1 Why Scheme matters

Advantages over a dedicated configuration language include:

- mature macro and module systems;
- hygienic code generation;
- debugging and REPL facilities;
- one language for package definitions, services, transformations and tooling;
- less distinction between “configuration syntax” and implementation language.

It does not eliminate complexity—Guix has its own records, monadic interfaces, g-expressions and build-side versus host-side Scheme—but users are learning a general-purpose language rather than an intentionally minimal DSL.

## 2.2 Bootstrappable builds

A functional derivation graph is only as trustworthy as its initial binary seeds. A malicious historical compiler could inject itself into later compilers—the classic “trusting trust” problem—even if all visible source is clean.

Traditional Guix bootstrapping trusted approximately 250 MB of binaries including Guile, GCC, Binutils, libc, shell and utilities. Guix’s **full-source bootstrap**, available for `i686-linux` and `x86_64-linux`, reduces the root to a **357-byte `hex0` seed** whose executable bytes correspond directly to an inspectable hexadecimal source representation. [web:397]

The chain is approximately:

```text
357-byte hex0 seed
 → hex0
 → hex1
 → M0
 → hex2
 → M1
 → mescc-tools
 → M2-Planet
 → Mes Scheme interpreter/C compiler
 → historical C toolchain stages
 → modern GCC/Binutils/glibc
 → normal Guix system
```

Guix additionally developed a **Scheme-only bootstrap** using Gash and Gash Utils in place of Bash, Awk, Coreutils, Grep, Gzip, Sed and Tar. Work remains for ARM, AArch64 and Hurd architectures. [web:397]

This is a different notion of reproducibility from merely locking versions:

- **repeatability** asks whether the same build yields the same bytes;
- **bootstrappability** asks whether every executable in the build ancestry can be derived from inspectable source and a minimal comprehensible seed.

## 2.3 Grafts

Guix has the same fundamental mass-rebuild consequence as Nix: changing libc or Bash normally changes all reverse-dependent store identities. **Grafts** provide a pragmatic exception for urgent security updates.

For a vulnerable package:

```scheme
(define bash
  (package
    ...
    (replacement bash-fixed)))
```

Guix:

1. builds the replacement;
2. copies affected installed packages;
3. rewrites references from the old store path to the replacement;
4. recursively propagates those rewrites toward the requested profile.

This is usually much cheaper than rebuilding the entire dependency chain. The official manual reports grafting an average package as usually taking less than a minute. [web:398]

Limitations reveal what grafting really is:

- replacement name/version strings must currently have equal length because binary files are patched in place;
- shared libraries must retain the same SONAME and be ABI-compatible;
- the resulting object was not rebuilt against the replacement, so grafting is an operational security-update mechanism rather than a proof that the original build derivation produced the rewritten bytes.

Conceptually, Guix grafts anticipate the same question addressed more generally by Spack splicing: **when is it safe to reuse a dependent binary with a different dependency?**

---

# 3. Spack

Spack is designed for HPC installations where “install HDF5” is incomplete. A real request may be:

```text
hdf5@1.14.5 +mpi +fortran ~java
    %gcc@14
    target=zen4
    ^openmpi@5 +cuda cuda_arch=90
```

Its central object is a **spec DAG** containing package versions, variants, compiler/toolchain, operating system, target microarchitecture and dependencies.

## 3.1 Variants and virtual dependencies

Variants are package-specific build choices:

```text
+mpi      enable MPI
~shared   disable shared libraries
cuda_arch=90
```

They may be conditional and may change dependencies, conflicts, generated code or ABI.

**Virtual dependencies** represent interfaces rather than implementations:

```text
depends_on("mpi@3:")
```

Potential providers include Open MPI, MPICH, MVAPICH and vendor MPI implementations. Users or site policy can constrain which provider is selected. Current Spack supports packages providing multiple virtual interfaces and selecting which dependency edge supplies each virtual. [web:189]

This is essential in HPC, where vendor MPI, BLAS/LAPACK, CUDA and ROCm implementations may be performance- or machine-specific.

## 3.2 Compilers as dependencies

Historically, Spack treated the compiler as a special node attribute selected from `compilers.yaml`. Modern Spack 1.x moves compilers toward ordinary **build dependencies**:

- language interfaces such as C, C++ and Fortran are virtuals;
- GCC, Clang and other compiler packages provide them;
- compilers may be external, installed in the Spack store or obtained from a build cache.

The 1.x transition explicitly changed compilers from node attributes to build-only dependencies. [web:185][web:193]

This improves modeling of bootstrapping and mixed toolchains, but increases the dependency problem: the solver is choosing build tools as well as libraries.

## 3.3 From custom concretizer to ASP/Clingo

The original concretizer was a custom greedy resolver. It could be fast for simple specs but could choose incorrectly or fail on satisfiable combinations. Since Spack 0.17, the default concretizer has been based on **Clingo**, an Answer Set Programming solver; the old concretizer was subsequently removed. [web:182][web:188]

### ASP pipeline

The current concretizer:

1. compiles user specs, package declarations, conflicts, variants, reusable installed specs and cache candidates into ASP facts;
2. combines them with rules expressing legal concrete DAGs;
3. asks Clingo for stable models satisfying all constraints;
4. optimizes among valid models;
5. interprets the selected model as a concrete spec graph.

Typical optimization goals include:

- reuse as many already installed or cached specs as possible;
- minimize source builds;
- select preferred or newest satisfying versions;
- satisfy site configuration and provider preferences.

Clingo grounds the logic program and uses techniques related to SAT solving, including conflict-driven clause learning. Spack must solve not only package versions but operating systems, CPU targets, variants, dependency types, virtual providers and reusable binary DAGs. [web:399]

## 3.4 Why concretization becomes slow

The theoretical search space resembles:

```text
versions
× variants
× compiler versions
× CPU targets
× OS/libc combinations
× virtual providers
× dependency versions
× reuse/splice alternatives
```

Not all combinations are explored explicitly—constraints prune heavily—but large unified environments can still be pathological.

A 2026 NASA/HPSF deployment report described:

- more than 270 root specs in one environment as impractical;
- the concretizer slowing to a crawl or hanging;
- a combinatorially explosive constraint problem;
- full source builds taking more than 12 hours;
- build caches reducing iteration to two or three hours. [web:167]

A separate CI operator reported that concretizing more than 100 Spack environments could consume up to **30 minutes of wall time** even with parallelism. [web:169]

Common mitigations are:

- split giant environments;
- use `unify: false` or less strict unification;
- constrain versions, compilers, providers and targets;
- register system packages as externals;
- preserve `spack.lock`;
- enable reuse from installed packages and build caches;
- cache concretization results.

## 3.5 Build caches and binary relocation

A Spack build cache stores complete concrete specs and package archives. Unlike Nix, Spack installations may live under different roots on different systems, so cached binaries must be **relocatable**.

At build time Spack records embedded installation prefixes and relocation targets. At installation it can rewrite:

- ELF RPATHs, using direct patching or `patchelf`;
- Mach-O install names with `install_name_tool`;
- text files;
- interpreter/shebang paths;
- other occurrences of old installation prefixes.

Spack strongly prefers RPATHs so binaries encode their dependency locations rather than relying on ambient `LD_LIBRARY_PATH`. [web:142][web:146]

A hard limitation is available binary-string space. If a package was built under a short prefix and installed under a longer one, the new path may not fit. Spack recommends a padded build prefix, for example:

```yaml
config:
  install_tree:
    root: /opt/spack
    padded_length: 128
```

If relocation is impossible, Spack falls back to a source build. Build-cache metadata is normally GPG-signed and verified before installation. [web:400]

## 3.6 Why cache misses are common

A Spack cache artifact corresponds to a detailed concrete spec, not just a package version. Its compatibility dimensions can include:

- OS and libc;
- target microarchitecture;
- compiler and compiler runtime;
- package version;
- every relevant variant;
- dependency DAG hashes;
- CUDA/ROCm architecture;
- Spack metadata or lock-format generation.

A cache guide summarizes “no binary for spec” as the cache lacking the exact ABI tuple of OS/libc, microarchitecture, compiler and SDK. [web:316] Another deployment guide notes that matching uses the spec hash, which incorporates build flags, architecture and dependency hashes. [web:408]

Thus Spack’s flexibility directly fights cache coverage. A public cache cannot feasibly prebuild the Cartesian product of all variants, CPUs, compilers, GPU architectures and MPI providers.

## 3.7 Splicing and ABI compatibility

**Splicing** redeploys an already-built package with one dependency replaced by another spec providing the same package or virtual.

For example:

```yaml
concretizer:
  splice:
    explicit:
    - target: mpi
      replacement: mpich/abcdef
      transitive: false
```

This can combine a generic cached application with a site-optimized MPI.

Spack distinguishes:

- **transitive splice:** shared-dependency conflicts are resolved in favor of the replacement node’s graph;
- **intransitive splice:** conflicts are resolved in favor of the original root’s graph.

The deployed spec retains a `build_spec` pointer to what was originally built.

### Explicit versus automatic

- **Explicit splicing:** the administrator names the replacement and assumes responsibility for ABI compatibility. Spack may not warn if the replacement is incompatible; the program can simply fail at runtime.
- **Automatic splicing:** packages declare compatibility with `can_splice`; the concretizer searches for ABI-compatible substitutions that maximize reuse of installed or cached specs.

Automatic splicing is still experimental and disabled by default. [web:401]

Splicing increases binary reuse but weakens the simple identity claim “this exact DAG was what produced these bytes.” The redeployed object intentionally has a runtime dependency graph different from its build graph.

---

# 4. Conda, mamba, Pixi and dependency solving

## 4.1 Conda and mamba

A Conda repository contains already-built artifacts distinguished by:

- package name and version;
- build string and build number;
- target subdirectory/platform;
- dependency constraints;
- channel and priority;
- optional track features or mutex packages.

The classic Conda resolver exposes a low-level SAT interface. Modern Conda has used `conda-libmamba-solver` as its default since Conda 23.10. It embeds the same **libsolv-powered** solver used by mamba and micromamba. [web:286][web:287][web:289]

Libsolv treats resolution as a backtracking satisfiability problem: package-version/build candidates become decisions and dependency/conflict rules constrain valid assignments. [web:66]

Compared with Nix:

- Conda solves an environment of compatible binary artifacts.
- Nix evaluates an explicitly programmed build graph and normally does not choose versions through a global SAT solve.
- Conda’s reproducibility primarily comes from explicit package URLs or lock files.
- Nix’s store identity includes the recipe and build-input graph, offering stronger provenance but not automatic bit reproducibility.

## 4.2 Pixi

Pixi adds a project-oriented model around the Conda ecosystem:

```text
pixi.toml
pixi.lock
.pixi/
```

The lock file records solved environments for supported platforms and is updated when the manifest and lock fall out of sync. [web:201]

For mixed Conda/PyPI projects, current Pixi performs:

```text
1. Solve Conda packages with rattler/resolvo.
2. Map those packages to Python package identities with parselmouth.
3. Solve remaining PyPI dependencies with uv’s PubGrub resolver.
```

A package declared in both ecosystems is normally satisfied by the Conda artifact. [web:402]

This is not one global cross-ecosystem solve. The Conda result becomes the base against which PyPI dependencies are resolved, which is practical but can make ecosystem-boundary conflicts harder to explain.

## 4.3 SAT, ASP and PubGrub

These techniques solve related problems but are not interchangeable labels.

| Method | Representation | Search behavior | Package-manager advantage |
|---|---|---|---|
| **SAT / CDCL** | Boolean variables and clauses | Unit propagation, decisions, conflict analysis and learned clauses | Mature high-performance solvers; can prove unsatisfiability |
| **ASP / Clingo** | Facts, rules, constraints, choices and optimization objectives | Grounds rules, then searches stable models using SAT-family methods | Expressive for recursive graph structure and ranked policy goals such as Spack reuse |
| **PubGrub** | Version ranges and explicit incompatibilities | Propagation, version decision, conflict derivation and backjumping | Domain-specific explanations showing why no version set works |

A simple SAT encoding gives each package-version pair a Boolean variable:

```text
A_1.0, A_2.0, B_3.1 ...
```

Constraints then express:

- exactly one selected version per package;
- selecting `A_2.0` implies an allowed version of `B`;
- conflicting builds cannot both be selected;
- root requests must be present.

PubGrub is SAT-like but works directly with package ranges and accumulated **incompatibilities**. When a conflict appears, it derives its root cause, learns an incompatibility and backtracks. The resulting derivation tree enables substantially better “because A requires B<2 while C requires B≥3” diagnostics. [web:153][web:155][web:163]

Resolution remains NP-hard in the worst case. Solver engineering, metadata pruning, preference ordering, lockfile reuse and limiting the candidate universe determine practical performance.

---

# 5. Content-addressed stores: general design lessons

## 5.1 Deduplication

True content addressing maps:

```text
content bytes → cryptographic digest → object identity
```

If two producers generate identical bytes, they share the object automatically. Deduplication can occur at several levels:

1. **Object level:** identical package trees share one object.
2. **File level:** identical files across packages become hard links.
3. **Chunk level:** large archives are divided into chunks and duplicate chunks stored once.
4. **Transport level:** caches transfer only missing objects or chunks.

Nix’s ordinary derivation outputs are input-addressed, but `nix-store --optimise` adds file-level deduplication. Attic adds content-addressed NAR/chunk storage with global deduplication across logical caches. [web:17][web:211]

## 5.2 Garbage collection

Immutable stores do not overwrite upgrades, so they require reachability-based collection:

```text
GC roots
  → profiles / generations / lock roots
  → direct references
  → transitive closure
```

Anything reachable remains live; unreachable objects can be removed. This is safe and conceptually simple, but users often accumulate hidden roots from profiles, development environments, result symlinks and CI.

A key operational trade-off follows:

- retaining generations enables instant rollback;
- retaining generations also pins all their dependency closures.

Deleting files directly from a functional store is unsafe because other objects may contain absolute references to them.

## 5.3 Closure size

The closure of package `P` is:

```text
closure(P) = {P} ∪ closure(each referenced store object)
```

Closure size matters more than the apparent size of `P`:

- deployment transfers the closure;
- containers include it;
- GC cannot delete any member while `P` is rooted;
- an accidental reference to a compiler or source tree can pull gigabytes into runtime deployment.

Store deduplication reduces physical disk use when closures overlap locally, but it does not necessarily reduce:

- logical closure size;
- network transfer to an empty machine;
- container image metadata;
- the number of security-relevant components.

## 5.4 Input addressing versus content addressing

The central tension is between **predictability before execution** and **identity by observed result**.

### Input addressing favors planning

Advantages:

- output paths are available before builds;
- dependency graphs can be fully evaluated and scheduled;
- cache queries are straightforward;
- two builds with intentionally different provenance remain distinct;
- a derivation can embed dependency paths before they exist.

Costs:

- semantically irrelevant input changes produce new paths;
- identical outputs do not automatically converge;
- hash changes propagate and cause mass rebuilds;
- the address does not authenticate output bytes, requiring signed cache metadata.

### Content addressing favors equivalence and verification

Advantages:

- bytes validate their own address;
- identical results deduplicate;
- untrusted storage can serve verifiable objects;
- output-equivalent changes enable early cutoff;
- moving closures between stores needs less trust in the transport.

Costs:

- output paths are unknown until execution or lookup;
- consumers need placeholders and later resolution;
- self-references require rewriting or special hashing;
- non-deterministic builds can produce multiple candidate outputs;
- one still needs a trusted, signed mapping from build request to result;
- provenance and content identity become separate records.

The strongest modern architecture is therefore usually hybrid:

```text
build specification identity
        +
content identity
        +
signed mapping between them
        +
explicit dependency/ABI metadata
```

Nix CA derivations call that mapping a **realisation**. Spack splicing separately records the original `build_spec` and deployed dependency graph. Both systems are converging on the insight that a single hash cannot simultaneously encode recipe provenance, byte identity and every permissible ABI substitution.

---

# 6. Overall assessment for scientific reproducibility

| Requirement | Best fit | Important qualification |
|---|---|---|
| Reproduce an entire Linux service or workstation declaratively | **Nix/NixOS** | Lock nixpkgs/flake inputs and audit impure build behavior; the learning and maintenance cost is significant. |
| Auditable bootstrap from tiny source seed | **Guix** | Full-source bootstrap is strongest on x86 architectures and remains an active project elsewhere. |
| Build optimized HPC stacks across MPI, compilers, CPUs and GPUs | **Spack** | Expect source builds, site configuration and substantial CI/cache investment. |
| Quickly distribute mainstream scientific binaries to users | **Conda/mamba** | Use strict channel policy and lock exact builds for reproducibility. |
| Modern reproducible project environment spanning Conda and PyPI | **Pixi** | Understand that Conda and PyPI are solved sequentially by different resolvers. |
| Maximum artifact deduplication and independently verifiable storage | **Content-addressed store** | Preserve signed provenance mappings; content hashes alone do not prove which recipe produced an object. |

## Bottom line

- **Nix** offers the most complete composition of package building, environment management and declarative operating systems. Its strongest benefits—immutable closures, generations, exact inputs and pure evaluation—also produce its chief costs: path churn, large closures, staging-world rebuilds and a demanding abstraction stack.
- **Guix** keeps the functional model but improves language uniformity and bootstrap auditability. Grafts are an explicit, ABI-constrained escape hatch from functional rebuild propagation.
- **Spack** attacks a harder multidimensional HPC selection problem. ASP makes that problem expressible and optimizable, but cannot remove its combinatorial nature. Relocation and splicing improve reuse at the price of additional ABI reasoning.
- **Conda/mamba/Pixi** optimize binary-environment usability rather than derivation provenance. SAT-family and PubGrub solvers choose compatible published artifacts; lock files, channels and artifact hashes—not a functional build graph—provide most of their reproducibility.
- **Content addressing is not a universal replacement for input addressing.** The durable design is to keep both: recipe identity for planning and provenance, content identity for verification and deduplication, and signed metadata connecting the two.

Citations:
[76] https://nix.dev/manual/nix/2.18/command-ref/new-cli/nix3-store-make-content-addressed
[77] https://nix.dev/manual/nix/2.32/store/derivation/index.html
[78] https://nix.dev/manual/nix/2.35/store/derivation/
[79] https://nix.dev/manual/nix/2.31/store/derivation/
[80] https://nix.dev/manual/nix/2.25/language/derivations
[81] https://nix.dev/manual/nix/2.33/store/derivation/
[82] https://nix.dev/manual/nix/2.30/store/derivation/
[83] https://nix.dev/manual/nix/2.34/store/derivation/outputs/input-address
[84] https://nix.dev/manual/nix/2.34/store/derivation/
[85] https://nix.dev/manual/nix/2.28/store/derivation/
[86] https://nix.dev/manual/nix/2.24/glossary
[87] https://input-output-hk.github.io/haskell.nix/tutorials/ca-derivations.html
[88] https://man.archlinux.org/man/extra/nix/nix-store-realise.1.en
[89] https://nix.dev/manual/nix/2.32/store/derivation/outputs/index.html
[90] https://nix.dev/manual/nix/2.28/store/derivation/outputs/content-address.html
[91] https://lobste.rs/s/psfsfo/curse_nixos
[92] https://news.ycombinator.com/item?id=32378603
[93] https://nomisiv.com/blog/nix-good-and-bad
[94] https://news.ycombinator.com/item?id=30058880
[95] https://blog.wesleyac.com/posts/the-curse-of-nixos
[96] https://www.reddit.com/r/NixOS/comments/1sgrcvl/nix_is_the_worst_designed_language_i_ever_had_to/
[97] https://discourse.nixos.org/t/the-many-issues-plaguing-nix/31458
[98] https://github.com/lambdanil/nix-problems
[99] https://www.reddit.com/r/NixOS/comments/1kh3l4k/nix_language/
[100] https://www.technowizardry.net/2025/11/i-like-the-idea-of-nix-but-dont-enjoy-using-it/
[101] https://snix.dev/docs/components/eval/nix-language-issues/
[102] https://news.ycombinator.com/item?id=41125565
[103] https://news.ycombinator.com/item?id=22878155
[104] https://news.ycombinator.com/item?id=30058727
[105] https://github.com/NixOS/nix-book
[121] https://nix.dev/concepts/flakes.html
[122] https://wiki.nixos.org/wiki/Flakes
[123] https://nix.dev/manual/nix/2.18/contributing/experimental-features
[124] https://nix.dev/manual/nix/2.24/development/experimental-features
[125] https://github.com/NixOS/nix/milestone/27
[126] https://nix.dev/manual/nix/2.29/command-ref/new-cli/nix3-flake.html
[127] https://stacker.news/items/523133
[128] https://nix.dev/manual/nix/2.18/command-ref/new-cli/nix3-flake.html
[129] https://nix.dev/manual/nix/2.24/command-ref/new-cli/nix3-flake
[130] https://discourse.nixos.org/t/discussion-of-flakes-from-2025-nixos-community-survey-report/78853?page=2
[131] https://discourse.nixos.org/t/nix-settings-experimental-features-not-enabling-flakes-nix-command-system-wide/49065
[132] https://zero-to-nix.com/concepts/flakes/
[133] https://discourse.nixos.org/t/discussion-of-flakes-from-2025-nixos-community-survey-report/78853
[134] https://dev.to/arnu515/getting-started-with-nix-and-nix-flakes-mml
[135] https://discourse.nixos.org/t/nix-settings-experimental-features-not-enabling-flakes-nix-command-system-wide/49065/2
[1] https://cache.nixos.org/
[2] https://nixos.wiki/wiki/Binary_Cache
[3] https://wiki.nixos.org/wiki/Binary_Cache
[4] https://docs.cachix.org/what-is-a-binary-cache
[5] https://docs.attic.rs/user-guide/index.html
[6] https://docs.cachix.org/getting-started
[7] https://docs.cachix.org/_sources/getting-started.rst.txt
[8] https://docs.attic.rs/reference/attic-cli.html
[9] https://github.com/zhaofengli/attic/blob/main/README.md
[10] https://nix.dev/manual/nix/2.29/release-notes/rl-1.2
[11] https://nix.dev/manual/nix/2.25/release-notes/rl-1.2
[12] https://zero-to-nix.com/concepts/caching/
[13] https://cachix.org/
[14] https://discourse.nixos.org/t/nix-binary-cache-for-macos-nix-darwin-with-attic/40118
[15] https://docs.attic.rs/tutorial.html
[106] https://toraritte.github.io/saves/Nixpkgs-Manual-Version-20.09.html
[107] https://github.com/NixOS/nixpkgs/blob/master/pkgs/development/libraries/glibc/default.nix
[108] https://nixcademy.com/de/posts/ekala-project/
[109] https://discourse.nixos.org/t/release-process-staging-branches/2799
[110] https://app.semanticdiff.com/gh/NixOS/nixpkgs/commit/5b19532b14a26a7411411a9cd073db367621c636
[111] https://nixos.github.io/release-wiki/Release-Process.html
[112] https://discourse.nixos.org/t/psa-new-branch-staging-nixos/70657
[113] https://github.com/NixOS/nixpkgs/issues/137501
[114] https://github.com/NixOS/nixpkgs/issues/54870
[115] https://github.com/NixOS/nixpkgs/issues/516381
[116] https://github.com/NixOS/nixpkgs/issues/403336
[117] https://github.com/NixOS/nixpkgs/pull/66528
[118] https://github.com/NixOS/nixpkgs/pull/425387
[119] https://git.sr.ht/~andir/nixpkgs/commit/d6b863fd9b7bb962e6f9fdf292419a775e772891
[120] https://git.sr.ht/~andir/nixpkgs/commit/89467818b701581d6a29dbae0e7c4d0fd5fcadda
[31] https://discourse.nixos.org/t/disk-usage-for-nix-store-speaking-of-bloat/74217
[32] https://discourse.nixos.org/t/disk-usage-for-nix-store-speaking-of-bloat/74217?page=2
[33] https://github.com/NixOS/nix/issues/7803
[34] https://github.com/NixOS/nix/issues/8621
[35] https://discourse.nixos.org/t/nix-store-and-rebuild-takes-up-all-available-space/50186
[36] https://github.com/NixOS/nixpkgs/issues/7117
[37] https://wiki.nixos.org/w/index.php?title=Talk:Storage_optimization&mobileaction=toggle_view_desktop
[38] https://fzakaria.com/2021/08/05/the-search-for-a-minimal-nix-shell-continued-mkshellminimal
[39] https://www.reddit.com/r/NixOS/comments/1jcjgk8/disk_usage_is_higher_that_what_it_should/
[40] https://unix.stackexchange.com/questions/665718/how-to-get-the-size-of-a-nix-derivation
[41] https://news.ycombinator.com/item?id=42355690
[42] https://news.ycombinator.com/item?id=42355877
[43] https://discourse.nixos.org/t/closure-size-small-dockertools-image-large/31413
[44] https://nix.dev/manual/nix/2.26/language/import-from-derivation
[45] https://nix.dev/manual/nix/2.31/language/import-from-derivation.html
[46] https://guix.gnu.org/manual/devel/en/html_node/Full_002dSource-Bootstrap.html
[47] https://guix.gnu.org/blog/2023/the-full-source-bootstrap-building-from-source-all-the-way-down/
[48] https://www.gnu.org/software/mes/manual/html_node/Bootstrappable-Builds.html
[49] https://guix.gnu.org/manual/en/html_node/Security-Updates.html
[50] https://www.gnu.org/software/mes/manual/html_node/Full-Source-Bootstrap.html
[51] https://guix.gnu.org/manual/pt-br/html_node/Atualizacoes-de-seguranca.html
[52] https://guix-home.trop.in/Bootstrapping.html
[53] https://manualzz.com/doc/31221077/gnu-guix-reference-manual
[54] https://www.gnu.org/software/mes/manual/html_node/The-Mes-Bootstrap-Process.html
[55] https://guix.gnu.org/eo/blog/2018/a-packaging-tutorial-for-guix/
[56] https://issues.guix.gnu.org/55227
[57] https://lists.gnu.org/archive/html/info-gnu/2019-11/msg00009.html
[58] https://www.gnu.org/software/mes/manual/html_node/Scheme_002donly-Bootsrap.html
[59] https://linuxiac.com/gnu-guix-1-5-brings-plasma-6-5-gnome-46-rootless-mode-and-more/
[60] https://guix.gnu.org/manual/devel/en/html_node/Build-Systems.html
[181] https://spack.readthedocs.io/en/latest/spack.solver.html
[182] https://spack.io/changes-spack-v017/
[183] https://spack.readthedocs.io/en/latest/_modules/spack/concretize.html
[184] https://spack.readthedocs.io/en/latest/environments.html
[185] https://github.com/spack/spack/releases
[186] https://arxiv.org/html/2509.07728
[187] https://spack.readthedocs.io/en/v0.23.1/spack.solver.html
[188] https://www.suse.com/support/update/announcement/2025/suse-fu-20250324-1/
[189] https://spack.readthedocs.io/en/latest/spec_syntax.html
[190] https://spack.readthedocs.io/en/latest/build_settings.html
[191] https://spack.readthedocs.io/en/latest/packages_yaml.html
[192] https://dyninst.github.io/scalable_tools_workshop/petascale2024/assets/slides/spack-compiler-deps-scalable-tools-2024.pdf
[193] https://spack.readthedocs.io/en/latest/configuring_compilers.html
[194] https://spack.github.io/spack-cscs2024/
[195] https://github.com/spack/spack/blob/develop/CHANGELOG.md
[136] https://spack.readthedocs.io/en/latest/binary_caches.html
[137] https://github.com/spack/spack/issues/17041
[138] https://spack.readthedocs.io/en/latest/build_settings.html
[139] https://spack.readthedocs.io/en/v0.23.1/binary_caches.html
[140] https://spack.readthedocs.io/en/v0.23.1/packaging_guide.html
[141] https://spack.readthedocs.io/en/latest/packaging_guide_advanced.html
[142] https://arxiv.org/html/2509.07728
[143] https://github.com/spack/spack/releases
[144] https://github.com/spack/spack/discussions/47648
[145] https://sourceforge.net/projects/spack.mirror/files/v1.0.0/
[146] https://archive.fosdem.org/2018/schedule/event/llnl_spack/attachments/slides/2663/export/events/attachments/llnl_spack/slides/2663/fosdem_spack_binary_packaging.pdf
[147] https://spack.readthedocs.io/en/latest/_modules/spack/relocate.html
[148] https://spack.readthedocs.io/en/latest/spack.html
[149] https://git-ce.rwth-aachen.de/gabriele.inghirami/spack_tuda/-/blob/v0.23.1/lib/spack/docs/build_settings.rst
[150] https://github.com/spack/spack/issues/26552
[166] https://github.com/spack/spack/issues/15040
[167] https://ntrs.nasa.gov/api/citations/20260001679/downloads/HPSF-Spack-Presentation.pdf
[168] https://github.com/spack/spack/issues/3863
[169] https://github.com/spack/spack/issues/43157
[170] https://github.com/spack/spack/labels/concretization
[171] https://spack.readthedocs.io/en/latest/build_settings.html
[172] https://forum.access-hive.org.au/t/questions-for-spack-meeting/3856?page=2
[173] https://github.com/spack/spack/issues/13554
[174] https://archive.fosdem.org/2023/schedule/event/spack_stat_storm/attachments/slides/5970/export/events/attachments/spack_stat_storm/slides/5970/presentation.pdf
[175] https://mug.mvapich.cse.ohio-state.edu/static/media/mug/presentations/21/spack-mug21.pdf
[176] https://dev.to/chadrik/using-spack-to-build-the-vfx-refence-platform-5788
[177] https://github.com/spack/spack/issues/42608
[178] https://spack.io/changes-spack-v017/
[179] https://spack.io/files/spack-rd100-2019-final-with-letters.pdf
[180] https://github.com/spack/spack/issues/27865
[61] https://docs.conda.io/projects/conda/en/latest/dev-guide/deep-dives/solvers.html
[62] https://mamba.readthedocs.io/en/latest/usage/solver.html
[63] https://docs.conda.io/projects/conda/en/stable/dev-guide/api/conda/resolve/index.html
[64] https://docs.conda.io/projects/conda/en/latest/dev-guide/api/conda/resolve/index.html
[65] https://docs.conda.io/projects/conda/en/stable/dev-guide/deep-dives/solvers.html
[66] https://mamba.readthedocs.io/en/latest/advanced_usage/package_resolution.html
[67] https://docs.conda.io/projects/conda/en/stable/dev-guide/api/conda/common/logic/index.html
[68] https://docs.conda.io/projects/conda/en/latest/dev-guide/deep-dives/solver-state.html
[69] https://docs.conda.io/projects/conda/en/4.13.x/dev-guide/deep-dive-solvers.html
[70] https://docs.conda.io/projects/conda/en/latest/dev-guide/specs/solver-state.html
[71] https://docs.conda.io/projects/conda/en/22.9.x/dev-guide/deep-dive-solvers.html
[72] https://docs.conda.io/projects/conda/en/4.14.x/dev-guide/deep-dive-solvers.html
[73] https://www.anaconda.com/docs/getting-started/concepts/what-is-the-solver
[74] https://docs.conda.io/projects/conda/en/stable/dev-guide/plugins/solvers.html
[75] https://fossies.org/linux/conda/conda/resolve.py
[151] https://nesbitt.io/2026/02/06/dependency-resolution-methods.html
[152] https://pubgrub-rs-guide.pages.dev/testing/sat
[153] https://pubgrub-rs-guide.pages.dev/internals/incompatibilities
[154] https://pubgrub-rs-guide.pages.dev/pubgrub_crate/solution
[155] https://pubgrub-rs-guide.pages.dev/internals/overview
[156] https://pubgrub-rs-guide.pages.dev/internals/conflict_resolution
[157] https://pubgrub-rs-guide.pages.dev/internals/intro
[158] https://qiita.com/tmk0308/items/34c0bbf09ae0633b3c5e
[159] https://github.com/dart-lang/pub/blob/master/doc/solver.md
[160] https://pubgrub-rs-guide.netlify.app/pubgrub_crate/custom_dep_provider
[161] https://github.com/astral-sh/uv/blob/main/docs/reference/internals/resolver.md
[162] https://github.com/dart-lang/pub/issues/2089
[163] https://pubgrub-rs.github.io/pubgrub/pubgrub/
[164] https://pubgrub-rs-guide.netlify.app/limitations/prerelease_versions
[165] https://pubgrub-rs-guide.pages.dev/pubgrub_crate/dep_provider
[16] https://gitlab.com/groups/gitlab-org/-/work_items/21037
[17] https://github.com/zhaofengli/attic
[18] https://www.xraph.com/work/trove
[19] https://github.com/zhaofengli/attic/blob/main/README.md
[20] https://input-output-hk.github.io/haskell.nix/tutorials/ca-derivations.html
[21] https://pypi.org/project/content-addressed-store/
[22] https://nix.dev/manual/nix/2.26/store/store-object/content-address
[23] https://discourse.nixos.org/t/nix-2-4-released/15822
[24] https://discourse.nixos.org/t/content-addressed-nix-call-for-testers/12881
[25] https://lib.rs/crates/titor
[26] https://docs.neullabs.com/rninja/architecture/cache/blob-storage/
[27] https://forrestcli.com/context/nix3-store/nix3-store:tldr:478a3
[28] https://deepwiki.com/janbaer/nixos-config/7.4-garbage-collection-and-optimization
[29] https://nix.dev/manual/nix/2.13/command-ref/new-cli/nix3-store-make-content-addressed
[30] https://github.com/AerynOS/os-tools
[196] https://prefix.dev/blog/pypi_support_in_pixi?_rsc=1drc2
[197] https://pixi.prefix.dev/latest/reference/pixi_manifest/
[198] https://prefix.dev/
[199] http://pixi.prefix.dev/v0.67.0/concepts/conda_pypi/
[200] https://pixi.prefix.dev/latest/concepts/conda_pypi/
[201] https://pixi.prefix.dev/latest/workspace/environment/
[202] https://pixi.prefix.dev/v0.63.2/concepts/conda_pypi/
[203] https://github.com/prefix-dev/pixi/issues/175
[204] https://acchapm1.github.io/tutorials/Pixi/pixi-deep-dive
[205] https://prefix.dev/blog/using_python_projects_with_pixi
[206] https://deepwiki.com/prefix-dev/pixi/5.6-build-system-implementation
[207] https://pixi.prefix.dev/dev/concepts/conda_pypi/
[208] https://prefix.dev/blog/introducing_multi_env_pixi
[209] https://prefix-dev-pixi.mintlify.app/concepts/system-requirements
[210] https://deepwiki.com/prefix-dev/pixi-docker/4.3-dependency-management-with-pixi
[241] https://github.com/NixOS/rfcs/blob/master/rfcs/0062-content-addressed-paths.md
[242] https://discourse.nixos.org/t/content-addressed-nix-call-for-testers/12881
[243] https://tweag.io/blog/2021-12-02-nix-cas-4/
[244] https://github.com/NixOS/nix/issues/4087
[245] https://nix.dev/manual/nix/2.18/command-ref/new-cli/nix3-store-make-content-addressed
[246] https://nix.dev/manual/nix/2.34/store/derivation/outputs/content-address.html
[247] https://nix.dev/manual/nix/2.28/store/derivation/outputs/content-address
[248] https://nix.dev/manual/nix/2.29/store/derivation/outputs/content-address.html
[249] https://news.ycombinator.com/item?id=38064308
[250] https://nix.dev/manual/nix/2.26/command-ref/new-cli/nix3-store-make-content-addressed
[251] https://input-output-hk.github.io/haskell.nix/tutorials/ca-derivations.html
[252] https://discourse.nixos.org/t/content-addressed-nix-call-for-testers/12881?page=12
[253] https://github.com/pdtpartners/nix-ninja/blob/main/docs/dynamic-derivations.md
[254] https://releases.nixos.org/nix/nix-2.24.0/manual/glossary.html
[255] https://discourse.nixos.org/t/what-guarantees-do-signatures-by-binary-caches-give/34802/8
[271] https://discourse.nixos.org/t/nixpkgs-has-been-the-largest-repository-for-months/10667
[272] https://nixos.wiki/wiki/Hydra
[273] https://repology.org/repositories/statistics/newest
[274] https://nixmultiverse.com/?view=stats
[275] https://github.com/NixOS/hydra
[276] https://repology.org/repositories/statistics/total
[277] https://repology.org/repositories/statistics/nonunique
[278] https://wiki.nixos.org/wiki/OfBorg
[279] https://nixos.org/blog/announcements/2026/nixos-2605/
[280] https://nix-community.org/continuous-integration/
[281] https://repology.org/repositories/statistics/outdated
[282] https://github.com/NixOS/nixpkgs/blob/master/nixos/modules/services/continuous-integration/hydra/default.nix
[283] https://git.lix.systems/ma27/hydra
[284] https://wiki.nixos.org/w/index.php?title=Hydra/de&mobileaction=toggle_view_desktop
[285] https://discourse.nixos.org/t/nixpkgs-tops-repology-stats/279
[211] https://nix.dev/manual/nix/2.23/command-ref/nix-store/optimise
[212] https://nixos.wiki/wiki/Storage_optimization
[213] https://www.mankier.com/1/nix-store
[214] https://github.com/NixOS/nixpkgs/blob/master/doc/build-helpers/special/checkpoint-build.section.md
[215] https://discourse.nixos.org/t/nix-store-gives-bad-estimate-about-hard-linking-after-gc/13480
[216] https://nix.dev/manual/nix/2.24/command-ref/nix-store/gc
[217] https://www.reddit.com/r/NixOS/comments/1r4et5s/didnt_learn_about_nix_garbage_collector_until/
[218] https://nix.dev/manual/nix/2.26/command-ref/nix-store/gc
[219] https://deepwiki.com/janbaer/nixos-config/7.4-garbage-collection-and-optimization
[220] https://garnix.io/blog/incremental-builds
[221] http://95.217.0.168/docs/incremental-builds
[222] https://fzakaria.com/2025/03/11/nix-dynamic-derivations-a-practical-application
[223] https://news.ycombinator.com/item?id=16529815
[224] https://news.ycombinator.com/item?id=28244665
[225] https://news.ycombinator.com/item?id=28241661
[256] https://nix.dev/manual/nix/2.33/language/import-from-derivation
[257] https://nix.dev/manual/nix/2.26/language/import-from-derivation
[258] https://github.com/NixOS/nix/issues/6299
[259] https://nix.dev/manual/nix/2.23/language/import-from-derivation
[260] https://discourse.nixos.org/t/using-readfile-on-derivation-output/14868/2
[261] https://nix.dev/manual/nix/2.31/language/import-from-derivation.html
[262] https://nix.dev/manual/nix/2.25/language/import-from-derivation
[263] https://nix.dev/manual/nix/2.29/language/import-from-derivation
[264] https://nix.dev/manual/nix/2.28/language/import-from-derivation
[265] https://nix.dev/manual/nix/2.19/language/import-from-derivation
[266] https://discourse.nixos.org/t/using-readfile-on-derivation-output/14868
[267] https://crane.dev/faq/ifd-error.html
[268] https://git.lix.systems/lix-project/lix/issues/86
[269] https://discourse.nixos.org/t/is-readfile-foo-sh-an-ifd/60857
[270] https://discourse.nixos.org/t/slow-nixos-rebuild/48059
[226] https://discourse.nixos.org/t/why-are-flakes-still-experimental/29317
[227] https://discourse.nixos.org/t/improving-flakes/12831
[228] https://releases.nixos.org/nix/nix-2.30.3/manual/release-notes/rl-2.26.html
[229] https://nix.dev/concepts/flakes.html
[230] https://discourse.nixos.org/t/experimental-does-not-mean-unstable-detsyss-perspective-on-nix-flakes/32703/25
[231] https://grahamc.com/blog/flakes-are-an-obviously-good-thing/
[232] https://discourse.nixos.org/t/an-incremental-strategy-for-stabilizing-flakes/16323
[233] https://discourse.nixos.org/t/my-painpoints-with-flakes/9750
[234] https://github.com/NixOS/nix/issues/7422
[235] https://spacekookie.de/blog/i-am-no-longer-using-flakes/
[236] https://discourse.nixos.org/t/determinate-nix-changelog-deprecating-channels-and-indirect-flake-references/63917
[237] https://discourse.nixos.org/t/a-call-for-the-nix-team-to-present-a-unified-front-on-the-outcome-and-strategy-around-nix-flakes/54959?page=4
[238] https://github.com/NixOS/rfcs/pull/136
[239] https://discourse.nixos.org/t/experimental-does-not-mean-unstable-detsyss-perspective-on-nix-flakes/32703
[240] https://discourse.nixos.org/t/why-are-flakes-still-experimental/29317/12
[301] https://en.wikipedia.org/wiki/GNU_Guix
[302] https://guix.gnu.org/blog/2018/multi-dimensional-transactions-and-rollbacks-oh-my/
[303] https://guix.gnu.org/manual/devel/en/guix.html
[304] https://cs.petrsu.ru/~kryshen/guix/manual/en/Features.html
[305] https://guix.gnu.org/guix-ghm-20120721.pdf
[306] https://lwn.net/Articles/560857/
[307] https://guix.gnu.org/manual/en/html_node/Managing-Software-the-Guix-Way.html
[308] https://guix.gnu.org/manual/devel/en/html_node/Derivations.html
[309] https://guix.gnu.org/en/about/
[310] https://lists.gnu.org/archive/html/info-gnu/2022-12/msg00007.html
[311] https://guix.gnu.org/manual/en/html_node/Derivations.html
[312] https://lists.gnu.org/archive/html/info-gnu/2018-07/msg00002.html
[313] https://arxiv.org/pdf/1305.4584
[314] https://guix.gnu.org/
[315] https://guix.gnu.org/manual/devel/en/html_node/The-Store.html
[316] https://e4s-project.github.io/e4s-buildcache/
[317] https://spack.readthedocs.io/en/latest/frequently_asked_questions.html
[318] https://github.com/spack/spack/issues/33461
[319] https://spack.readthedocs.io/en/latest/_sources/build_settings.rst.txt
[320] https://spack.io/spack-binary-packages/
[321] https://spack-tutorial.readthedocs.io/en/latest/tutorial_binary_cache.html
[322] https://github.com/spack/spack/discussions/44141
[323] https://spack.readthedocs.io/en/latest/binary_caches.html
[324] https://mug.mvapich.cse.ohio-state.edu/static/media/mug/presentations/21/spack-mug21.pdf
[325] https://cache.spack.io/
[326] https://github.com/spack/spack/issues/5676
[327] https://github.com/spack/spack/issues/26167
[328] https://aws.amazon.com/blogs/hpc/introducing-the-spack-rolling-binary-cache/
[329] https://indico.cern.ch/event/813800/contributions/3830178/attachments/2031805/3400697/SpackGDBMeeting_1.pdf
[330] https://documentation.its.umich.edu/arc-spack/glossery
[286] https://conda.github.io/conda-libmamba-solver/
[287] https://conda.github.io/conda-libmamba-solver/user-guide/
[288] https://github.com/conda/conda-libmamba-solver
[289] https://conda.org/blog/2023-11-06-conda-23-10-0-release
[290] https://anaconda.org/anaconda/conda-libmamba-solver
[291] https://conda.github.io/conda-libmamba-solver/_sources/user-guide/index.md.txt
[292] https://conda.github.io/conda-libmamba-solver/user-guide/faq/
[293] https://github.com/conda/conda/issues/13566
[294] https://conda.org/blog/2024-01-31-january-releases/
[295] https://www.anaconda.com/blog/a-faster-conda-for-a-growing-community
[296] https://docs.conda.io/projects/conda/en/latest/user-guide/concepts/conda-performance.html
[297] https://conda.github.io/conda-libmamba-solver/dev/changelog/
[298] https://docs.conda.io/projects/conda/en/stable/user-guide/concepts/conda-performance.html
[299] https://anaconda.org/conda-forge/conda-libmamba-solver
[300] https://www.anaconda.com/blog/conda-is-fast-now
[331] https://nix.dev/manual/nix/2.18/language/
[332] https://nix.dev/manual/nix/2.26/language/
[333] https://nix.dev/tutorials/nix-language.html
[334] https://nix.dev/manual/nix/2.25/language/
[335] https://nix.dev/manual/nix/2.23/language/
[336] https://nix.dev/manual/nix/2.28/language/
[337] https://nix.dev/manual/nix/2.19/language/
[338] https://nix.dev/manual/nix/2.33/language/index.html
[339] https://nix.dev/manual/nix/2.32/language/
[340] https://nix.dev/manual/nix/2.34/language/index.html
[341] https://zemdregon.github.io/nix-docs/cheatsheets/language/
[342] https://nix.dev/manual/nix/2.30/language/index.html
[343] https://nix.dev/manual/nix/2.27/language/
[344] https://adoptingstability.com/primer/
[345] https://nix.dev/manual/nix/2.18/language/values
[376] https://nixos.org/surveys/community/2025/
[377] https://discourse.nixos.org/t/2025-nixos-community-survey-report/78812
[378] https://nixos.org/surveys/
[379] https://discourse.nixos.org/t/nix-community-survey-2024-results/55403/3
[380] https://discourse.nixos.org/t/nix-community-survey-2024-results/55403
[381] https://discourse.nixos.org/t/discussion-of-flakes-from-2025-nixos-community-survey-report/78853/45
[382] https://discourse.nixos.org/t/discussion-of-flakes-from-2025-nixos-community-survey-report/78853/27
[383] https://discourse.nixos.org/t/discussion-of-flakes-from-2025-nixos-community-survey-report/78853
[384] https://discourse.nixos.org/t/nix-community-survey-2025/68870
[385] https://discourse.nixos.org/t/ux-design-for-the-nix-portal/53746/3
[386] https://discourse.nixos.org/t/discussion-of-flakes-from-2025-nixos-community-survey-report/78853?page=3
[387] https://discourse.nixos.org/t/nix-community-survey-2024/49446
[388] https://discourse.nixos.org/t/discussion-of-flakes-from-2025-nixos-community-survey-report/78853?page=2
[389] https://nixos.org/blog/
[390] https://www.linkedin.com/posts/ronefroni_3399-nixosers-took-the-time-to-tell-us-activity-7485401252466659328-RoPd
[346] https://nix.dev/manual/nix/2.24/command-ref/nix-env/rollback
[347] https://nix.dev/manual/nix/2.33/command-ref/nix-env/rollback
[348] https://wiki.nixos.org/wiki/Nixos-rebuild
[349] https://nix.dev/manual/nix/2.18/command-ref/nix-env/rollback
[350] https://wiki.nixos.org/wiki/Development_environment_with_nix-shell
[351] https://nix.dev/manual/nix/2.28/command-ref/nix-env/rollback
[352] https://devenv.sh/
[353] https://nix.dev/manual/nix/2.27/command-ref/nix-env/rollback.html
[354] https://nix.dev/manual/nix/2.18/command-ref/nix-env/delete-generations
[355] https://haseebmajid.dev/slides/reproducible-envs-with-nix/
[356] https://oneuptime.com/blog/post/2026-01-25-nix-reproducible-environments/view
[357] https://github.com/cachix/devenv
[358] https://nix.dev/manual/nix/2.18/command-ref/new-cli/nix3-develop
[359] https://dev.to/iampavel/reproducible-dev-environments-with-nix-and-direnv-5gba
[360] https://letsbuildsolutions.com/blog/devops/nix-for-development-environments-flakes-dev-shells-and-reproducible-builds-that-eliminate-configuration-drift/
[361] https://nix.dev/tutorials/packaging-existing-software.html
[362] https://github.com/NixOS/nixpkgs/blob/master/doc/stdenv/stdenv.chapter.md
[363] https://toraritte.github.io/saves/Nixpkgs-Manual-Version-20.09.html
[364] https://nixos.org/guides/nix-pills/20-basic-dependencies-and-hooks.html
[365] https://releases.nixos.org/nixpkgs/nixpkgs-1.0pre29503_fca11ef/manual/manual.pdf
[366] https://discourse.nixos.org/t/are-hooks-abused-as-a-module-system-for-nix-builds/13107
[367] https://github.com/NixOS/nixpkgs/issues/296544
[368] https://jade.fyi/blog/building-nix-derivations-manually/
[369] https://pavluk.org/blog/2023/05/30/nix_packaging.html
[370] https://unix.stackexchange.com/questions/717168/how-to-package-my-software-in-nix-or-write-my-own-package-derivation-for-nixpkgs
[371] https://github.com/NixOS/nixpkgs/issues/354220
[372] https://nixos.org/guides/nix-pills/19-fundamentals-of-stdenv
[373] https://n8henrie.com/2023/08/hacking-on-nixpkgs-with-nix-develop/
[374] https://nixos.wiki/wiki/Nixpkgs/Create_and_debug_packages
[375] https://github.com/nixOS/nixpkgs/issues/393496
[421] https://discourse.nixos.org/t/disk-usage-for-nix-store-speaking-of-bloat/74217
[422] https://docs.nixos.org/
[423] https://www.reddit.com/r/NixOS/comments/1j04qne/why_shouldnt_i_use_nix/
[424] https://nix.dev/reference/nix-manual.html
[425] https://news.ycombinator.com/item?id=42355690
[426] https://news.ycombinator.com/item?id=42355877
[427] https://discourse.nixos.org/t/disk-usage-for-nix-store-speaking-of-bloat/74217?page=2
[428] https://nixos-and-flakes.thiscute.world/introduction/advantages-and-disadvantages
[429] https://discourse.nixos.org/t/nix-store-and-rebuild-takes-up-all-available-space/50186
[430] https://discourse.nixos.org/c/howto/15
[431] https://lab.abilian.com/Tech/Linux/Packaging/Nix/Some%20comments%20on%20Nix/
[432] https://talks.nixcon.org/nixcon-2022/talk/KFR3ZF/
[433] https://github.com/NixOS/nixpkgs/blob/master/nixos/modules/misc/documentation.nix
[434] https://nixos.org/manual/nixos/stable/release-notes
[435] https://nix.dev/manual/nix/2.34/release-notes/rl-2.34
[406] https://ntrs.nasa.gov/api/citations/20260001679/downloads/HPSF-Spack-Presentation.pdf
[407] https://binaries.spack.io/
[408] https://numpex-pc5.gitlabpages.inria.fr/tutorials/hpc-env/bin-spack/index.html
[409] https://github.com/spack/spack/issues/15040
[410] https://spack-tutorial.readthedocs.io/en/latest/tutorial_binary_cache.html
[411] https://github.com/spack/spack/labels/concretization
[412] https://cache.spack.io/
[413] https://e4s-project.github.io/e4s-buildcache/
[414] https://github.com/spack/spack/discussions/30634
[415] https://spack.io/spack-binary-packages/
[416] https://static.sched.com/hosted_files/hpsf2025/9b/HPSF_2025_Democratizing_HPC_BinaryCache_AWS.pdf
[417] https://cloud.google.com/blog/topics/hpc/announcing-public-binaries-for-spack-on-google-cloud/
[418] https://spack.readthedocs.io/en/latest/frequently_asked_questions.html
[419] https://aws.amazon.com/blogs/hpc/introducing-the-spack-rolling-binary-cache/
[420] https://github.com/spack/spack/blob/develop/CHANGELOG.md
[391] https://nix.dev/nix-dev.pdf
[392] https://news.ycombinator.com/item?id=42666851
[393] https://dev.to/pickuma/nixos-nixpkgs-in-2026-reproducible-dev-environments-without-docker-38o1
[394] https://www.gamesgokul.com/homepage/home/blogs/blog627-standardizing-developer-workspaces-using-nix-shells.html
[395] https://wiki.nixos.org/wiki/Nixos-rebuild
[396] https://devopsboys.com/blog/nix-reproducible-devops-environments-guide-2026
[397] https://oneuptime.com/blog/post/2026-01-25-nix-reproducible-environments/view
[398] https://www.mrlatte.net/en/stories/2026/03/22/why-i-love-nixos/
[399] https://www.kanthi.in/posts/why-nix-nixos-a-declarative-journey/
[400] https://nix.dev/tutorials/first-steps/declarative-shell.html
[401] https://nixos.org/guides/how-nix-works/
[402] https://cfp.cfgmgmtcamp.org/ghent2026/talk/H3UXV8/
[403] https://discourse.nixos.org/t/reproducibility-nix-develop-derivations-and-ignore-environment/32415
[404] https://bashlover.de/posts/why-nix/
[405] https://smartlogic.io/podcast/elixir-wizards/s14-e08-nix-for-elixir-apps/
