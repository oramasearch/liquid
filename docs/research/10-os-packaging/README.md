# OS packaging

> Evidence: [`sources.md`](sources.md)
>
> **Requirement:** liquid must produce packages for different operating systems —
> dpkg/apt, rpm, and the rest.

## The central finding

A universal packager **cannot be a format translator**. Producing a `.deb` is not
"write these files into an ar archive with a control file." Every distribution
encodes policy: where files go, how dependencies are expressed, what a package may
do at install time, how upgrades are sequenced, which libraries are allowed to be
bundled. FPM and nfpm exist and are useful precisely because they *don't* attempt
this — they produce packages that install, not packages a distribution would
accept.

liquid should be honest about which of those two goals it is serving, because they
demand different amounts of work.

## Debian: dpkg and apt

A `.deb` is an `ar` archive containing `debian-binary`, `control.tar.*` and
`data.tar.*`. The interesting parts are the relationship fields, which are more
expressive than most people realise:

`Depends`, `Pre-Depends`, `Recommends`, `Suggests`, `Enhances`, `Conflicts`,
`Breaks`, `Replaces`, `Provides`. The distinction between `Conflicts` (cannot be
unpacked simultaneously) and `Breaks` (cannot be *configured* simultaneously)
exists because dpkg has a two-phase install, and `Provides` gives virtual packages
— the same idea as Spack's virtual dependencies
([09](../09-package-managers/)).

**Maintainer scripts** (`preinst`, `postinst`, `prerm`, `postrm`) are the single
biggest source of packaging bugs. They are shell scripts that must be idempotent,
must handle every upgrade and downgrade path, must cope with being interrupted, and
must work when a *different* version of the package is partially configured.
**dpkg triggers** exist to batch expensive postinst work (ldconfig, icon caches)
across a transaction.

`debhelper`/`dh` compresses most of this into declarative sequences. Source
packages (`.dsc`, `debian/rules`, quilt patch series) are the actual unit Debian
maintains.

APT's solver is pluggable via EDSP, and `apt` and `aptitude` historically gave
different answers to the same question — a reminder that dependency resolution is
an optimisation problem with taste, not a single correct output.

## RPM

Spec files with declarative sections and macros; `%files` manifests with explicit
ownership and permissions; `libsolv` (SAT) for resolution; automatic dependency
generation from ELF `SONAME`s and script shebangs — a genuinely good idea, since
the package's real dependencies are extracted from the built artifacts rather than
declared by hand.

`deltarpm` ships binary deltas between package versions. Fedora's Modularity was an
ambitious attempt at parallel version streams that was ultimately retired — a
useful data point that "multiple versions in a traditional package manager" is
harder than it looks, and that Nix/Spack's store model exists for good reason.

## The rest of the target matrix

| Target | Notes |
|---|---|
| **Alpine apk** | Simple, fast, musl-based. Chainguard's melange/apko build on it because it is the easiest format to generate correctly. |
| **Arch pacman / PKGBUILD** | A shell function per phase. Minimal ceremony, rolling release, no stable ABI promise. |
| **Homebrew** | Ruby formulae; bottles are prebuilt binaries with relocation. Casks wrap macOS apps. |
| **Windows** | MSI (declarative, transactional, painful), MSIX, winget, Chocolatey. Code signing via Authenticode. |
| **macOS** | `pkg`/`dmg`, plus **codesigning and notarisation** — a hard gate: unnotarised binaries are refused by Gatekeeper. |
| **Flatpak / Snap / AppImage** | Bundle-the-world, with runtimes and sandboxing. Solve the dependency problem by ignoring it. |
| **Language ecosystems** | npm, pip/wheels/manylinux, cargo, go modules, maven — each with its own resolver and its own idea of a version. |

The existing cross-format tools — FPM, nfpm, CPack, jpackage, cargo-deb — all take
the "produce something installable" path, and none produce distro-acceptable
packages.

## The genuinely hard problems

These are the ones liquid must actually solve, and they are mostly *not* about
archive formats:

1. **ABI compatibility and symbol versioning.** Whether a rebuilt library can
   replace an installed one is an ABI question, and it is not answerable by looking
   at version numbers. `libabigail`/`abidiff` and `dpkg-gensymbols` extract and
   compare actual ABI surfaces — see
   [15-binary-introspection](../15-binary-introspection/).
2. **Relocation.** RPATH/RUNPATH, `$ORIGIN`, absolute paths embedded in binaries,
   scripts, `.pc` files and config. Spack's binary relocation is fragile for
   exactly this reason ([09](../09-package-managers/)); Homebrew bottles hit it too.
3. **glibc vs musl**, and glibc's own version floor: a binary built against a newer
   glibc will not run on an older one, which is why `manylinux` exists.
4. **File conflicts** between packages claiming the same path.
5. **Transactional upgrade.** Applying a set of package changes atomically, with
   rollback, when the underlying mechanism is "unpack files over the filesystem."
6. **Cross-compilation and sysroots** — building for a target without running on it.

## The immutable-image shift

Fedora Silverblue, ostree, bootc and NixOS change the picture significantly: the
system is an **image composed from content-addressed pieces**, updated atomically
with rollback, rather than a filesystem mutated by package scripts. ostree in
particular checks out by hardlinking from a content-addressed store — which is
structurally the same thing as a Nix store or liquid's CAS.

This is where the world is going, and it is much closer to liquid's natural model
than traditional package management is.

## Implications for liquid

1. **Build the artifact once; project it into formats.** liquid's internal model —
   a content-addressed tree of files plus complete dependency and provenance
   metadata — is a *superset* of what every package format needs. Package
   generation is then a projection: a per-format backend that maps liquid metadata
   onto that format's conventions. This is the only tractable architecture.

2. **Be explicit about two tiers, and do not blur them.**
   - **Tier A — installable packages.** Correct archives with correct dependency
     metadata that install and upgrade cleanly. Achievable, broadly useful, and
     what FPM/nfpm users actually want. Ship this.
   - **Tier B — distro-policy-compliant packages.** Lintian-clean, policy-conformant
     source packages a Debian maintainer would sponsor. Enormously more work, and
     genuinely valuable, but a later goal.

3. **Derive dependencies from artifacts, not declarations.** RPM's automatic
   `SONAME` extraction is the right model, and liquid can do better: it already
   knows the exact build inputs *and* can inspect the produced binaries
   ([15](../15-binary-introspection/)). Dependency metadata should be generated,
   never hand-written.

4. **Treat ABI as a first-class, checkable property.** Extract the ABI surface of
   every produced library and compare it against the previous version. This
   answers "can this be a drop-in replacement?" mechanically, which is the question
   behind symbol versioning, `Breaks`, soname bumps, and content-addressed
   substitution ([09](../09-package-managers/)).

5. **Solve relocation by construction, not by patching.** Decide the relocation
   strategy up front — `$ORIGIN`-relative RPATHs, or a store model where paths are
   fixed and content-addressed — rather than rewriting binaries after the fact.
   Spack's experience says post-hoc patching does not fully work.

6. **Maintainer scripts are a liability; minimise the surface.** Where liquid emits
   them, generate them from declarative intent (register a service, update a cache)
   rather than passing through user shell. Arbitrary install-time code is both the
   top bug source and a supply-chain execution vector
   ([13](../13-supply-chain-security/)).

7. **Target the image-based world as a first-class output.** ostree/bootc
   commits, OCI images ([11](../11-container-images/)) and atomic
   rollback fit liquid's content-addressed model far better than mutating a
   filesystem does. This is also the natural bridge to the deployment runtime
   ([20](../20-deployment-runtime-k8s/)).

8. **Handle signing and notarisation per platform.** macOS notarisation and
   Windows Authenticode are hard gates, not optional polish. They belong in the
   same signing infrastructure as everything else
   ([13](../13-supply-chain-security/)).
