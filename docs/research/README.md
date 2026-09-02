# liquid — research

This tree is the evidence base for liquid's design. It is deliberately separated
from `docs/features/`, which states what liquid will *do*. Research here states
what the world *currently does*, what works, what is broken, and what people
actually complain about.

## What liquid is

liquid is one tool that aims to subsume the roles currently split across git,
GitHub, Spack, Nix, `docker build`, CMake and Autotools, plus an artifact service
and a runtime. The organising idea is in the name: **software changes form**.
Source becomes objects, objects become libraries, libraries become packages,
packages become images, images become running processes — and liquid should be
able to move a project along that chain, incrementally and in both directions,
computing only the minimum delta at each step, distributed across many machines,
with a cryptographic chain of custody the whole way.

## How each directory is organised

Every topic directory contains:

- **`README.md`** — curated analysis written for this project. How the incumbent
  works, what is genuinely good about it, what is missing or broken, what people
  love and hate, and a closing **Implications for liquid** section. This is the
  part to read.
- **`sources.md`** — the raw web-grounded research the analysis was drawn from,
  with citations. Kept verbatim so claims can be checked and so the analysis can
  be revised without re-running the research.

## Topics

| # | Topic | Why it matters to liquid |
|---|---|---|
| [01](01-version-control/) | Version control | The core model. Git's object graph, patch theory (Darcs/Pijul), jj, Sapling, and the merge/compose problems liquid must beat. |
| [02](02-large-files-and-binary-assets/) | Large files and binary assets | Must work on day one. LFS's failures, Perforce's workflow wins, content-defined chunking, Xet. |
| [03](03-structural-diff-and-formats/) | Structural diff and container formats | Diffing *inside* zip, HDF5, Parquet, SQLite, OOXML — unpack, canonicalise, diff, repack. |
| [04](04-build-systems/) | Build systems | Bazel, Buck2, CMake, Ninja, Meson. Incremental-build correctness theory and the "build the world" problem. |
| [05](05-autotools-and-configure-era/) | Autotools and the configure era | Automake/Autoconf/Libtool, why `./configure` is slow, and the xz backdoor that hid in generated build machinery. |
| [06](06-build-and-compiler-caching/) | Build and compiler caching | Must be built in, not bolted on. ccache/sccache internals, cache-key correctness, false hits vs false misses. |
| [07](07-distributed-execution/) | Distributed execution | REAPI, BuildBarn/BuildBuddy/NativeLink, distcc/icecc, and why output materialisation dominates cost. |
| [08](08-incremental-compilation/) | Incremental compilation | Query-based compilers (Salsa, rustc), Unison's content-addressed code, and sub-file dependency granularity. |
| [09](09-package-managers/) | Package managers | Spack's ASP/Clingo concretiser (the model to beat), Nix's store and derivations, Guix, conda, PubGrub. |
| [10](10-os-packaging/) | OS packaging | dpkg/apt, rpm, apk, pacman, Homebrew, MSI, Flatpak/Snap. ABI, relocation, transactions. |
| [11](11-container-images/) | Container images | OCI internals, BuildKit's LLB graph, and why tar layers are a poor content-addressed format. |
| [12](12-artifact-distribution/) | Artifact distribution and CAS | Registries, Merkle DAGs, chunking, delta transfer, lazy fetch, and GC in a shared store. |
| [13](13-supply-chain-security/) | Supply chain security | SLSA, in-toto, Sigstore, TUF, reproducible builds, and the attacks liquid must structurally prevent. |
| [14](14-secrets-and-encryption/) | Secrets and encryption | Encrypted content in-repo, the determinism-vs-dedup tension, envelope encryption, and why history is forever. |
| [15](15-binary-introspection/) | Binary introspection and decomposition | The "decompile" direction: ELF/DWARF, build-id, debuginfod, diffoscope, SBOM-from-binary. |
| [16](16-monorepo-scale-and-authoring-loop/) | Monorepo scale and the authoring loop | EdenFS, VFS for Git, watchman, language servers — the machinery behind "compile as you type". |
| [17](17-runtime-hot-update/) | Runtime hot update | Erlang relups, kernel live patching, JVM HotSwap, Wasm — updating a running system without restart. |
| [18](18-implementation-cpp26/) | Implementation: C++26 | What C++26 actually delivers in 2026, and the best-in-class libraries for hashing, chunking, async I/O and crypto. |
| [19](19-full-text-search/) | Full-text and code search | Trigram indexes, Zoekt, GitHub Blackbird, and indexing a content-addressed versioned corpus. |
| [20](20-deployment-runtime-k8s/) | Deployment runtime and minimal k8s | k0s/k3s/Talos, Railway-style PaaS, buildpacks, GitOps — liquid's delivery target. |
| [21](21-consumer-sync-and-web/) | Consumer sync and web | Dropbox's Nucleus and block model, Drive, CFAPI/File Provider/FUSE, and sharing UX for non-developers. |
| [99](99-synthesis/) | Synthesis | Cross-cutting conclusions: the shared model, the ordering of the build, and what liquid must get right first. |

## Method and provenance

Research was gathered on **2026-09-01** via the Perplexity MCP server
(`perplexity_research`, and `perplexity_reason` for two topics where the deeper
tier timed out). Every `sources.md` is web-grounded and carries its citations.

Two cautions when reading `sources.md` files:

1. **They are evidence, not decisions.** Where a source recommends an
   architecture, that is the source's opinion about the existing landscape, not a
   liquid design commitment. Design commitments live in `docs/features/`.
2. **They were correct as gathered.** Version numbers, project status and pricing
   move. Re-check anything load-bearing before building on it.

## Reading order

If you are new to the project, read in this order:

1. [99-synthesis](99-synthesis/) — the argument in one place.
2. [01-version-control](01-version-control/) — the model everything else hangs off.
3. [04-build-systems](04-build-systems/) and [06-build-and-compiler-caching](06-build-and-compiler-caching/) — the incremental core.
4. [09-package-managers](09-package-managers/) — how dependency resolution should work.
5. [13-supply-chain-security](13-supply-chain-security/) — the property that must hold everywhere.
