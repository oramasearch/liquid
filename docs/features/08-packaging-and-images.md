# Packaging and images

> Research: [10-os-packaging](../research/10-os-packaging/) ·
> [11-container-images](../research/11-container-images/)
>
> **Status:** Tier A `.deb`, `.rpm`, `.apk`, OCI image, and Helm projection is implemented in issue
> #28, including derived dependencies, ABI refusal, deterministic compression, and attached
> attestations. Maintainer scripts and conflict transactions remain issue #80; Tier B remains #79.

## Projection, not translation

liquid builds an artifact once. Package formats are **projections** of liquid's
internal model — a content-addressed file tree plus complete dependency, ABI and
provenance metadata, which is a superset of what any format needs.

This is the only tractable architecture. A universal packager that translates
between formats fails, because every distribution encodes policy, not just structure.

## Two honest tiers

Blurring these is how cross-format packagers disappoint people.

**Tier A — installable packages.** Correct archives with correct dependency metadata
that install, upgrade and remove cleanly. This is what FPM and nfpm users actually
want, and it is broadly useful. liquid ships this.

**Tier B — distro-policy-compliant packages.** Lintian-clean, policy-conformant
source packages a Debian maintainer would sponsor. Enormously more work, genuinely
valuable, and a later goal.

## Targets

| Target | Notes |
|---|---|
| **`.deb` / apt** | `ar` + `control.tar` + `data.tar`. Full relationship field support — `Depends`, `Pre-Depends`, `Recommends`, `Conflicts`, `Breaks`, `Replaces`, `Provides`. `Breaks` vs `Conflicts` reflects dpkg's two-phase install; `Provides` is virtual packages. |
| **`.rpm` / dnf** | Spec generation, `%files` with ownership and permissions, automatic `SONAME`-derived dependencies. |
| **Alpine `apk`** | The simplest correct target; the natural first implementation. |
| **Arch `PKGBUILD`** | |
| **Homebrew** | Formulae and bottles, with relocation. |
| **Windows** | MSI / MSIX / winget, with Authenticode signing. |
| **macOS** | `pkg` / `dmg`, with **codesigning and notarisation** — a hard gate, not polish. |
| **Flatpak / Snap / AppImage** | Bundled runtimes. |
| **OCI images** | See below. |
| **ostree / bootc commits** | The image-based-OS target, and the closest fit to liquid's model. |
| **Helm charts** | The Kubernetes packaging standard — see below. |

## Helm charts

liquid emits Helm charts as a projection, like every other package format.

Two things make this more than a template renderer:

- **Charts are generated from the deployment graph, not hand-written.** liquid knows
  what it built, what the images are, what they depend on, what ports and volumes they
  need, and which config and secrets they consume ([13](13-deployment-runtime.md)).
  Values, image digests and dependency ordering are derived rather than transcribed
  into templated YAML by hand — which is where most chart bugs live.
- **Images are pinned by digest, with provenance attached.** A chart references
  immutable digests rather than mutable tags, and each referenced image carries its
  attestation ([10](10-supply-chain.md)). "What is actually deployed, and where did it
  come from?" stays answerable after the chart ships.

Helm 3 charts are **OCI artifacts**, so lake's registry hosts them natively with no
separate chart museum ([09](09-artifact-service.md)) — and they deduplicate against the
images they reference, because both are chunks in one store.

Charts remain a first-class **output** even for teams deploying to their own clusters
rather than to lake's distribution. Emitting the ecosystem's standard format is how
liquid stays adoptable without demanding the runtime be adopted too.

## Dependencies are derived, never written

RPM's automatic `SONAME` extraction is the right idea; liquid does better because it
knows the exact build inputs *and* can inspect the produced binaries. Package
dependency metadata is generated from the build graph plus ABI extraction
([07](07-dependency-resolution.md)), never hand-maintained.

## Maintainer scripts are a liability

`preinst`/`postinst`/`prerm`/`postrm` are the top source of packaging bugs and an
install-time arbitrary-code-execution vector.

Where liquid emits them, it **generates them from declarative intent** — register a
service, update a cache, create a user — rather than passing through arbitrary user
shell.

## OCI images

liquid emits OCI images; it does not adopt tar layers internally
([01](01-object-model-and-storage.md)).

**Deterministic by construction.** Sorted entries, `SOURCE_DATE_EPOCH`, fixed
ownership and modes, stripped volatile metadata, pinned compressor settings. Output
is bit-identical across machines by default, not by careful configuration.

**Layers are computed, not hand-ordered.** liquid knows the real dependency graph, so
it partitions the filesystem to maximise registry reuse — stable dependencies low,
volatile application code high. This eliminates the entire `COPY . .` genre of
problem, because the user is no longer encoding a dependency order in instruction
sequence.

**Deleted bytes never ship.** liquid computes the final filesystem rather than
replaying mutations, so the whiteout-bloat trap — deleting a 1 GB file in a later
layer while still shipping it — cannot occur.

**Incremental compilation inside the image, for free.** The loudest Docker complaint.
liquid compiles with its own cache ([05](05-caching.md)) and then assembles the
image, so a one-line change recompiles one object and produces an image differing by
a handful of chunks. No cache mounts, no `cargo-chef`, no dependency pre-copy trick.

**Attestations attach without mutating the digest**, via OCI 1.1 `subject` and the
Referrers API ([10](10-supply-chain.md)).

**Cross-compile rather than emulate.** QEMU is the wrong default and is punishingly
slow for compilation. liquid ships toolchains in the input tree
([06](06-distributed-execution.md)), so cross-compilation is the natural path, with
native remote workers where a target genuinely must execute.

**zstd layers**, and optionally chunk-addressed lazy-pull formats where liquid also
controls the runtime ([13](13-deployment-runtime.md)).

## The hard problems liquid must actually solve

Archive formats are the easy part. These are not:

- **ABI compatibility** — extracted and diffed continuously
  ([07](07-dependency-resolution.md)).
- **Relocation** — implemented as position-independent install trees checked before publication;
  see [relocation.md](../library/relocation.md).
- **glibc version floors** and musl/glibc differences — expressed as constraints in
  the solver rather than discovered at runtime.
- **File conflicts** between packages claiming a path.
- **Transactional upgrade** with rollback.
- **Cross-compilation and sysroots** — first-class, because the toolchain is an input.

## Open questions

- **How far to pursue Tier B**, and for which distributions first.
- **Whether to emit source packages** (`.dsc`, `debian/rules`, spec files) or only
  binary packages. Source packages are what distributions actually maintain.
- **Signing key custody per platform** — Authenticode and notarisation have
  incompatible trust models.
