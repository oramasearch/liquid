# Synthesis

> This document draws the cross-cutting conclusions from all twenty research
> topics. It is the argument for why liquid is one tool rather than several, and
> what has to be true for it to work.

## The one-sentence thesis

**Every tool liquid replaces is independently rediscovering the same three
mechanisms — a content-addressed store, a dependency graph over pure actions, and
lazy materialisation — and each is crippled by owning only one layer of the stack.**

Git content-addresses source but knows nothing about builds. Bazel content-addresses
build actions but knows nothing about source history or packaging. Nix
content-addresses packages but has no incrementality inside a build. OCI
content-addresses images but with a layer granularity so coarse it defeats the
purpose. Zoekt content-addresses blobs for search but must reindex whole
repositories because it does not own the change stream.

Every one of these is a partial implementation of the same idea. liquid's bet is
that **doing it once, properly, across all the layers is worth more than the sum of
the parts** — because the boundaries between the layers are where the losses are.

## The three mechanisms, and who already proved each

**1. One content-addressed store, chunked.**

- Git proves content addressing makes distribution and integrity fall out for free
  ([01](../01-version-control/)).
- Hugging Face Xet proves content-defined chunking beats file-level versioning by
  ~50% on real workloads, and that the dedup unit must differ from the transfer unit
  ([02](../02-large-files-and-binary-assets/)).
- restic and Borg prove dedup and strong encryption coexist if you dedup *before*
  encrypting with keyed chunk IDs ([14](../14-secrets-and-encryption/)).
- Nydus proves the same conclusion from the container runtime side
  ([11](../11-container-images/)).
- Blackbird proves indexing by blob ID deduplicates search across 45M repositories
  ([19](../19-full-text-search/)).

Five domains, one answer.

**2. One dependency graph over pure, observed actions.**

- Bazel proves an action as a pure function from an input Merkle tree gives
  hermeticity, caching, distribution and provenance from a single abstraction
  ([04](../04-build-systems/), [07](../07-distributed-execution/)).
- Buck2 proves it needs **dynamic** dependencies to handle real languages.
- Salsa and rustc prove the same graph, made demand-driven with fingerprints and
  early cutoff, gives sub-second incrementality
  ([08](../08-incremental-compilation/)).
- ccache's direct-mode manifest proves dependencies should be **observed and
  recorded**, not declared ([06](../06-build-and-compiler-caching/)).
- Spack proves resolution over that graph should be declarative constraint
  optimisation, not ad-hoc ([09](../09-package-managers/)).

**3. Lazy materialisation everywhere.**

Bazel's Build without the Bytes, EdenFS and VFS for Git, stargz/SOCI/Nydus, and
HTTP range requests over a CDN are four communities independently discovering that
**eager materialisation is the dominant cost**
([07](../07-distributed-execution/), [16](../16-monorepo-scale-and-authoring-loop/),
[11](../11-container-images/), [12](../12-artifact-distribution/)).

## What "build the world" actually is

Steve's complaint has four distinct causes, and only fixing all four eliminates it:

1. **Coarse granularity.** Nix and Spack rebuild whole packages; Docker rebuilds
   whole layers. Fix: the action is a compile, not a package.
2. **Input addressing.** A glibc bump changes every downstream store path *even when
   every rebuilt artifact is byte-identical*. This is Nix's mass-rebuild problem in
   one sentence. Fix: **content-address outputs**, so an identical rebuild
   terminates the cascade.
3. **Over-declared dependencies.** Fix: observe what an action actually read.
4. **Cache misses from irrelevant variation** — paths, timestamps, environment. Fix:
   normalise by construction inside a sandbox.

**Content-addressed outputs are the highest-leverage single decision in the
project.** Nix knows this (RFC 062) and has spent years unable to retrofit it.
liquid can make it the default on day one — and doing so *requires* reproducibility
to be a hard, enforced property, which is why [13](../13-supply-chain-security/) is
load-bearing rather than a feature.

## The things that are free because liquid owns the whole stack

This is the actual argument for the project. Each of these is expensive or
impossible for a tool that owns only one layer:

| Capability | Why it is nearly free for liquid |
|---|---|
| **SLSA provenance and SBOMs** | liquid already records every input, the toolchain and the outputs of every action. The attestation *is* the build record ([13](../13-supply-chain-security/)). Everyone else infers or hand-writes it. |
| **Compile-as-you-type** | The editor and the build query the same warm incremental graph. No duplicated language-server universe ([08](../08-incremental-compilation/), [16](../16-monorepo-scale-and-authoring-loop/)). |
| **Incremental compilation inside a container image** | liquid compiles with its own cache and then assembles the image, so a one-line change produces one new object and one changed chunk. The loudest Docker complaint, dissolved ([11](../11-container-images/)). |
| **No buildpack heuristics** | A PaaS guesses how to build a repo; liquid knows, because it built it ([20](../20-deployment-runtime-k8s/)). |
| **Search inside published artifacts** | The format registry that decomposes a `.deb` for storage also indexes it ([19](../19-full-text-search/), [03](../03-structural-diff-and-formats/)). |
| **Continuous ABI checking** | One extraction feeds packaging, safe substitution and hot-update classification ([15](../15-binary-introspection/), [10](../10-os-packaging/), [17](../17-runtime-hot-update/)). |
| **Cheap PR environments** | Images share chunks; builds share a cache ([20](../20-deployment-runtime-k8s/)). |
| **`./configure` in milliseconds** | Probe results are a function of (toolchain, sysroot, target) — content-addressable, cacheable, shareable globally ([05](../05-autotools-and-configure-era/)). |

## Shared components, built once

The research repeatedly points at the same handful of components serving many
purposes. Building each once, properly, is the architecture:

- **The chunk store + Merkle DAG** — source, artifacts, images, search corpus,
  encrypted content.
- **The virtual filesystem / lazy materialiser** — checkout, large files, build
  outputs, artifact fetch, container rootfs.
- **The incremental query engine** — build, package resolution, editor analysis,
  search indexing.
- **The format registry** — diff, dedup, packaging, SBOM, search, decomposition.
- **The sandbox** — hermeticity, dependency observation, reproducibility,
  provenance.
- **The signing and attestation layer** — commits, artifacts, cache entries,
  images, deployments.

If any of these ends up implemented twice, the design has gone wrong.

## The genuinely novel parts

Most of liquid is careful synthesis of proven ideas. Three things are not, and they
carry the real risk:

1. **First-class repository composition.** Nesting, splitting and merging unrelated
   repositories with history intact. Nothing does this. It subsumes both submodules
   and the merge-two-codebases problem, and it must be in the object model from the
   start ([01](../01-version-control/)).
2. **Unifying VCS and build under one incremental graph.** Individually proven;
   never combined. This is what makes drip-by-drip possible.
3. **Runtime hot update driven by build-level change classification.** The honest
   scope is tiers 0–2 with a clean fallback to restart
   ([17](../17-runtime-hot-update/)).

## Risks worth stating plainly

- **Scope.** This is five or six ambitious products. Sequencing is everything.
- **Incremental correctness.** rustc has shipped incremental-compilation
  correctness bugs repeatedly. A stale-but-plausible artifact is worse than a slow
  build. Continuous clean-rebuild validation is mandatory, not optional
  ([08](../08-incremental-compilation/)).
- **Shared-cache trust.** A writable shared cache is a fleet-wide code-execution
  primitive ([06](../06-build-and-compiler-caching/)).
- **GC in a distributed chunked store** is research-grade and must be designed
  before launch, not after ([12](../12-artifact-distribution/)).
- **Network effects.** Git and GitHub are not beaten on merit. jj's Git backend is
  why anyone tried jj; liquid needs an equally lossless on-ramp.
- **C++26 is less ready than the label implies** ([18](../18-implementation-cpp26/)).

## What to build first

The dependency order is fairly forced:

1. **The chunk store and object model**, with repository composition designed in.
   Everything sits on this, and it cannot be changed later.
2. **The sandboxed action executor with dependency observation**, and the three-tier
   cache. This alone — wrapping existing CMake/Autotools projects to give them
   caching and provenance — is a shippable product.
3. **Remote execution** via REAPI compatibility, so existing farms work.
4. **The incremental query engine**, turning the action graph demand-driven with
   early cutoff.
5. **Resolution** — Spack-grade constraint solving over content-addressed outputs.
6. **Projection** — OCI images, OS packages, the artifact service.
7. **Search, deployment, hot update.**

Steps 1–2 are the foundation and the first useful product. If liquid can wrap an
existing project and make its builds correct, cached, distributed and attested
without that project changing anything, everything else has a route to market.
