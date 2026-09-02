<!-- Raw research source. Generated via Perplexity `perplexity_research` (web-grounded,
     multi-source) on 2026-09-01 for the liquid project. Lightly reformatted only.
     Treat as evidence, not as liquid design decisions - see README.md in this
     directory for the curated analysis and the implications drawn for liquid. -->

# OS-level software packaging and distribution, 2024–2026

## Executive conclusion

A “universal packager” cannot merely translate one archive format into another. It must behave more like a **compiler with policy-aware backends**:

1. build separately for every OS, architecture, libc and ABI baseline;
2. stage files into a target-specific filesystem tree;
3. translate dependency and conflict semantics;
4. generate lifecycle integration—users, services, caches, desktop metadata, configuration migration;
5. satisfy each ecosystem’s policy, signing, repository and review rules;
6. test fresh install, upgrade, downgrade, removal, purge, interrupted transaction and cross-architecture cases.

Existing cross-format tools are effective for self-contained applications, but they do not solve ABI compatibility, distribution policy, dependency-name mapping, repository admission, or safe lifecycle behavior.

---

# 1. Debian: `dpkg`, APT and `.deb`

## 1.1 Architecture of the stack

| Layer | Responsibility |
|---|---|
| `dpkg` | Low-level installation database and `.deb` unpack/configure/remove engine. It understands package states, conffiles, file ownership, maintainer scripts and triggers, but does not normally fetch repository dependencies itself. |
| APT (`apt`, `apt-get`) | Repository metadata, candidate-version selection, pinning, dependency solving, downloading, authentication and orchestration of `dpkg`. |
| `aptitude` | Separate higher-level front end with its own interactive dependency-resolution machinery and ability to propose alternative solutions. |
| Debian archive/release machinery | Determines whether a source package is acceptable, rebuildable, Policy-compliant and eligible to migrate from unstable to testing or to enter stable/backports. |

## 1.2 Binary `.deb` format

A modern `.deb` is a Unix **`ar` archive**, in this exact order:

1. `debian-binary` — currently contains `2.0\n`;
2. `control.tar`, optionally compressed as `.gz`, `.xz` or `.zst`;
3. `data.tar`, optionally compressed with gzip, xz, zstd, bzip2 or lzma.

`control.tar` contains the mandatory `control` file and may contain `conffiles`, `md5sums`, `triggers`, `shlibs`, `symbols`, and the four maintainer scripts. `data.tar` is the filesystem payload whose paths are unpacked relative to `/`. [web:27]

This split is important: converting a tarball into a `.deb` is easy; correctly producing the **control metadata and state transitions** is the hard part.

## 1.3 Important binary control fields

Typical identity fields include `Package`, `Version`, `Architecture`, `Maintainer`, `Description`, `Section`, `Priority`, `Installed-Size`, `Homepage`, `Essential` and `Multi-Arch`. Relationships are comma-separated conjunctions; fields such as `Depends` can contain `|` alternatives, version constraints and architecture/profile restrictions. [web:16][web:26]

| Field | Precise operational meaning |
|---|---|
| `Depends` | Package cannot be successfully configured until its dependencies are configured, subject to dependency-cycle handling. It does not necessarily prevent the package from being unpacked and left unconfigured. |
| `Pre-Depends` | Stronger ordering barrier: the dependency must have been configured before unpacking starts, although certain previously configured but currently unpacked states are accepted. Intended for rare bootstrap-critical cases. |
| `Recommends` | Strong but nonessential relationship: packages expected together in all but unusual installations. APT installs recommendations by default unless configured otherwise. |
| `Suggests` | Optional related functionality; omission is considered entirely reasonable. Normally not installed automatically. |
| `Breaks` | The named version may coexist in the unpacked sense, but must be deconfigured and cannot be configured simultaneously. Common for transitions and package splits. |
| `Conflicts` | Stronger than `Breaks`: both packages cannot be unpacked simultaneously. One must be removed before the other is installed. |
| `Replaces` | Permits taking ownership of files from another package; when paired with a conflict, it can also tell the package manager which whole package should be displaced. Package splits commonly use versioned `Breaks` plus `Replaces`. |
| `Provides` | Makes a concrete package satisfy a virtual package name, such as `mail-transport-agent`. Versioned provides use exact `Provides: name (= version)`; an unversioned provide cannot satisfy a versioned dependency. |

These distinctions are not interchangeable. In particular, Debian recommends `Breaks` rather than `Conflicts` where simultaneous unpacking is harmless, because `Conflicts` imposes much harder ordering constraints on the resolver. [web:348][web:347]

### File ownership

Two unrelated packages normally may not own the same pathname. `Replaces` authorizes a deliberate takeover, while `Breaks`/`Conflicts` handles the incompatible versions. Without correct transition metadata, `dpkg` aborts with “trying to overwrite” rather than silently choosing a winner. [web:347]

## 1.4 Maintainer scripts

| Script | Normal role | Important complications |
|---|---|---|
| `preinst` | Runs before the new payload is unpacked. | Cannot assume its own package files exist. Normally only Essential packages and `Pre-Depends` are safe to use. |
| `postinst` | Configures the unpacked package: service setup, alternatives, users, database/cache updates or configuration migration. | Dependencies are generally configured, except around cycles and recovery states. A failure leaves the package `Half-Configured`. |
| `prerm` | Runs before removal or before the old version is erased during upgrade. | May be invoked for deconfiguration, conflict handling or failed upgrade recovery, not just ordinary removal. |
| `postrm` | Runs after payload removal and on purge. | Its package files are gone; dependencies may also be unavailable. It must distinguish `remove`, `purge`, `upgrade`, `abort-*` and `disappear`. |

### Why scripts cause bugs

Maintainer scripts are arbitrary programs, usually run as root. That alone defeats purely declarative transaction analysis: a script can modify any file, start a service, alter a database, contact the network or add a repository key. Debian has explicitly discussed this as a security and predictability problem because scripts “can thus do anything.” [web:111]

They are also a state-machine problem:

- upgrades invoke a mixture of the **old and new package’s** scripts;
- a script can run after some new files were unpacked but while other files remain old;
- dependencies may be only `Unpacked`, `Half-Installed` or `Half-Configured`;
- scripts must be idempotent because interrupted operations are retried;
- they cannot require a terminal and must support unattended execution;
- error unwinding invokes additional `abort-install`, `abort-upgrade`, `failed-upgrade` or `abort-remove` actions;
- after the old `postrm upgrade`, Debian describes a point of no return where a later failure may require reinstalling the package;
- configuration errors are not fully unwound and can leave persistent half-configured states. [web:106]

Debian’s own maintainer guidance warns that many script bugs appear only during remove or purge, which is why testing only a fresh installation is inadequate. [web:279]

Good packaging therefore uses declarative helpers where possible—`tmpfiles.d`, `sysusers.d`, `update-alternatives`, `ucf`, `debconf`, systemd helper snippets—and keeps handwritten scripts small, version-aware and idempotent.

## 1.5 `dpkg` triggers

Triggers defer shared housekeeping until all relevant package changes have occurred. Typical conceptual uses include regenerating icon, MIME, font, manual-page or linker caches once rather than once per package.

A package can:

- declare `interest`, `interest-await` or `interest-noawait` in a named trigger;
- activate a trigger explicitly or through `activate[-await|-noawait]`;
- remain in `triggers-awaited` until the interested package processes it.

`noawait` is preferred where the generated result is not essential to configuring the activating package: it avoids an early trigger run and permits batching at the end of the upgrade. [web:355]

Triggers reduce duplicate script execution but introduce another asynchronous state: an apparently unpacked package may not reach fully installed status until a different package’s trigger handler completes.

## 1.6 Multiarch

| Value | Semantics |
|---|---|
| absent / `Multi-Arch: no` | Same-name packages for different architectures cannot coexist, and dependencies are normally satisfied only by the same architecture. |
| `Multi-Arch: same` | Same name and exact version may be installed for multiple architectures, typical for runtime libraries. Any pathname shared between variants must be bit-identical; architecture-specific libraries normally live under `/usr/lib/<multiarch-triplet>/`. |
| `Multi-Arch: foreign` | The package’s interface is considered architecture-independent and can satisfy another architecture’s dependency, typical for many command-line build tools. It is inappropriate for ordinary shared libraries. |
| `Multi-Arch: allowed` | Normally behaves like `no`, but a depender may explicitly use `package:any`; intended for unusual dual-use packages such as interpreters with architecture-specific plugins. |

`Multi-Arch: same` also makes maintainer scripts more difficult: one architecture’s removal must not assume every instance has disappeared. [web:16]

## 1.7 Source packages, `debian/rules` and `debhelper`

A Debian source package normally consists of:

- a signed `.dsc` control file listing the source artifacts and hashes;
- `name_version.orig.tar.*`, the upstream source;
- `name_version-revision.debian.tar.*`, Debian patches and packaging;
- or, for a native package, a single source tarball.

One source package may build several binary packages—for example runtime library, development headers, debug symbols, command-line tools and documentation. [web:361][web:365]

`debian/rules` is an executable makefile interface. Policy requires targets including `clean`, `build`, `build-arch`, `build-indep`, `binary`, `binary-arch` and `binary-indep`. [web:363]

In modern packages it may be nearly minimal:

```make
#!/usr/bin/make -f
%:
	dh $@
```

`dh` runs an ordered sequence of `debhelper` programs: configure/build/test, stage under `debian/package`, install docs and systemd units, calculate shared-library dependencies, generate maintainer-script fragments, strip binaries and construct `.deb` files. A maintainer can replace or extend individual steps with `override_dh_*`, `execute_before_dh_*` and `execute_after_dh_*` targets. [web:364]

This automation improves consistency, but it does not remove Debian-specific decisions: package splitting, SONAME transitions, copyright review, patches, reproducibility, architecture coverage and Policy interpretation remain human work.

## 1.8 APT dependency resolution, EDSP and aptitude

### Native APT solvers

Classic APT uses candidate versions, pin priorities, dependency relationships and heuristics to construct an install/remove/upgrade plan. APT 2.9.3 introduced **Solver3**, and APT 3.x carries it as a fully backtracking, DPLL-like solver selected with `--solver 3.0`; it delays choices, protects manually installed packages and can explain paths not taken more clearly than the classic resolver. [web:392][web:396]

### EDSP

The **External Dependency Solver Protocol** exports the installed/available package universe and the user request in a Deb822-like batch scenario. An external solver returns either an error or a complete proposed set expressed as installs and removals. `apt-cudf` can bridge EDSP to CUDF solvers such as `aspcud`. [web:136][web:141]

EDSP separates:

- **planning**—what package set satisfies the constraints—from
- **execution**—APT fetching artifacts and directing `dpkg`.

It does not make maintainer scripts transactional or prove the runtime compatibility of the resulting binaries.

### `apt-get` versus `aptitude`

| Tool | Solver behavior and intended use |
|---|---|
| `apt-get` | Script-friendly APT front end; normally produces one proposed transaction through APT’s configured solver. |
| `apt` | Human-facing interface over the same APT machinery, with friendlier progress and output not intended as a stable scripting API. |
| `aptitude` | Has immediate resolution followed by an interactive search when dependencies remain broken. It can rank and present multiple alternatives—remove one package, hold another, choose a different version—letting the operator navigate among solutions. [web:140][web:142] |

“Better solver” therefore depends on the objective function. One tool may minimize removals, another preserve manually installed packages, another minimize version changes. Dependency solving is not merely satisfiability; it is **optimization under user-policy preferences**.

## 1.9 Debian Policy and the real packaging bottleneck

Debian Policy defines technical and integration requirements, not merely archive syntax. A package must fit filesystem conventions, dependency rules, shared-library practice, maintainer-script behavior, configuration-file handling and archive licensing requirements. [web:370]

The practical pain comes from several independent clocks:

- upstream releases;
- volunteer maintainer availability;
- architecture builds and reproducibility;
- NEW/license review;
- testing migration and release-critical bugs;
- release freezes;
- stable’s conservative dependency baseline;
- backport maintenance.

Backports are generally versions already in testing, rebuilt against stable so that they run without importing newer libraries where possible. That can require recursively backporting dependencies, and the package version must preserve an upgrade path to the next stable release. [web:377][web:378]

A concrete institutional complaint came from Debian itself in January 2026: the project described long-standing complaints about **unpredictable NEW processing times**, nontransparent and rigid policy, and “palpable frustration”; it also reported that one person had performed 96.87% of the review workload before responsibilities were reorganized. [web:230] Earlier maintainers reported a median NEW wait of roughly two months. [web:228]

“Dependency hell” is consequently not only a weak resolver. It includes:

- incompatible repository suites and pinning;
- absent versions on an older stable release;
- transitions where many reverse dependencies must rebuild together;
- dependency cycles and virtual-provider ambiguity;
- development/runtime package splits;
- ABI changes hidden behind unchanged names;
- packages installable in metadata terms but unusable because a plugin, driver or external service is incompatible.

---

# 2. RPM and DNF

## 2.1 RPM format and spec files

An RPM consists conceptually of lead/signature/header information plus a compressed `cpio` payload. Binary RPM metadata records dependencies, capabilities, file ownership, checksums, scriptlets and signatures. A source RPM carries sources, patches and the spec recipe used to build binary RPMs. [web:170][web:171]

A spec file is heavily macro-expanded and typically contains:

| Section/tag | Purpose |
|---|---|
| `Name`, `Version`, `Release`, `Epoch` | Package identity and RPM version ordering. |
| `Summary`, `License`, `URL` | Descriptive metadata. |
| `SourceN`, `PatchN` | Upstream inputs and downstream patches. |
| `BuildRequires` | Build-environment dependencies. |
| `Requires`, `Recommends`, `Suggests`, `Provides`, `Conflicts`, `Obsoletes` | Runtime capability and transition relationships. |
| `%description` | Long description. |
| `%prep` | Unpack and patch, commonly through `%autosetup`. |
| `%build` | Compile. |
| `%install` | Stage files under `%{buildroot}` rather than writing to the host root. |
| `%check` | Test staged software. |
| `%files` | Authoritative payload/ownership list, with `%doc`, `%license`, `%config`, `%ghost` and file attributes. |
| `%pre`, `%post`, `%preun`, `%postun`, `%pretrans`, `%posttrans` | Lifecycle scriptlets. |
| `%changelog` | Packaging history, depending on distribution practice. |

RPM macros are textual/programmatic abstractions such as `%{name}`, `%{version}`, `%{_bindir}`, `%{_libdir}`, `%cmake`, `%meson`, `%pyproject_wheel` and distribution conditionals. They prevent hard-coded paths and encode distribution build policy, but they also make a spec’s behavior dependent on the target distribution’s macro set. [web:166][web:167][web:173]

`%files` is not merely a list to archive: it establishes ownership, permissions, configuration-file behavior and subpackage boundaries. [web:177]

RPM scriptlet ordering on upgrade installs the new version before erasing the old one, interleaved with triggers; Fedora documents a sequence involving new `%pre/%post`, trigger actions, old `%preun/%postun`, then `%posttrans`. [web:424] As with Debian scripts, arbitrary scriptlets weaken atomicity and make failure recovery difficult.

## 2.2 DNF and `libsolv`

DNF is the repository/dependency layer above RPM. It uses `libsolv`, whose model translates package requirements and capabilities into a satisfiability problem. `libsolv` uses a SAT-style algorithm and compact indexed representation rather than the older ad-hoc dependency walking characteristic of early package managers. [web:334][web:341]

The important RPM concept is **capability resolution**. Dependencies need not be literal package names: RPM automatically derives capabilities such as shared-library SONAME requirements and provides, interpreter dependencies and architecture-qualified capabilities. That is powerful but makes simplistic cross-format name translation unreliable.

## 2.3 Delta RPMs: technically interesting, operationally retired in Fedora

A delta RPM encoded differences from an installed RPM to a new one; the client reconstructed the complete target RPM locally. Fedora disabled generation and DNF/DNF5 use by default in Fedora 40 because deltas existed for too few upgrades, reconstruction could fail and force a full redownload, and delta metadata was paid by all clients—including GUI and OSTree clients that did not use it. Fedora’s measurements found average savings of 8%, but failed reconstructions wasted an average 52.7 MB. [web:331]

This is a useful lesson for a universal distributor: binary deltas are beneficial only when the publisher knows the exact source version, compression and payload, and when metadata overhead plus reconstruction failure is less costly than modern content-addressed or chunked transport.

## 2.4 Modularity’s failure

Fedora Modularity attempted to ship multiple coordinated streams of RPMs, such as different language/runtime versions, using Module Build Service and modular repository metadata. Fedora retired it for Fedora 39+: there were few modules, no active maintainers and a persistent infrastructure problem building modules. Fedora explicitly offered no direct replacement in the retirement proposal. [web:335]

The failure illustrates that adding another dependency dimension—package version plus module stream plus profile plus context—raises the cognitive and infrastructure cost for maintainers, repositories and solvers.

## 2.5 Fedora versus RHEL

| Area | Fedora | RHEL |
|---|---|---|
| Release cadence | Approximately every six months, with about 13 months of package maintenance. [web:290] | Long enterprise lifecycle with compatibility commitments. |
| Package freshness | Tracks newer toolchains, kernels and desktop stacks; suitable as an upstream proving ground. | Holds major versions relatively stable and selectively backports fixes. |
| Update policy | Stable releases aim primarily at fixes, although selected packages receive larger updates. | Prefers backporting security and bug fixes into the shipped version rather than introducing new feature releases. [web:293][web:298] |
| ABI target | Changes more frequently between Fedora releases. | Long-lived ABI/API and certification expectations. |
| Packaging consequences | Rebuild against each Fedora release; fast macro and compiler evolution. | Older compiler/library baseline, downstream backports and conditionals, with stricter supportability requirements. |
| Additional repositories | Fedora repositories and COPR. | Base/AppStream/CodeReady repositories and often EPEL, whose community lifecycle differs from supported RHEL content. |

A single “RPM” is therefore not universally suitable for Fedora, RHEL, openSUSE and Amazon Linux. They share a container format but not dependency names, macro APIs, filesystem policy, library baselines or lifecycle guarantees.

---

# 3. Other targets a universal packager must handle

## 3.1 OS and application formats

| Target | Artifact and build description | Distribution/runtime model | Critical obligations and traps |
|---|---|---|---|
| Alpine `apk` | APK v2 has three concatenated gzip streams containing signatures, control information and package data; metadata lives in `.PKGINFO`. `APKBUILD` is a shell-like recipe consumed by `abuild`, with `prepare`, `build`, `check`, `package`, subpackages and dependency classes. [web:211][web:155] | `apk` resolves repository constraints; Alpine commonly uses musl and BusyBox. | Rebuild for musl; map dependencies and service integration to Alpine/OpenRC; do not relabel a glibc binary as an Alpine package. |
| Arch `pacman` | `.pkg.tar.zst` is a compressed tar archive with `.PKGINFO`, `.BUILDINFO`, `.MTREE`, optional scripts and detached OpenPGP signature. `PKGBUILD` is executable Bash interpreted by `makepkg`. [web:152][web:219] | Rolling-release repositories; simple packaging closely follows current upstream. AUR distributes recipes rather than trusted official binaries. | Test continually against moving dependencies; PKGBUILDs execute code; target official repository, private repository and AUR workflows separately. |
| Homebrew formula | Ruby DSL recipe for source builds, dependencies, tests and installation into a versioned Cellar “keg.” | Homebrew can pour a prebuilt **bottle** instead of building. [web:159][web:165] | Bottle relocation, macOS version/architecture tags, linkage auditing, Cellar-prefix assumptions and tap review policy. |
| Homebrew cask | Ruby DSL that downloads and installs an upstream-signed prebuilt app, PKG, font or binary. | Installer orchestration rather than source compilation. | URL/version/hash maintenance, app names, uninstall/zap behavior, upstream signing and auto-update semantics. |
| Windows MSI | Relational Windows Installer database containing products, features, components, resources and action sequences. | Windows Installer tracks components and can commit or roll back a multi-package transaction. [web:54] | Stable component GUID and key-path rules, upgrade/product codes, 32/64-bit registry views, per-user/per-machine scope, reboot handling and dangerous custom actions. |
| Windows MSIX | Modern signed application package derived from AppX technologies, with manifest-declared identity and capabilities. [web:48] | Registration, container/virtualization, block-aware updates and relatively clean uninstall. | Signing identity must match publisher; restricted filesystem/registry behavior; traditional drivers and many service patterns are unsuitable. |
| WinGet | Not an installer format. YAML manifests describe installer URL, SHA-256, architecture, type, silent switches, scope and upgrade behavior. WinGet can drive MSI, MSIX, EXE, ZIP and other installers. [web:301][web:302] | Discovery and orchestration over vendor-hosted installers or Store packages. | Generate and maintain manifests separately from the underlying installer; handle changing URLs, return codes, silent flags and repository validation. |
| Chocolatey | NuGet-style `.nupkg` with XML `.nuspec`, commonly plus `chocolateyInstall.ps1`, uninstall/before-modify scripts and embedded or downloaded installers. [web:49][web:53] | PowerShell-driven package management, public or private feeds. | Script idempotency, checksums, proxy/TLS behavior, unattended switches and duplicated state between Chocolatey and the underlying MSI/EXE. |
| macOS `.pkg` | Installer package, commonly built with `pkgbuild`/`productbuild`; may carry payloads, scripts and distribution choices. | Apple Installer writes system locations and records receipts. | Use a Developer ID Installer certificate; scripts need careful permissions and upgrade behavior. |
| macOS `.dmg` | Mountable disk image, normally containing an `.app` and `/Applications` alias. It is a delivery container, not intrinsically an installer. | User performs drag-and-drop or launches an enclosed installer. | Preserve app-bundle layout, resource forks and signatures; mount/eject UX differs from package installation. |
| macOS signing/notarization | Sign nested Mach-O code and bundles with Developer ID, produce ZIP/PKG/DMG, submit with `notarytool`, then staple the ticket where supported. Apple accepts ZIP, PKG and DMG for notarization. [web:31][web:34] | Gatekeeper evaluates identity, hardened runtime, entitlements and notarization. | Sign nested code correctly before the enclosing bundle; use the right Application versus Installer certificate; signing must happen after all binary mutation. |
| Flatpak | Manifest builds an application against a named runtime; application and runtime are OSTree objects/refs, or OCI/single-file bundles. | Distribution-independent runtimes, per-app sandbox, explicit permissions and XDG portals. Multiple runtime versions coexist. [web:81][web:82] | Desktop-oriented—not a general host service/package replacement. Select and maintain runtime branches, bundle absent libraries, declare exports and minimize permissions. |
| Snap | Immutable SquashFS with `snap.yaml` metadata, applications/services, plugs and slots. | `snapd` mounts the image, applies confinement through namespaces/AppArmor/seccomp, and supports transactional refresh and rollback. [web:83][web:89] | Store assertions, confinement interfaces, writable data locations, daemon dependence and classic-confinement review. |
| AppImage | Executable runtime prepended to a SquashFS application filesystem; mounted and launched without installation. | Usually no central resolver, mandatory daemon or default sandbox. [web:79] | Bundle everything except deliberately assumed base interfaces; use old-enough glibc; handle FUSE/extraction fallback, desktop integration, signatures and updates separately. |

## 3.2 Language package managers

These solve a different problem from OS packages. They normally manage project-level libraries or application source artifacts, not host users, services, SELinux policy, boot integration or system-wide ownership.

| Ecosystem | Package and dependency model | Native-code portability issue | Universal-packager implication |
|---|---|---|---|
| npm | Registry tarball controlled by `package.json`; semver graph installed under `node_modules`. `package-lock.json` records the exact resolved tree for repeatable installs. [web:121][web:123] | Native addons depend on Node ABI/N-API, compiler, libc and platform. Install scripts execute arbitrary code. | Prefer locked, offline installs; distinguish pure JS from prebuilt/native addons; do not rely on npm lifecycle scripts during privileged OS installation. |
| Python/pip | Source distributions and ZIP-format wheels with `{python tag}-{ABI tag}-{platform tag}` and `.dist-info` metadata. | `manylinux_x_y` defines a glibc baseline; `musllinux` is distinct. A generic `linux_x86_64` wheel is not broadly portable. Manylinux wheels target a restricted common userspace ABI. [web:126][web:133] | Build one wheel per Python/ABI/platform combination, or use pure-`py3-none-any`; audit bundled libraries and avoid conflicts with distro Python policy. |
| Cargo/crates.io | `.crate` source archive plus `Cargo.toml`; semver resolver writes exact selected versions to `Cargo.lock`. [web:242][web:244] | Build scripts and `-sys` crates may require C libraries, `pkg-config`, bindgen or target-specific linkers. Rust binaries are not automatically libc-independent. | Vendor sources for offline/reproducible OS builds and map native dependencies to the distribution; Cargo alone does not install services, manuals or system policy. |
| Go modules | Module identity and requirements in `go.mod`; `go.sum` authenticates direct and indirect module content; proxy and checksum-database protocols support reproducible retrieval. [web:316][web:317] | Pure-Go binaries are relatively portable, but cgo introduces compiler, libc and shared-library dependencies. Kernel/syscall assumptions still matter. | `CGO_ENABLED=0` can simplify deployment only when functionality permits; still package licenses, completion, services and configuration explicitly. |
| Maven | JAR/POM artifacts addressed by group ID, artifact ID, version, classifier and packaging; Maven automatically traverses transitive dependencies. [web:320][web:322] | JNI libraries, Java runtime versions, split packages and classpath/module conflicts remain platform-sensitive. | Use dependency locking/BOM policy and preferably a controlled runtime image; Maven coordinates are not mappings to Debian/RPM package names. |

---

# 4. Existing cross-format tools

| Tool | Outputs/model | Strengths | Fundamental limitations |
|---|---|---|---|
| **FPM — Effing Package Management** | Converts directories, tarballs, gems, Python/npm packages and existing packages into DEB, RPM, Pacman, FreeBSD, Solaris and macOS PKG targets. Can attach or strip lifecycle scripts. [web:62] | Very fast path from a staged installation tree to a native-looking package; useful for internal deployments and vendor CLI tools. | Conversion is structurally lossy: Debian and RPM dependency names, script phases, config-file semantics and policy are not isomorphic. It does not create a Debian archive-quality source package or perform distro-specific ABI integration. |
| **nFPM** | Single YAML definition generating DEB, RPM, APK, IPK, Arch packages and MSIX; implemented as a standalone Go tool. [web:63][web:68] | Reproducible, low-dependency release automation; strong fit for self-contained Go/Rust binaries. | Deliberately does not have every FPM feature or format. It packages already-built files; it is not a build farm, dependency solver, repository admission system or substitute for native policy. |
| **CPack** | Consumes CMake `install()` output and selects generators including DEB, RPM, AppImage, archives, NSIS/WiX and macOS DragNDrop/Bundle. [web:256][web:258] | Natural for CMake projects; supports components and generator-specific variables. | Generator portability is not artifact portability. Native tools may be platform-specific, and CPack’s generic metadata does not automatically produce high-quality Debian source packaging, Fedora-compliant specs, signing, notarization or repository publication. |
| **`jpackage`** | Bundles a Java application with a runtime image; creates `deb`, `rpm`, `pkg`, `dmg`, `msi`, `exe` or an application image. [web:199][web:202] | Excellent for self-contained desktop Java applications and runtime trimming through `jlink`. | Java-specific, often large, and explicitly **not cross-platform**: each native format must be built on its target platform. Signing, notarization, updates and store/repository submission remain external concerns. |
| **`cargo-deb`** | Reads Cargo metadata/assets and produces a binary Debian package; supports Debian maintainer scripts and systemd helper generation. [web:271][web:272] | Convenient for Rust executables and simple services. | Debian-only output, not a general distro backend. It does not replace a policy-compliant Debian source package, archive review, architecture build infrastructure or careful native-library dependency analysis. Workspace and cross-build edge cases have produced script and `dpkg-shlibdeps` problems. [web:276][web:283] |

The FPM author’s own motivation captures the real complaint: Ubuntu and CentOS used “two totally different package systems with completely different packaging policies and support tools,” and learning both was “painful and confusing.” [web:407] That is precisely what generic archive generators simplify—but they cannot erase the underlying policy differences.

---

# 5. Core hard problems

| Hard problem | Why it is technically difficult | Required strategy |
|---|---|---|
| ABI compatibility and symbol versioning | Package metadata may say “libc is installed” while the binary requires a newer `GLIBC_2.x`, `GLIBCXX_*`, C++ dual ABI, CPU ISA, plugin ABI or kernel interface. ELF symbol versioning lets libraries retain old implementations and allows the loader to reject missing required symbol versions, but only if maintainers preserve compatibility correctly. glibc uses compatibility symbols for this purpose. [web:3] | Build on the oldest supported ABI baseline; inspect `DT_NEEDED`, SONAMEs and version requirements with `readelf`/`objdump`; run ABI checks such as `abidiff`; test on every oldest-supported target, not only on the build host. |
| `RPATH`, `RUNPATH` and relocation | Absolute build paths break after installation. `DT_RPATH` is used only when `DT_RUNPATH` is absent and applies transitively; `DT_RUNPATH` follows `LD_LIBRARY_PATH` and applies only to direct `DT_NEEDED` children. `$ORIGIN` enables relative lookup but can create security or library-shadowing problems. [web:Keb6TQtApQK478MHfPfxmLm9] | Prefer normal system lookup for distro packages; for private application libraries use a narrow `$ORIGIN`-relative RUNPATH; strip staging/sysroot paths before signing; retest after tools such as `patchelf` mutate binaries. |
| glibc versus musl | musl aims for source compatibility with common interfaces, not drop-in glibc binary compatibility. The musl project states that all but the simplest glibc-linked applications generally fail if musl is substituted as the dynamic loader. [web:7] | Produce separate artifacts and dependency metadata; build and test natively against musl; use `musllinux` rather than `manylinux` wheels; avoid claiming Alpine support merely because the CPU architecture matches. |
| Package/file conflicts | Two packages can claim the same pathname, alternatives or service identity. A naive overwrite corrupts ownership and uninstall semantics. Debian needs `Replaces` plus usually `Breaks`; RPM/DNF rejects conflicting files unless policy explicitly permits shared identical content or alternatives. [web:347][web:97] | Maintain a per-target ownership map; split packages consistently; use alternatives/diversions sparingly; encode transitions from old package names and test upgrades from every supported predecessor. |
| Upgrade/downgrade transaction semantics | Dependency solving can be atomic as a plan while payload extraction and root-running scripts are not. A power failure or script error can leave half-installed packages, databases, restarted services or externally migrated schemas. DNF history can only undo when old packages remain available. [web:428] | Keep scripts idempotent and reversible where possible; retain old artifacts; separate schema migration from package install; use snapshots/offline updates for whole-system guarantees; test interruption at every lifecycle phase. |
| Sysroot versus host contamination | A sysroot relocates the compiler’s logical `/usr/include` and `/usr/lib` to a target tree. Build tools, generators and test executables still run on the host. Accidental host header or library discovery creates binaries that compile successfully but cannot run on the target. GCC defines `--sysroot` as the logical root for headers and libraries. [web:436] | Model build, host and target architectures separately; set compiler, linker, `pkg-config`, CMake find-root and emulator rules; use clean containers/chroots; audit paths and linkage after installation. |
| Cross-compilation | Projects often run generated target executables during configuration, probe host capabilities, download prebuilt host tools, or confuse build-time and target-time dependencies. Language-native extensions add more toolchains. | Use explicit toolchain files, target-aware dependency metadata and QEMU only where appropriate; distinguish build tools from target libraries; produce packages in target-native roots and run tests on real or emulated target systems. |
| Dependency-name translation | `openssl`, `libssl-dev`, `openssl-devel`, Homebrew `openssl@3` and a Maven/npm package are not equivalent abstractions. Package splits and virtual capabilities differ by release. | Maintain versioned distro-specific maps plus capability detection. Never infer a target dependency solely by rewriting a source package name. |
| Signing and trust | Archive signatures, repository metadata, Windows Authenticode/MSIX publisher identities, Apple nested-code signatures and notarization cover different objects and trust roots. Repacking usually invalidates signatures. | Sign only after final relocation/stripping; isolate signing keys; produce SBOMs and provenance; make signing/notarization/publishing separate target backend stages. |
| Policy and review | Technically valid packages can still be rejected for licenses, bundled libraries, filesystem paths, vendoring, weak cryptography, networked builds or missing source. | Treat Policy as executable lint plus human review. Generate native source recipes and patches, not only binary archives. |

---

# 6. Immutable and image-based systems change the unit of distribution

| System | Model | Effect on packaging |
|---|---|---|
| Fedora Silverblue / `rpm-ostree` | OSTree stores and deploys versioned filesystem trees. `/` and `/usr` are read-only; `/etc` and `/var` remain writable. Installing a layered RPM composes a new deployment that normally becomes visible after reboot. [web:192] | Traditional RPMs still exist as build inputs, but the user-facing transaction is an OS deployment. Host mutation scripts and assumptions about writable `/usr` become problematic; GUI apps move toward Flatpak and development tools toward containers. |
| OSTree deployments | Bootable trees are switched atomically; the previous deployment remains selectable and can be made permanent with `rpm-ostree rollback`. [web:451][web:465] | Rollback covers the deployment, not arbitrary database and user-data mutations under `/var`. Package migrations still need forward/backward compatibility. |
| `bootc` | Ships the operating system as an OCI/Docker image and performs transactional in-place OS updates. [web:184][web:189] | The primary release artifact becomes a bootable container image assembled in CI. RPM/Deb packages are often image-build inputs rather than independently resolved on each production host. |
| NixOS | Packages reside in immutable, content-derived `/nix/store` paths; profiles/configuration generations select sets of store objects. New versions do not overwrite old ones. | Side-by-side dependencies, atomic profile switching and rollback largely remove pathname ownership conflicts. Nix’s documentation explicitly describes upgrades as atomic because packages are added at new paths rather than overwritten. [web:182] |
| Conventional mutable distro | Package manager writes individual paths into the running root and invokes scripts against live state. | Small updates and local customization are convenient, but partial failure and state drift are much harder to eliminate. |

Immutable systems do **not** eliminate packaging. They move responsibility earlier:

- dependencies are resolved during image or derivation construction;
- integration tests should run against the complete composed image;
- update granularity becomes a tree, image or profile;
- local layering is possible but may reduce reproducibility;
- persistent data and schema migrations remain outside the atomic OS image and are still the dangerous boundary.

---

# 7. Practical architecture for a universal packaging system

A credible implementation should use a normalized intermediate representation, but preserve escape hatches for native semantics.

| Stage | Required function |
|---|---|
| Source normalization | Immutable source URI/hash, patches, license inventory, vendoring policy and reproducible environment. |
| Target matrix | Explicit OS release, architecture, libc, kernel baseline, runtime ABI and package-manager generation. |
| Hermetic build | Separate build/host/target dependencies; sysroot/toolchain declaration; no undeclared network or host filesystem access. |
| Staging manifest | Files, modes, owners, capabilities, config classification, services, users, icons, completions and license files. |
| ABI inspection | SONAME and symbol-version extraction, interpreter and RPATH validation, native-extension detection and CPU baseline checks. |
| Policy backend | Generate Debian source metadata/rules, RPM spec/macros, APKBUILD/PKGBUILD, Homebrew formula/cask, Windows/macOS manifests or sandbox declarations. |
| Lifecycle lowering | Convert abstract actions into systemd/tmpfiles/sysusers, Debian helpers, RPM macros, MSI tables or MSIX declarations; reject actions that cannot be translated safely. |
| Package tests | Fresh install, upgrade from all supported versions, downgrade, remove, purge, reinstall, interruption, service restart, file ownership and dependency closure. |
| Signing/publication | Target-native signature, repository metadata, SBOM, provenance, notarization and submission/review workflow. |
| Image composition | Optionally consume generated native packages into OSTree/bootc/container/Nix-style immutable outputs and test the resulting whole system. |

The central design rule should be: **share build intent and staged content, not the assumption that all package systems have equivalent semantics**. A generated `.deb`, RPM, MSIX and Flatpak may originate from the same release description, but each must remain an independently tested product.

Citations:
[16] https://www.debian.org/doc/debian-policy/ch-controlfields.html
[17] https://www.debian.org/doc/debian-policy/
[18] http://www.huge-man-linux.net/man1/dh.html
[19] https://www.debian.org/doc/debian-policy/policy.txt
[20] https://e50dfc11-2a89-4b9d-9f2b-c723c98d269d.filesusr.com/ugd/54794e_f136627196004fbb8436b366da691b6f.pdf?index=true
[21] https://www.debian.org/doc/debian-policy/ch-binary.html
[22] https://manpages.debian.org/testing/debhelper/dh.1.es.html
[23] https://manpages.debian.org/unstable/dpkg-dev/deb.5.en.html
[24] https://manpages.debian.org/testing/dpkg-dev/deb.5.en.html
[25] https://manpages.debian.org/buster/debhelper/dh.1.de.html
[26] https://www.debian.org/doc/debian-policy/ch-relationships.html
[27] https://manpages.debian.org/trixie/dpkg-dev/deb.5.en.html
[28] https://www.debian.org/doc/manuals/debian-handbook/sect.source-package-structure.ru.html
[29] https://manpages.debian.org/bookworm/dpkg-dev/deb.5.en.html
[30] https://ripe75.ripe.net/presentations/113-Debian-packaging-101.pdf
[136] https://github.com/Debian/apt/blob/main/doc/external-dependency-solver-protocol.md
[137] https://github.com/ParrotSec/apt/blob/master/doc/external-dependency-solver-protocol.md
[138] https://www.debian.org/doc/manuals/aptitude/ch02s03s02.ja.html
[139] https://lib.rs/crates/apt-edsp
[140] https://www.debian.org/doc/manuals/aptitude/ch02s03s01.en.html
[141] https://manpages.ubuntu.com/manpages/jammy/man1/apt-cudf.1.html
[142] https://www.debian.org/doc//manuals/aptitude/ch02s03.en.html
[143] https://manpages.ubuntu.com/manpages/focal/man1/apt-cudf.1.html
[144] https://www.debian.org/doc/manuals/debian-reference/ch02.en.html
[145] https://serverfault.com/questions/21105/whats-the-difference-between-apt-get-and-aptitude
[146] https://learnwithirfan.com/blog/ubuntu/apt-vs-apt-get-vs-aptitude/
[147] https://www.mancoosi.org/~abate/aptget-external-solvers-call-testers.html
[148] https://www.linuxjournal.com/content/debian-package-management-aptitude-vs-apt-get-ubuntu
[149] https://forums.debian.net/viewtopic.php?t=145091
[150] https://lists.debian.org/deity/2010/12/msg00059.html
[166] https://rpm-packaging-guide.github.io/
[167] https://rpm.org/docs/4.20.x/manual/spec.html
[168] https://docs.fedoraproject.org/en-US/packaging-guidelines/
[169] https://docs.redhat.com/en/documentation/red_hat_enterprise_linux/7/html/rpm_packaging_guide/advanced-topics
[170] https://docs.redhat.com/en/documentation/red_hat_enterprise_linux/7/epub/rpm_packaging_guide/working-with-spec-files
[171] https://docs.redhat.com/en/documentation/red_hat_enterprise_linux/7/html-single/rpm_packaging_guide/index
[172] https://jfearn.fedorapeople.org/en-US/RPM/4/html/RPM_Guide/ch-specfile-syntax.html
[173] https://docs.fedoraproject.org/en-US/packaging-guidelines/RPMMacros/
[174] https://docs.pagure.org/packaging-guidelines/Packaging:RPMMacros.html
[175] https://ftp.osuosl.org/pub/rpm/max-rpm/s1-rpm-specref-macros.html
[176] https://jfearn.fedorapeople.org/en-US/RPM/4/html/RPM_Guide/ch-specfiles.html
[177] https://jfearn.fedorapeople.org/en-US/RPM/4/html/RPM_Guide/ch08s02s03s06.html
[178] https://docs.redhat.com/en/documentation/red_hat_enterprise_linux/7/html/rpm_packaging_guide/packaging-software
[179] https://en.opensuse.org/openSUSE:Specfile_guidelines
[180] http://rpm-guide.readthedocs.io/en/latest/rpm-guide.html
[151] https://wiki.alpinelinux.org/wiki/APKBUILD_Reference
[152] https://wiki.archlinux.org/title/PKGBUILD
[153] https://man.archlinux.org/man/PKGBUILD.5
[154] https://wiki.archlinux.org/title/Arch_package_guidelines
[155] https://github.com/alpinelinux/abuild/blob/master/APKBUILD.5.scd
[156] https://github.com/alpinelinux/aports/blob/master/main/abuild/APKBUILD
[157] https://github.com/alpinelinux/abuild/blob/master/sample.APKBUILD
[158] https://deepwiki.com/alpinelinux/abuild/3.1-apkbuild-files
[159] https://docs.brew.sh/FAQ
[160] https://man.archlinux.org/man/PKGBUILD.5.en.raw
[161] https://github.com/github-linguist/linguist/blob/main/samples/Alpine%20Abuild/filenames/APKBUILD
[162] https://wiki.archlinux.jp/index.php/PKGBUILD
[163] https://docs.brew.sh/
[164] https://pkg.go.dev/gitlab.alpinelinux.org/alpine/go/apkbuild
[165] https://docs.brew.sh/Manpage
[46] https://docs.chocolatey.org/en-us/guides/create/create-script-package/
[47] https://learn.microsoft.com/en-us/windows/msix/packaging-tool/tool-overview
[48] https://learn.microsoft.com/en-us/windows/msix/
[49] https://community.chocolatey.org/courses/creating-chocolatey-packages/nuget-packages-and-nuspec
[50] https://help.puppet.com/pe/2023.8/topics/create_package_with_chocolatey.htm
[51] https://learn.microsoft.com/en-us/windows/package-manager/winget/
[52] https://docs.chocolatey.org/en-us/create/commands/pack/
[53] https://docs.chocolatey.org/en-us/create/create-packages/
[54] https://learn.microsoft.com/en-us/windows/win32/api/msi/nf-msi-msiendtransaction
[55] https://help.puppet.com/pe/2025.7/topics/create_package_with_chocolatey.htm
[56] https://docs.chocolatey.org/en-us/choco/commands/install/
[57] https://docs.chocolatey.org/en-us/guides/create/create-config-package/
[58] https://docs.chocolatey.org/en-us/create/create-packages-quick-start/
[59] https://winget.run/pkg/MasterPackager/MasterPackager
[60] https://blog.chocolatey.org/2025/02/using-chocolatey-for-more-than-software-installation/
[31] https://developer.apple.com/developer-id/
[32] https://developer.apple.com/documentation/xcode/packaging-mac-software-for-distribution
[33] https://stackoverflow.com/questions/77731278/how-to-distribute-custom-macos-app-without-app-store
[34] https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution
[35] https://www.dolthub.com/blog/2024-10-22-how-to-publish-a-mac-desktop-app-outside-the-app-store/
[36] https://www.augmentedmind.de/2021/06/13/distributing-macos-applications/
[37] https://www.rambo.codes/posts/2021-01-08-distributing-mac-apps-outside-the-app-store
[38] https://gist.github.com/IsmailAlamKhan/c13fde81044ac8930f328b98272e9d97
[39] https://cloudstreet-dev.github.io/macOS-and-Unix/part4-packages/native-installers.html
[40] https://pruizlezcano.github.io/blog/distributing-mac-app/
[41] https://developer.apple.com/macos/distribution/
[42] https://sparkles-docs.pages.dev/research/application-packaging/macos-dmg-pkg-xip
[43] https://dev.to/ajpagente/distributing-a-macos-app-outside-the-mac-app-store-433g
[44] https://www.reddit.com/r/MacOS/comments/fmcfsh/dmg_pkg_and_zip_differences/
[45] https://superuser.com/questions/73120/what-is-the-better-packaging-way-in-mac
[76] https://wiki.archlinux.org/title/Flatpak?useskin=vector
[77] https://deepwiki.com/flatpak/flatpak-docs/7.1-ostree-and-version-control
[78] https://www.denner.co/2026/03/24/flatpak4.html
[79] https://www.electron.build/docs/appimage/
[80] https://docs.flatpak.org/en/latest/flatpak-command-reference.html
[81] https://docs.flatpak.org/en/latest/basic-concepts.html
[82] https://fedoraproject.org/wiki/Workstation/Flatpaks
[83] https://events.canonical.com/event/35/contributions/292/attachments/61/104/talk-snap-2023.pdf
[84] https://manpages.ubuntu.com/manpages/mantic/man1/flatpak.1.html
[85] https://wiki.ubuntuusers.de/snap/
[86] https://events.canonical.com/event/89/contributions/481/attachments/251/361/Desktop%20Linux%20as%20easy%20as%20a%20smartphone%20-%20Just%20in%20a%20Snap!%20-%20OOS%202024.pdf
[87] https://github.com/flatpak/flatpak-docs/blob/master/docs/portal-docs.html
[88] https://github.com/flatpak/flatpak/wiki/Distribution
[89] https://snapcraft.io/docs/reference/system-architecture/
[90] https://kevinboone.me/appimage.html
[121] https://docs.npmjs.com/cli/v8/configuring-npm/package-lock-json/
[122] https://docs.npmjs.com/cli/v9/configuring-npm/package-lock-json/
[123] https://docs.npmjs.com/cli/v8/using-npm/registry/
[124] https://docs.npmjs.com/cli/v12/configuring-npm/package-json/
[125] https://daobook.github.io/packaging.python.org/specifications/platform-compatibility-tags.html
[126] https://peps.python.org/pep-0513/
[127] https://packaging.python.org/ja/latest/specifications/platform-compatibility-tags/
[128] https://github.com/ossf/package-manager-best-practices/blob/main/published/npm.md
[129] https://docs.npmjs.com/cli/v6/configuring-npm/package-locks/
[130] https://docs.npmjs.com/cli/v6/configuring-npm/package-lock-json/
[131] https://stackoverflow.com/questions/45022048/why-does-npm-install-rewrite-package-lock-json
[132] https://developers.redhat.com/blog/2019/09/27/python-wheels-ai-ml-and-abi-compatibility
[133] https://github.com/pypa/manylinux?tab=readme-ov-file
[134] https://peps.python.org/pep-0599/
[135] https://github.com/Oneflow-Inc/manylinux
[61] https://github.com/rcrowley/fpf
[62] https://github.com/jordansissel/fpm
[63] https://nfpm.goreleaser.com/docs/
[64] https://deepwiki.com/jordansissel/fpm/2.1-package-base-class
[65] https://goreleaser.com/customization/package/nfpm/
[66] https://github.com/jordansissel/fpm/wiki
[67] https://github.com/prof-milki/xpm
[68] https://nfpm.goreleaser.com/docs/cmd/nfpm_package/
[69] https://www.pantsbuild.org/2.25/reference/subsystems/nfpm
[70] https://www.pantsbuild.org/dev/reference/targets/nfpm_deb_package
[71] https://deepwiki.com/jordansissel/fpm
[72] https://www.pantsbuild.org/dev/reference/targets/nfpm_rpm_package
[73] https://fpm.readthedocs.io/en/v1.14.0/cli-reference.html
[74] https://aur.archlinux.org/packages/fpm
[75] https://github.com/Zoomdata/fpm-nodejs-new
[1] https://codebrowser.dev/glibc/glibc/include/shlib-compat.h.html
[2] https://howtech.substack.com/p/mastering-symbol-versioning-for-backward
[3] https://developers.redhat.com/blog/2019/08/01/how-the-gnu-c-library-handles-backward-compatibility
[4] https://gcc.gnu.org/onlinedocs/libstdc++/manual/abi.html
[5] https://gcc.gnu.org/onlinedocs/gcc-8.5.0/libstdc++/manual/manual/abi.html
[6] https://www.kernel.org/doc/Documentation/ABI/stable/vdso
[7] https://www.musl-libc.org/faq.html
[8] https://lists.debian.org/lsb-spec/1999/12/msg00017.html
[9] https://stackoverflow.com/questions/77516188/glibc-vs-musl-shared-binary-compatibility
[10] https://johannst.github.io/notes/development/symbolver.html
[11] https://konifay.io/blog/2026-0002-glibc-symbol-versioning/
[12] https://deepwiki.com/prefix-dev/rattler-build/5.4-binary-relinking
[13] https://gcc.gnu.org/onlinedocs/gcc-8.2.0/libstdc++/manual/manual/abi.html
[14] https://git.zx2c4.com/glibc/tree/abi-tags?id=acaee4d7aa686e058f3cce22fda157c572f4de2c
[15] http://54.37.137.89/pkgs.void/package/gcompat/
[91] https://oneuptime.com/blog/post/2026-03-04-rollback-package-updates-dnf-history-rhel-9/view
[92] https://discussion.fedoraproject.org/t/idea-a-public-log-of-rpm-ostree-update-changes/126478
[93] https://www.reddit.com/r/linuxquestions/comments/q6t2lf/how_do_rpm_transactions_work/
[94] https://muxup.com/building-testing-and-distributing-llvm-clang-and-friends
[95] https://tuxfixer.com/fedora-dnf-package-manager-rollback-rpm-update-removal-installation/
[96] https://autotools.info/pkgconfig/cross-compiling.html
[97] https://discussion.fedoraproject.org/t/transaction-test-error-with-different-packages/71921
[98] https://man.archlinux.org/man/extra/dnf/dnf4.8.en
[99] https://www.fedorafaq.com/en/dnf-history/
[100] https://software-dl.ti.com/processor-sdk-linux/esd/AM62X/10_01_10_04/exports/docs/linux/Overview/GCC_ToolChain.html
[101] https://lists.rpm.org/pipermail/rpm-maint/2008-March/002002.html
[102] https://docs.peridio.com/developer-reference/cross-compilation
[103] https://unix.stackexchange.com/questions/173811/how-do-i-fix-a-transaction-error
[104] https://www.fedorafaq.com/en/recover-from-failed-update/
[105] https://linuxcapable.com/fedora-dnf-command-line-cheat-sheet/
[181] https://releases.nixos.org/nix/nix-1.5.2/manual.pdf
[182] https://nixos.org/guides/how-nix-works/
[183] https://releases.nixos.org/nix/nix-1.8/manual.pdf
[184] https://bootc.dev/bootc/
[185] https://docs.redhat.com/en/documentation/red_hat_enterprise_linux/10-beta/html/using_image_mode_for_rhel_to_build_deploy_and_manage_operating_systems/managing-rhel-bootc-images
[186] https://releases.nixos.org/nix/nix-1.11.9/manual.pdf
[187] https://releases.nixos.org/nix/nix-1.11.5/manual.pdf
[188] https://blog.while-true-do.io/spotlight-fedora-silverblue/
[189] https://docs.fedoraproject.org/en-US/bootc/
[190] https://docs.fedoraproject.org/en-US/fedora-silverblue/_attachments/silverblue-cheatsheet.pdf
[191] https://zero-to-nix.com/concepts/nixos/
[192] https://docs.fedoraproject.org/en-US/fedora-silverblue/technical-information/
[193] https://discussion.fedoraproject.org/t/understanding-differences-between-rpm-ostree-and-opensuses-transactional-update/84289
[194] https://docs.fedoraproject.org/en-US/fedora-silverblue/reading-and-resources/
[195] https://nixos.wiki/wiki/Nix_package_manager
[106] https://www.debian.org/doc/debian-policy/ch-maintainerscripts.html
[107] https://www.debian.org/doc/debian-policy/_sources/ch-maintainerscripts.rst.txt
[108] https://stackoverflow.com/questions/22790927/sourcing-common-functions-in-debian-maintainer-scripts
[109] https://www.reddit.com/r/linuxquestions/comments/1g6svfd/why_wont_linux_foundation_standardize_application/
[110] https://serverfault.com/questions/11712/why-cant-linux-developers-create-a-universal-packaging-format
[111] https://lwn.net/Articles/770784/
[112] https://gadgetfee.com/apps-software-tips/i-hate-packaging-my-software-for-linux/
[113] https://www.reddit.com/r/linuxquestions/comments/r4bsc8/why_is_it_so_difficult_for_a_software_application/
[114] https://lists.debian.org/debian-user/2016/11/msg00922.html
[115] https://www.reddit.com/r/linux/comments/1acbeb0/how_hard_is_it_to_agree_on_a_single_app_format/
[116] https://zeli.app/en/story/49270764
[117] https://itsfoss.com/news/onepackage-announcement/
[118] https://wiki.debian.org/EmdebianAuditDetail
[119] https://docs.fedoraproject.org/en-US/packaging-guidelines/
[120] https://pagure.io/packaging-committee/issue/388
[331] https://fedoraproject.org/wiki/Changes/Drop_Delta_RPMs
[332] https://lwn.net/Articles/995337/
[333] https://frostyx.cz/posts/copr-modularity-the-end-of-an-era
[334] https://man.archlinux.org/man/extra/libsolv/libsolv.3.en
[335] https://fedoraproject.org/wiki/Changes/RetireModularity
[336] https://github.com/rpm-software-management/dnf/pull/2040
[337] https://man.archlinux.org/man/libsolv.3.en
[338] https://www.comss.ru/page.php?id=18556
[339] https://discussion.fedoraproject.org/t/dnf-drpm-not-needed/96648
[340] https://packages.fedoraproject.org/pkgs/dnf/python3-dnf/fedora-40.html
[341] https://lwn.net/Articles/503581/
[342] https://github.com/openSUSE/libsolv
[343] https://www.phoronix.com/news/MTgwNTY
[344] https://communityblog.fedoraproject.org/modularity-dead-long-live-modularity/
[345] https://packages.fedoraproject.org/pkgs/libsolv/libsolv/index.html
[286] https://docs.fedoraproject.org/en-US/fesco/Updates_Policy/
[287] https://www.reddit.com/r/Fedora/comments/1f41r4o/fedora_update_policy_within_a_release/
[288] https://discussion.fedoraproject.org/t/puzzled-by-fedoras-policy-in-updating-packages-within-a-stable-release/140110
[289] https://docs.fedoraproject.org/en-US/package-maintainers/Package_Update_Guide/
[290] https://docs.fedoraproject.org/en-US/releases/lifecycle/
[291] https://fedoraproject.org/wiki/UpdatePolicy(draft)
[292] https://docs.fedoraproject.org/en-US/council/archive/board_stable_updates_vision/
[293] https://access.redhat.com/solutions/57665
[294] https://fedoraproject.org/wiki/Stable_Release_Updates_Proposal
[295] https://docs.fedoraproject.org/en-US/council/historical/board_stable_updates_vision/
[296] https://discussion.fedoraproject.org/t/what-is-fedoras-update-philosophy/74258
[297] https://www.linuxcompatible.org/story/kernels-openshift-and-core-packages-updates-for-rhel/
[298] https://www.redhat.com/en/blog/what-backporting-and-how-does-it-apply-rhel-and-other-red-hat-products
[299] https://en.wikipedia.org/wiki/Red_Hat_Enterprise_Linux
[300] https://discussion.fedoraproject.org/t/difference-between-system-version-point-release-update-and-regular-package-updates/87700
[256] https://cmake.org/cmake/help/latest/manual/cpack-generators.7.html
[257] https://man.archlinux.org/man/extra/cmake/cpack-generators.7.en
[258] https://cmake.org/cmake/help/book/mastering-cmake/chapter/Packaging%20With%20CPack.html
[259] https://manpages.debian.org/bullseye/cmake-data/cpack-generators.7.en.html
[260] https://man.archlinux.org/man/cpack-generators.7.en
[261] https://www.wasilzafar.com/pages/series/cmake-mastery/cmake-mastery-part22-cpack.html
[262] https://cmake.org/cmake/help/latest/cpack_gen/deb.html
[263] https://github.com/olibre/cmake-tutorial/blob/master/make_tut/mysql-connector-cpp/CPackConfig.cmake
[264] https://github.com/mgalloy/cpack-cheatsheets/blob/master/cpack-variables.rst
[265] https://manpages.ubuntu.com/manpages/trusty/man1/cpack.1.html
[266] https://stackoverflow.com/questions/46011440/cmake-cpack-preferred-package-generators-for-different-platforms
[267] https://github.com/intel-iot-devkit/tinyb/blob/master/cmake/modules/CPackDeb.cmake
[268] https://cmake.org/cmake/help/latest/cpack_gen/rpm.html
[269] https://github.com/eclipse-upm/upm/blob/master/cmake/modules/CPackDeb.cmake
[270] https://gitlab.kitware.com/cmake/cmake/-/blob/master/Modules/CPack.cmake
[196] https://docs.oracle.com/en/java/javase/25/docs/specs/man/jpackage.html
[197] https://docs.oracle.com/en/java/javase/14/docs/specs/man/jpackage.html
[198] https://docs.oracle.com/en/java/javase/24/docs/specs/man/jpackage.html
[199] https://docs.oracle.com/en/java/javase/26/docs/specs/man/jpackage.html
[200] https://docs.oracle.com/en/java/javase/17/docs/specs/man/jpackage.html
[201] https://docs.oracle.com/en/java/javase/21/docs/specs/man/jpackage.html
[202] https://docs.oracle.com/en/java/javase/26/jpackage/packaging-overview.html
[203] https://dzone.com/articles/make-your-own-msi-and-rpm-deb-amp-dmg-with-jdk14
[204] https://openjdk.org/jeps/392
[205] https://mail.openjdk.org/pipermail/core-libs-dev/2021-July/080171.html
[206] https://docs.oracle.com/en/java/javase/16/docs/specs/man/jpackage.html
[207] https://bugs.java.com/bugdatabase/view_bug.do?bug_id=JDK-8237490
[208] https://docs.oracle.com/en/java/javase/18/docs/specs/man/jpackage.html
[209] https://stackoverflow.com/questions/60953329/code-signing-notarization-using-jpackage-utility-isnt-working-on-macos
[210] https://bugs.openjdk.org/browse/JDK-8238184
[271] https://docs.rs/crate/cargo-deb/latest/source/README.md
[272] https://github.com/kornelski/cargo-deb/blob/main/systemd.md
[273] https://github.com/kornelski/cargo-deb/blob/main/README.md
[274] https://www.debian.org/doc/debian-policy/ch-maintainerscripts.html
[275] https://github.com/kornelski/cargo-deb
[276] https://github.com/mmstick/cargo-deb/issues/151
[277] https://github.com/kornelski/cargo-deb/issues
[278] https://docs.rs/crate/cargo-deb/1.28.1/source/README.md
[279] https://www.debian.org/doc/manuals/maint-guide/maint-guide.en.txt
[280] https://github.com/mmstick/cargo-deb/issues/100
[281] https://www.debian.org/doc/debian-policy/policy.txt
[282] https://unix.stackexchange.com/questions/630436/debian-build-depends-on-non-package-binary
[283] https://github.com/kornelski/cargo-deb/issues/21
[284] https://www.debian.org/doc/manuals/debmake-doc/ch06.en.html
[285] https://wiki.debian.org/Teams/RustPackaging/Policy
[241] https://docs.rs/cargo
[242] https://doc.rust-lang.org/cargo/reference/resolver.html
[243] https://doc.rust-lang.org/nightly/cargo/CHANGELOG.html
[244] https://doc.rust-lang.org/cargo/guide/dependencies.html
[245] https://rustwiki.org/en/cargo/reference/registries.html
[246] https://github.com/rust-lang/cargo/blob/master/src/cargo/ops/lockfile.rs
[247] https://deepwiki.com/rust-lang/cargo/3.3-lockfile-management
[248] https://doc.rust-lang.org/cargo/CHANGELOG.html
[249] https://doc.rust-lang.org/cargo/commands/cargo-generate-lockfile.html
[250] https://rust.googlesource.com/cargo/+/582f0cdbc78185eb2a499b3cf274945aaa299469/ARCHITECTURE.md
[251] https://ebarnard.github.io/2019-06-03-rust-smaller-trait-implementers-docs/cargo/print.html
[252] https://docs.rs/cargo/latest/cargo/core/resolver/
[253] https://stackoverflow.com/questions/75884097/using-cargo-lock-to-effectively-manage-pull-dependency-versions-in-my-crates
[254] https://hackmd.io/@rust-cargo-team/B13O52Zko
[255] https://man.archlinux.org/man/cargo.1
[316] https://go.dev/ref/mod
[317] https://sum.golang.org/
[318] https://go.dev/blog/module-mirror-launch
[319] https://guides.dataverse.org/en/5.6/developers/dependencies.html
[320] https://maven.apache.org/guides/introduction/introduction-to-dependency-mechanism.html
[321] https://goframe.org/en/docs/install-go/go-module
[322] https://maven.apache.org/pom.html
[323] https://guides.dataverse.org/en/latest/developers/dependencies.html
[324] https://github.com/golang/go/wiki/Modules/6ae647c7e344bc643f9ab7478e9cdd77b8795f72
[325] https://guides.dataverse.org/en/6.7.1/developers/dependencies.html
[326] https://pkg.go.dev/cmd/go/internal/modfetch
[327] https://pkg.go.dev/github.com/jcmturner/go/src/cmd/go/internal/modfetch
[328] https://go.dev/src/cmd/go/testdata/script/mod_sumdb_golang.txt
[329] https://stackoverflow.com/questions/36559692/maven-install-transitive-dependencies
[330] https://groups.google.com/g/golang-announce/c/0wo8cOhGuAI
[346] https://www.debian.org/doc/debian-policy/ch-controlfields.html
[347] https://www.debian.org/doc/debian-policy/ch-relationships.html
[348] https://www.debian.org/doc/manuals/debian-faq/pkg-basics.en.html
[349] https://www.debian.org/doc/debian-policy/
[350] https://www.debian.org/doc/manuals/debian-reference/ch02.en.html
[351] https://www.debian.org/doc/manuals/debian-faq/pkg-basics.ko.html
[352] https://www.debian.org/doc/debian-policy/policy.pdf
[353] https://www.debian.org/doc/manuals/debian-faq/pkg-basics.ja.html
[354] https://www.debian.or.jp/community/devel/debian-policy-ja/policy.ja.html/ch-relationships.html
[355] https://man7.org/linux/man-pages/man5/deb-triggers.5.html
[356] https://www.opennet.ru/man.shtml?topic=deb-control&category=5&russian=0
[357] https://www.thecodeship.com/gnu-linux/understanding-apt-cache-depends-output/
[358] https://www.opennet.ru/man.shtml?topic=deb-control&russian=2
[359] https://man.cx/deb-triggers(5)/de
[360] https://wiki.debian.org/RelacionPaquetes
[361] https://www.debian.org/doc/debian-policy/ch-source.html
[362] https://www.debian.org/doc/debian-policy/ap-pkg-sourcepkg.html
[363] https://www.debian.org/doc/debian-policy/policy.pdf
[364] https://manpages.debian.org/testing/debhelper/dh.1.en.html
[365] https://www.debian.org/doc/manuals/debian-faq/pkg-basics.en.html
[366] https://manpages.debian.org/trixie/debhelper/dh.1.en.html
[367] https://www.debian.org/doc/debian-policy/ch-controlfields.html
[368] https://manpages.debian.org/jessie/debhelper/dh.1.en.html
[369] https://manpages.debian.org/buster/dpkg-dev/dpkg-source.1.en.html
[370] https://www.debian.org/doc/debian-policy/
[371] https://manpages.debian.org/stretch/dpkg-dev/dpkg-source.1.en.html
[372] https://manpages.debian.org/buster/debhelper/dh.1.en.html
[373] https://www.debian.org/doc/packaging-manuals/upgrading-checklist.txt
[374] https://www.debian.org/doc/debian-policy/ch-relationships.html
[375] https://lists.debian.org/debian-devel-announce/2024/04/msg00003.html
[226] https://lists.debian.org/debian-kernel/
[227] https://lists.debian.org/debian-devel/2018/04/msg00185.html
[228] https://lists.debian.org/debian-devel/2018/04/msg00172.html
[229] https://release.debian.org/bullseye/FAQ.html
[230] https://lists.debian.org/debian-devel-announce/2026/01/msg00010.html
[231] https://lists.debian.org/debian-devel-announce/2025/12/msg00000.html
[232] https://www.debian.org/doc/developers-reference/pkgs.html
[233] https://release.debian.org/testing/FAQ.html
[234] https://lists.debian.org/bugs.html
[235] https://release.debian.org/forky/FAQ.html
[236] https://unix.stackexchange.com/questions/52699/how-soon-do-new-releases-get-packaged-into-debian-testing
[237] https://lists.debian.org/debian-devel-announce/2005/08/msg00011.html
[238] https://lists.debian.org/debian-devel/2018/12/msg00297.html
[239] https://lists.debian.org/debian-ai/
[240] https://lists.debian.org/debian-ai/2025/04/msg00108.html
[301] https://learn.microsoft.com/en-us/windows/package-manager/package/manifest
[302] https://github.com/microsoft/winget-pkgs/blob/master/doc/manifest/schema/1.6.0/installer.md
[303] https://github.com/microsoft/winget-pkgs/blob/master/doc/manifest/schema/1.2.0/installer.md
[304] https://deepwiki.com/microsoft/winget-pkgs/3.1.1-manifest-schema-reference
[305] https://deepwiki.com/microsoft/winget-pkgs/1-overview
[306] https://deepwiki.com/microsoft/winget-pkgs/3.1-manifest-structure-and-format
[307] https://msendpointmgr.com/2026/07/30/is-winget-enterprise-ready/
[308] https://www.advancedinstaller.com/application-packaging-training/msix-packaging/ebook/winget-windows-package-manager.html
[309] https://readmex.com/en-US/microsoft/winget-pkgs/manifest-structure98cc0860-83b9-4c1f-a810-2873b235cfc8
[310] https://learn.microsoft.com/en-us/windows/win32/msi/roadmap-to-windows-installer-documentation
[311] https://learn.microsoft.com/en-us/windows/package-manager/winget/install
[312] https://pacesuite.com/blog/msix-limitations
[313] https://windowsforum.com/windows-news.4/winget-mastery-scriptable-fast-windows-app-management-with-winget.398062/
[314] https://blog.intunepckgr.com/2023/12/12/winget-updating-a-package/
[315] https://deepwiki.com/microsoft/winget-pkgs/3.5-manifest-authoring-tools
[211] https://wiki.alpinelinux.org/wiki/Apk_spec
[212] https://www.hydrogen18.com/blog/apk-the-strangest-format.html
[213] https://wiki.alpinelinux.org/w/index.php?title=Apkindex_format
[214] https://wiki.alpinelinux.org/wiki/Alpine_Package_Keeper
[215] https://wiki.alpinelinux.org/wiki/Apk
[216] https://github.com/kaniini/alpine-policy/blob/master/source/ch-binary.rst
[217] https://chimera-linux.org/docs/apk/world
[218] https://stackoverflow.com/a/78829048
[219] https://man.archlinux.org/man/alpm-package.7.en
[220] https://man.archlinux.org/man/apk-v2.5.en
[221] https://www.reddit.com/r/chimeralinux/comments/1p81mjn/what_compression_format_does_apk_package_use/
[222] https://stackoverflow.com/questions/59684341/alpine-linux-create-a-apk-package
[223] https://deepwiki.com/goreleaser/nfpm/2.2.3-apk-packager
[224] https://a.osmarks.net/content/serverfault.com_en_all_2022-11/questions/846141/what-does-alpine-s-apk-stands-for
[225] https://safeguard.sh/resources/blog/alpine-apk-security-model
[391] https://lwn.net/Articles/1017315/
[392] https://blog.jak-linux.org/2024/05/14/solver3/
[393] https://magenta.jak-linux.org/bin/solver3.pdf
[394] https://lwn.net/Articles/1017012/
[395] https://tracker.debian.org/media/packages/a/apt/changelog-3.0.3
[396] https://tracker.debian.org/media/packages/a/apt/changelog-3.2.0
[397] https://tracker.debian.org/media/packages/a/apt/changelog-3.1.8
[398] https://systemadministration.net/apt-3-debians-package-manager/
[399] https://www.aqee.net/post/whats-new-in-apt-3-0/
[400] https://tracker.debian.org/media/packages/a/apt/changelog-3.1.5
[401] https://tracker.debian.org/news/1529821/accepted-apt-293-source-into-unstable/
[402] https://thenewstack.io/debian-retools-apt-for-superior-dependency-management/
[403] https://www.phoronix.com/news/APT-2.9.3-New-Package-Solver
[404] https://www.theregister.com/2024/05/22/apt_gains_keepassxc_loses/
[405] https://github.com/Debian/apt/blob/main/doc/external-dependency-solver-protocol.md
[376] https://backports.debian.org/Instructions/
[377] https://backports.debian.org/
[378] https://backports.debian.org/Contribute/
[379] https://backports.debian.org/FAQ/
[380] https://www.debian.org/doc/manuals/developers-reference/developers-reference.en.html
[381] https://wiki.debian.org/Diaspora/Packaging/backporting
[382] https://www.debian.org/doc/manuals/debian-reference/ch02.en.html
[383] https://github.com/formorer/backports.debian.org/blob/master/Instructions.mdwn
[384] https://wiki.debian.org/Backports
[385] https://www.debian.org/doc/debian-policy/ch-controlfields.html
[386] https://wiki.debian.org/DebianGis/Repository
[387] https://wiki.debian.org/FastTrack
[388] https://wiki.debian.org/pt_BR/Backports
[389] https://linuxcapable.com/how-to-install-backports-experimental-repository-on-debian/
[390] https://www.debian.org/doc/manuals/debian-faq/getting-debian.en.html
[421] https://rpm.org/docs/6.1.x/man/rpm-scriptlets.7
[422] https://rpm-software-management.github.io/rpm/manual/tsort.html
[423] https://dnf.readthedocs.io/en/latest/command_ref.html
[424] https://docs.fedoraproject.org/en-US/packaging-guidelines/Scriptlets/
[425] https://rpm.org/wiki/Releases/4.10.0
[426] https://dl.acm.org/doi/10.5555/982972.982973
[427] https://www.fedorafaq.com/en/dnf-history/
[428] https://docs.redhat.com/ja/documentation/red_hat_enterprise_linux/10-beta/html/managing_software_with_the_dnf_tool/handling-package-management-history
[429] https://www.fedorafaq.com/en/downgrade-package/
[430] https://stackoverflow.com/questions/28632274/rpm-package-creation-scriptlet-ordering-during-upgrade
[431] https://lists.rpm.org/pipermail/rpm-maint/2022-May/020642.html
[432] https://docs.redhat.com/en/documentation/red_hat_enterprise_linux/6/html/6.3_technical_notes/rpm
[433] https://www.linuxjournal.com/article/7034
[434] https://rpm.org/wiki/Releases/4.9.0
[435] https://docs.fedoraproject.org/jp/fedora/f37/system-administrators-guide/package-management/DNF/
[451] https://docs.fedoraproject.org/en-US/iot/applying-updates-UG/
[452] https://docs.redhat.com/en/documentation/red_hat_enterprise_linux_atomic_host/7/html/cli_reference/atomic_commands
[453] https://docs.fedoraproject.org/en-US/fedora-coreos/auto-updates/
[454] https://docs.fedoraproject.org/en-US/fedora-silverblue/updates-upgrades-rollbacks/
[455] https://docs.redhat.com/ja/documentation/red_hat_enterprise_linux_atomic_host/7/html/cli_reference/_atomic_commands
[456] https://projectatomic.io/docs/os-updates/
[457] https://travier.github.io/ostree/atomic-rollbacks/
[458] https://docs.fedoraproject.org/en-US/fedora-sericea/updates-upgrades-rollbacks/
[459] https://projectatomic.io/blog/2018/05/pinning-deployments-ostree-based-systems/
[460] https://docs.fedoraproject.org/en-US/iot/remove-layered/
[461] https://github.com/coreos/rpm-ostree
[462] https://github.com/coreos/rpm-ostree/blob/main/docs/index.md
[463] https://docs.redhat.com/en/documentation/red_hat_enterprise_linux_atomic_host/7/html/installation_and_configuration_guide/managing_atomic_hosts
[464] https://docs.bazzite.gg/Installing_and_Managing_Software/Updates_Rollbacks_and_Rebasing/rolling_back_system_updates/
[465] https://ostreedev.github.io/ostree/atomic-upgrades/
[436] https://gcc.gnu.org/onlinedocs/gcc/Directory-Options.html
[437] https://cmake.org/cmake/help/v3.0/manual/cmake-toolchains.7.html
[438] https://gcc.gnu.org/onlinedocs/gcc-4.8.0/gcc/Directory-Options.html
[439] http://wiki.osdev.org/Hosted_GCC_Cross-Compiler
[440] https://www.baeldung.com/linux/sysroot
[441] https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html
[442] https://www.faschingbauer.me/trainings/material/soup/linux/toolchain/cmake/cross.html
[443] https://cmake.org/cmake/help/v3.10/manual/cmake-toolchains.7.html
[444] https://cloud.tencent.com/developer/article/1729465
[445] https://www.wasilzafar.com/pages/series/cmake-mastery/cmake-mastery-part25-cross-compilation.html
[446] https://discourse.cmake.org/t/linker-configuration-in-toolchain-file-for-cross-compiling/14192
[447] https://codecat.tistory.com/entry/sysroot
[448] https://www.peak-system.com/produktcd/Develop/Microcontroller%20hardware/PEAK-DevPack/Compiler/10%202020-q4-major/share/doc/gcc-arm-none-eabi/html/gccinstall/configure.html
[449] https://cmake.org/cmake/help/book/mastering-cmake/chapter/Cross%20Compiling%20With%20CMake.html
[450] https://www.smallstepsystems.com/cmake-cross-compilation-based-on-yocto-sdk-69076183a6eccd001b7aa599/
[406] https://www.reddit.com/r/linuxmasterrace/comments/iwyklu/why_is_package_management_such_a_mess/
[407] https://github.com/jordansissel/fpm/blob/main/README.rst
[408] https://forums.developer.apple.com/forums/thread/705967
[409] https://forums.developer.apple.com/forums/thread/736488
[410] https://www.debian.org/doc/debian-policy/policy.txt
[411] https://forums.developer.apple.com/forums/thread/764746
[412] https://developer.apple.com/documentation/security/resolving-common-notarization-issues
[413] https://www.debian.org/doc/debian-policy/
[414] https://www.debian.org/doc/manuals/developers-reference/ch03
[415] https://github.com/jordansissel/fpm
[416] http://www.fifi.org/doc/debian-policy/policy.html/index.html
[417] https://www.socallinuxexpo.org/scale12x/presentations/fpm-package-software-and-be-happy.html
[418] https://www.electron.build/docs/troubleshooting/
[419] https://unix.stackexchange.com/questions/764772/why-do-different-linux-distros-have-different-package-formats-and-package-manag
[420] https://stackoverflow.com/questions/75107171/issues-with-codesigning-and-notarization-for-mac-m1
