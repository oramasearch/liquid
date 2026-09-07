# Packaging and images

> Research: [10-os-packaging](../research/10-os-packaging/) ·
> [11-container-images](../research/11-container-images/)
>
> **Status:** Tier A `.deb`, `.rpm`, `.apk`, OCI image, and Helm projection is implemented in issue
> #28, including derived dependencies, ABI refusal, deterministic compression, and attached
> attestations. Issue #77 adds Homebrew bottles, Arch binary-package recipes, AppImage, Flatpak, and
> Snap, including the owned SquashFS writer and detached attestations. Issue #78 adds deterministic
> `archive-z2` and `bare-user` OSTree commits plus bootc OCI encapsulation from a digest-pinned base
> tree and overlays. Issue #76 adds reproducible unsigned macOS packages, explicitly non-reproducible
> disk images, deterministic MSIX and winget manifests, external platform signing, and the
> `platform_signature/1` chain. The credentialed Apple notarisation run remains pending. Issue #80
> adds declarative install intent, generated maintainer scripts, file-owner conflict detection,
> derived replacement relationships, and generation-based install/rollback. Issue #79 implements
> Tier B for Debian source packages; other distribution source-policy formats remain deferred.

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
source packages a Debian maintainer would sponsor. Debian is implemented. A distribution qualifies
for another Tier B target only when liquid can run both its reproducible source-build tool and its
policy linter in a pinned peer environment.

## Targets

| Target | Notes |
|---|---|
| **`.deb` / apt** | `ar` + `control.tar` + `data.tar`. Full relationship field support — `Depends`, `Pre-Depends`, `Recommends`, `Conflicts`, `Breaks`, `Replaces`, `Provides`. `Breaks` vs `Conflicts` reflects dpkg's two-phase install; `Provides` is virtual packages. |
| **Debian source** | Implemented Tier B output: unsigned `.dsc`, deterministic orig and Debian tarballs, generated `debian/` policy files, `sbuild`, and `lintian -EvIL +pedantic` peer acceptance. |
| **`.rpm` / dnf** | Spec generation, `%files` with ownership and permissions, automatic `SONAME`-derived dependencies. |
| **Alpine `apk`** | The simplest correct target; the natural first implementation. |
| **Arch `PKGBUILD`** | Implemented as a deterministic binary-package source archive, `PKGBUILD`, and `.SRCINFO`, with derived ELF dependencies. |
| **Homebrew** | Formulae and stable-gzip bottles implemented with relocation refusal and `:any_skip_relocation`. |
| **Windows** | MSIX and winget with Authenticode signing are implemented. MSI is not part of #76. |
| **macOS** | Reproducible unsigned `pkg`, non-reproducible `dmg`, external codesigning, and the notarisation path are implemented. The credentialed notarisation gate is pending. |
| **Flatpak / Snap / AppImage** | Implemented manifests, pack-ready trees, pinned AppImage runtimes, and owned SquashFS output. External runtime/store publication remains separate. |
| **OCI images** | See below. |
| **ostree / bootc commits** | Implemented in #78: a supplied root tree plus path-mounted overlays becomes a reproducible OSTree commit and a component-layered bootc OCI layout. Booting it is Phase 7. |
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

The implemented `install_intent/1` vocabulary is closed: service, user, group, directory, symlink,
cache update, and alternatives registration. Strict name and canonical-path grammars run before
lowering. Debian, RPM, and Alpine lifecycle scripts come from the same intent and fixed POSIX
templates; their digests are bound into both the projection attestation and package SBOM.

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
- **File conflicts** between packages claiming a path — implemented for request batches and
  package, dpkg, or RPM inventories, with explicit replacement derivation.
- **Transactional upgrade** with rollback — implemented with staged generations and one atomic
  `current` selection.
- **Cross-compilation and sysroots** — first-class, because the toolchain is an input.

## Tier B decision

Liquid emits source packages for Debian only in Phase 5. Debian is the distribution named by this
specification, `sbuild` supplies a reproducible mechanical build, and lintian supplies a concrete
policy bar. The acceptance target is an unsigned package tree a Debian maintainer could review and
sponsor. OpenPGP signing remains the sponsor's `debsign` step; liquid does not add another signing
framework.

RPM `.spec` output remains an ordinary Tier A byproduct and is labelled not policy-reviewed.
Fedora, openSUSE, Arch, and every other distribution source-policy format are deferred. They can
qualify later only with a reproducible source-build tool and policy linter runnable in a pinned
peer environment.

## Open questions

- **Apple notarisation credentials** — the credentialed run remains pending; its custody and
  signature model are documented in
  [projection.md](../library/projection.md#signing-key-custody-per-platform).
