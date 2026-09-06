# Package and image projection

Issues #28, #76, #77, and #78 implement projection from a result's checked install tree, or from an
explicit install-tree digest, into `.deb`, `.rpm`, `.apk`, OCI image layouts, Helm charts, macOS
packages and disk images, Windows MSIX and winget manifests, Homebrew bottles, Arch recipes,
AppImages, Flatpak build directories, Snaps, OSTree commits, and bootc images. Projection does not
convert one ecosystem package into another: every writer consumes the same content-addressed file
tree and explicit metadata
request.

## Public contract

`liquid/projection.hpp` defines `projection_request`, `package_metadata`, service values, target
options (including OCI entrypoint and command vectors), and
`project(repository&, projection_request)`. Sources are result IDs or tree digests. Package, OCI,
and bootc targets require an architecture because no producing action exists to supply one for a
bare tree. A result supplies its action platform, output tree, source attestations, completion
epoch, and ABI verdicts.

OSTree and bootc requests treat the first source as the base root filesystem. Later sources are
overlays, mounted at each `projection_source::path`; later files replace the corresponding base
paths. The signed `projection/1` statement records the base digest and ordered overlay digests.
OSTree requests also select a ref and `archive-z2` or `bare-user` repository mode. Reports expose
the commit checksum separately from the projected artifact digest.

The report returns artifact paths and SHA-256 digests, all source and projection attestation
digests, immutable image pins, derived and declared dependency labels, and whether an incompatible
ABI was explicitly allowed. Projection artifacts are ordinary files or OCI-layout directories;
publication to a registry is outside this library operation.
Package identity fields must be safe single path components; relationship fields, target options,
and Helm values reject control characters before any archive, JSON, or YAML is written.

Platform targets also return whether each artifact is reproducible and one
`platform_signing_request` per unsigned installer. `sign_platform` consumes that request outside
projection and `verify_platform_signature` checks both its TAAP sidecar and the operating system's
native signature. A winget source is the filesystem path of an already-built MSIX because its
SHA-256 is part of the generated installer manifest.

## Package formats

The package writers and their bounded readers live under `src/formats/` and are registered with
the built-in format registry:

- Debian output is an owned `ar` writer containing `debian-binary`, `control.tar.zst`, and
  `data.tar.zst`. Control metadata includes `Depends`, `Pre-Depends`, `Recommends`, `Conflicts`,
  `Breaks`, `Replaces`, `Provides`, `md5sums`, and optional `conffiles`.
- RPM output contains the v3 lead, digest-bearing signature header, package and file tags, and a
  `cpio` newc payload compressed with zstd. Signing is represented by the TAAP attestation rather
  than an embedded OpenPGP signature.
- Alpine output is a deterministic pair of gzip members. Its `.PKGINFO` carries the SHA-256
  `datahash`, relationship fields, and `so:` dependencies.

Maintainer scripts and file-conflict transactions remain issue #80.

## Homebrew

The Homebrew target writes a formula, a source tarball, and a bottle. The bottle is the canonical
install tree below `<name>/<version>/`; both archives use the owned tar and deflate writers. The
formula records the source URL and SHA-256, declares `cellar: :any_skip_relocation`, installs the
staged tree into the keg, and tests that its first top-level `bin` program is executable. The
formula leaves the bottle root at Homebrew's default so `HOMEBREW_BOTTLE_DOMAIN` can select a local
or mirrored root without regenerating the formula.

The shared relocation check runs before the formula or bottle is written. A build-root string,
absolute runtime search path, absolute script interpreter, or other finding therefore refuses the
projection instead of weakening `:any_skip_relocation`. Submitting formulae to Homebrew core is
outside this operation.

## Arch PKGBUILD

The Arch target writes `PKGBUILD`, `.SRCINFO`, and a deterministic source tarball containing the
install tree below `<pkgname>-<pkgver>/`. `package()` copies that tree below `$pkgdir/usr` without
network access or a build phase. The recipe pins the archive SHA-256 and disables stripping so
the projected bytes remain the checked bytes.

ELF requirements use the same evidence as the native package writers. glibc names map to `glibc`,
the C++ and GCC support libraries map to `gcc-libs`, and unknown SONAMEs remain explicit
lower-case capabilities. Declared dependencies are additive and `.SRCINFO` repeats the complete
list consumed by `makepkg`.

## AppImage

The AppImage target writes a type 2 executable: the architecture runtime followed directly by the
owned SquashFS 4 image. The filesystem contains `AppRun`, the install tree below `usr/`, a desktop
file, and `.DirIcon`. `AppRun` uses the runtime-provided `APPDIR` and executes the first top-level
program in `bin`. x86-64 and AArch64 generation is available on Unix hosts; Windows returns an
explicit unsupported result because its build does not embed ELF runtimes.

The upstream [AppImage type 2 runtime](https://github.com/AppImage/type2-runtime) is a private
build input pinned in `CMakeLists.txt`:

| Architecture | URL | SHA-256 | Bytes |
|---|---|---|---:|
| x86-64 | `https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64` | `1cc49bcf1e2ccd593c379adb17c9f85a36d619088296504de95b1d06215aebbf` | 944632 |
| AArch64 | `https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-aarch64` | `7d5d772b7c32f0c84caf0a452a3072a5709027d7eac5856feb89a7a7a8881372` | 936456 |

The URL can advance, but a changed byte fails configuration until its provenance and hash are
reviewed together. The runtimes are embedded as read-only data; projection itself does not fetch
or invoke AppImage tooling.

## Flatpak

The Flatpak target writes `<name>-<version>.flatpak/manifest.json`, a `payload/` source directory,
and an export-ready `files/` plus `metadata` build directory. The manifest pins
`org.freedesktop.Platform` and `org.freedesktop.Sdk` to branch `24.08`, uses the simple buildsystem,
and copies only the emitted payload into `/app`. A generated desktop entry and icon accompany the
selected command.

The manifest text is deterministic. A Flatpak repository commit is intentionally not produced by
the library: `flatpak build-export` adds repository metadata through the installed Flatpak and
OSTree implementation. The optional `realworld` peer runs both the ready directory through
`flatpak build-export` and the manifest through `flatpak-builder --build-only` when the pinned
runtime is installed.

## Snap

The Snap target writes `snapcraft.yaml`, a `payload/` tree, a `prime/` tree ready for `snap pack`,
and an owned `.snap` SquashFS. `core24`, strict confinement, the target architecture, and the
selected `usr/bin` command are explicit. `prime/meta/snap.yaml` is the metadata consumed by
`snap pack`; the same `prime` content is passed to the deterministic SquashFS writer for the
reported `.snap` digest.

The peer runs `snap pack` and checks both images with `unsquashfs -l`. Installation is not claimed:
containers do not run the `snapd` mount, namespace, AppArmor, and refresh services required for a
real confined installation. Store assertions and submission to the Snap Store are outside scope.

## Reproducibility and detached attestations

| Output | Reproducible payload | Attachment |
|---|---|---|
| Homebrew bottle and source archive | yes | adjacent DSSE collection |
| Homebrew formula | yes | adjacent DSSE collection |
| `PKGBUILD`, `.SRCINFO`, and source archive | yes | adjacent DSSE collection |
| AppImage | yes for a fixed runtime pin and zstd version | adjacent DSSE collection |
| Flatpak manifest and build-tree files | yes | adjacent DSSE collection; exported OSTree commit is tool-owned |
| Snap recipe, prime tree, and owned `.snap` | yes | adjacent DSSE collection; `snap pack` output is tool-owned |

Each primary file has `<artifact>.attestations.json`. The collection names its SHA-256 subject and
contains the signed `projection/1` statement plus verified source provenance and SBOM DSSE
envelopes. TAAP signature timestamps mean detached collections produced at different signing
instants need not have the same bytes; they do not change the primary artifact digest. OCI and
Helm retain their subject-bound OCI 1.1 attachment mechanism.

## Derived dependencies and ABI refusal

Every regular file is passed through the ELF, Mach-O, and PE readers. Needed-library names become
package relationships; library identities become provided capabilities. Declared dependencies are
additive and appear with `declared: true` in the report.

| Evidence | Debian | RPM | Alpine |
|---|---|---|---|
| needs `libfoo.so.1` | mapped package name | `libfoo.so.1()(64bit)` on 64-bit targets | `so:libfoo.so.1` |
| provides `libfoo.so.1` | `libfoo.so.1` virtual | `libfoo.so.1()(64bit)` | `so:libfoo.so.1` |

The Debian mapping is deliberately small and explicit: `libc.so.6`, `libdl.so.2`,
`libm.so.6`, `libpthread.so.0`, and `librt.so.1` map to `libc6`; `libstdc++.so.6` maps to
`libstdc++6`; and `libgcc_s.so.1` maps to `libgcc-s1`. An unknown SONAME remains a lower-case
virtual dependency, so no guessed distribution package silently replaces binary evidence.

A source result with a verified `abi_compatibility/1` verdict of `incompatible` is refused.
`allow_abi_break` permits it and sets `abi_break_allowed` in the report and the CLI JSON.

## Byte identity and compression

All entries are sorted, timestamps use `SOURCE_DATE_EPOCH`, user and group IDs are zero, executable
and regular modes are normalised, and xattrs are omitted. zstd output uses the vendored 1.5.7
implementation with a fixed level, one worker, content size, and checksum. It is used for Debian,
RPM, and OCI by default.

Gzip-only output never calls zlib on the write path. `src/formats/deflate/` emits a fixed RFC 1951
stored-block stream, a zero-time gzip header, and an owned CRC-32 implementation. Alpine always
uses it; `compat_gzip` selects it for Debian control/data members and OCI layers. zlib is retained
only for bounded read-side inflation.

## OCI images and layer selection

An OCI target is an image layout containing `oci-layout`, `index.json`, and content-addressed
`blobs/sha256/`. The config records Linux, architecture, rootfs diff IDs, and epoch-derived created
and history times. Layers contain the final filesystem only, so projection never emits a whiteout.

When a stored build trace contains the selected result, its dependency edges assign actions by
distance from the selected action: transitive tool/runtime producers are the low-change layer,
direct project dependencies are the middle layer, and selected-action outputs are the high-change
layer. Files without a matching trace output use the stable install-tree categories `lib`, other,
and `bin`. Each category is independently sorted, and empty layers are omitted. The fallback makes
bare-tree requests deterministic while the trace path preserves useful cache boundaries for builds.

The image manifest is the subject of `projection/1`. Source `slsa_provenance/1` is exported as its
in-toto DSSE envelope; source SBOM envelopes carry the JSON document and its registered TAAP
statement; projection statements carry the registered TAAP statement.
Each is an OCI 1.1 artifact manifest with the image manifest as `subject`, and every descriptor is
listed in `index.json`. Registries without the referrers API can use the deterministic
`sha256-<manifest-hex>.<profile>` annotation as a fallback tag.

## OSTree repositories

`src/formats/ostree/` owns the writer; no OSTree or GLib library is linked. It serialises the fixed
OSTree GVariant schemas directly:

- file headers use `(uuuusa(ayay))`, with big-endian uid, gid, mode, an empty xattr array, and the
  symbolic-link target when present; the `.file` checksum covers the aligned length-prefixed
  header and regular-file content;
- `.dirtree` uses `(a(say)a(sayay))`, `.dirmeta` uses `(uuua(ayay))`, and `.commit` uses
  `(a{sv}aya(say)sstayay)` with the selected ref in `ostree.ref-binding`;
- `summary` uses `(a(s(taya{sv}))a{sv})` and points the selected ref at the commit.

Names and objects are sorted, uid and gid are zero, xattrs are empty, and the commit timestamp is
`SOURCE_DATE_EPOCH`. Regular-file and symbolic-link mode bits are retained. `archive-z2` writes
length-prefixed archive headers and deterministic raw DEFLATE payloads as `.filez`; `bare-user`
writes content plus `user.ostreemeta`. Both modes use `objects/xx/...`, `refs/heads/<ref>`,
`config`, and `summary`.

The committed fixture checksum
`01b3c73af4e373360c6fbb04258c53ed526ba89056c321e4f144a07edbec04b4` was calculated independently
with GLib's GVariant encoder and is pinned in the macOS and Linux tests.

## bootc images

The bootc target embeds the same commit in an OCI image using OSTree's `bare-split-xattrs` tar
layout below `sysroot/ostree/repo`. Content objects and their checked-out paths share a component
layer through tar hard links; symbolic links retain their targets. The metadata layer carries the
commit, dirtrees, dirmetas, repository configuration, and `ostree.encapsulated=true`. The base and
each overlay form independently compressed component layers named by `ostree.components`, so an
overlay change does not rewrite unrelated layers.

The config and manifest carry `containers.bootc=1`, `ostree.bootable=true`, `ostree.commit`,
`ostree.final-diffid`, and the component annotations defined by ostree-ext. Here, “bootable” means
the input tree claims a bootc-compatible userspace and the image contains a verifiable OSTree
commit. Kernel, initramfs, bootloader, signing beyond attached attestations, and actually booting a
k0s node remain their later phase work. The OCI manifest digest, including its commit and computed
layers, is the node image identity; `projection/1` and source evidence are attached with OCI 1.1
`subject` without changing it.

## Helm

Helm projection writes both a chart directory and an OCI chart artifact. A fixed template set
contains Deployment, Service, and ConfigMap templates. Request service values provide names,
ports, environment, volumes, and images from the current build. `values.yaml` represents every
image as `repository@sha256:digest`; mutable tags are not accepted or generated.

The OCI artifact uses the Helm config and content media types and receives the same subject-bound
attestation manifests. The full deployment graph remains Phase 7.

## Unsigned projection and platform signing

Platform delivery deliberately has two operations:

1. `project` materialises the checked install tree and produces an unsigned installer. Its
   `projection/1`, provenance, and SBOM evidence bind the reproducible unsigned digest. Projection
   runs platform tools as actions or through `runtime::offload`; it has no network or Keychain
   access.
2. `sign` verifies that unsigned digest, reaches identities only through custody URIs, invokes the
   native signer outside the sandbox, and writes a signed `platform_signature/1` sidecar. The new
   statement binds the unsigned digest to the shipped digest, signing-certificate fingerprint,
   native verification output, and notarisation ticket or timestamp response.

Changing the unsigned artifact after projection is refused. Changing the signed artifact or the
sidecar is refused by `sign verify`. The signer also refuses an output path that resolves to the
unsigned input, so a failed signing attempt cannot replace it. The signed digest is intentionally
different and is not described as a reproducible build output.

### macOS package and disk image

`macos-pkg` invokes Apple's `pkgbuild` and `productbuild` with `--timestamp=none`, a fixed
environment, fixed payload dates, sorted inputs, and stripped xattrs. `pkgutil --flatten` writes the
final Apple package. liquid canonicalises the outer XAR's volatile ownership, inode, device, and
time metadata, then recompresses only its XML table of contents and updates the XAR checksum. It
does not implement XAR, BOM, or cpio. The committed macOS fixture pins the unsigned digest and
projects twice to prove byte identity.

`macos-dmg` invokes `hdiutil` over the same tree. HFS+ allocation state is not reproducible, so its
report says `reproducible=false`; the package is the reproducible installer. A disk image is a
delivery convenience, not the source of the reproducibility claim.

`sign macos` accepts `keychain:<installer-identity>|<application-identity>`. The second name may be
omitted only when one test identity supports both purposes. Mach-O payload files are signed with
`codesign --options runtime`, the request's entitlements, and a trusted timestamp for a release.
The rebuilt package is signed with the Developer ID Installer identity through `productsign`.
With all three `LIQUID_APPLE_NOTARY_KEY`, `LIQUID_APPLE_NOTARY_KEY_ID`, and
`LIQUID_APPLE_NOTARY_ISSUER_ID` values present, the command submits with `xcrun notarytool --wait`,
staples the accepted ticket, and requires `spctl --assess --type install` to pass. The private API
key is written only to a mode-`0600` temporary file and is removed after submission. An incomplete
notary environment produces an explicit skipped result; `--no-notarize` is reserved for local test
identities.

The release notarisation gate is pending. No credentialed run, ticket ID, `spctl` result, or run
date is recorded yet. This paragraph must be replaced with that evidence after Steve supplies the
Developer ID and App Store Connect credentials; credential-free CI is not that gate.

### Windows MSIX and winget

`msix` writes a pinned `AppxManifest.xml`, correctly sized deterministic PNG assets, and a sorted
MakeAppx mapping, normalises every source timestamp, and invokes the Windows SDK's `makeappx pack`.
The package identity, publisher subject, display name, executable, version, and architecture come
from `package_metadata`. Windows CI signs it with a temporary certificate, installs it through
`Add-AppxPackage`, launches the installed executable, and requires `Get-AuthenticodeSignature` to
report `Valid`.

`winget` emits the version, installer, and default-locale YAML set. Its installer YAML uses the
HTTPS MSIX URL from `installer_url` and the exact uppercase SHA-256 of the supplied MSIX. Package
identifiers are validated before they become output filenames. Portable tests pin the full XML and
YAML bytes. MSI is not produced by this work.

`sign windows` accepts `file:<pfx-path>` or
`csp:<provider>|<key-container>|<certificate-path>`. It invokes `signtool sign /fd sha256`; when a
timestamp server is supplied it also passes `/tr <url> /td sha256`. It then requires
`signtool verify /pa` to pass. `LIQUID_WINDOWS_PFX_PASSWORD` supplies a PFX password without putting
it in a request file or output.

## Signing key custody per platform

No signing key or certificate password belongs in the repository. Steve controls the release
credentials and CI may receive them only as secret environment values. Commands never print secret
values, private keys, passwords, or full signer arguments.

| Credential | Custody and access | What it proves | What it does not prove |
|---|---|---|---|
| Developer ID Application key | macOS Keychain, or an HSM exposed through a Keychain-compatible provider; selected by the application part of `keychain:` | Apple issued the application certificate to the named team and the Mach-O bytes have not changed | Source review, reproducibility, or notarisation |
| Developer ID Installer key | macOS Keychain or compatible HSM; selected by the installer part of `keychain:` | Apple issued the installer certificate to the named team and the package bytes have not changed | That every payload binary has the right entitlement or that Apple accepted it |
| App Store Connect API key | Issuer ID and key ID in environment text; private `.p8` content in `LIQUID_APPLE_NOTARY_KEY` only | Authenticates a submission to Apple's notary service | The safety or provenance of submitted code |
| Authenticode key | `file:` for a locally protected PFX; `csp:` for hardware-token or cloud-HSM providers | The certificate holder signed the exact PE/MSIX bytes | Source review, reproducibility, or current certificate status without trust validation |
| Authenticode EV key | Hardware token or cloud HSM only; #81 tokens and #51 custody providers are the supported paths | The EV certificate holder signed the bytes without exporting the private key | That the build was benign or reproducible |
| RFC 3161 timestamp service | Public service URL, independently verified by `signtool` | The signature existed while its certificate was valid | Who built the unsigned artifact |

The `projection/1` and `platform_signature/1` chain supplies the missing build relationship. The
platform signature authenticates distribution bytes; TAAP binds those bytes back to the checked
unsigned artifact and its source evidence. #81 remains responsible for token drivers and
hardware-held signing operations; this work names their custody boundary without implementing
them.

## Verification and peers

The committed install-tree fixture pins each target's digest in `tests/test_projection.cpp` and is
run by both static and shared suites on macOS and by the Linux proof lane. The shared suite links
the private package, platform, tar-writer, and SquashFS-writer translation units directly,
preserving their internal ABI while exercising the same deterministic bytes. Tests cover the package
readers, format-registry recognition, deterministic gzip, repeated projection, OCI referrers,
signed `projection/1`, both OSTree repository modes, symbolic links, overlay paths, the independent
GVariant checksum, the bootc image digest, `platform_signature/1` schema, pinned Appx and winget
manifests, digest-only Helm values, and native `helm lint` where Helm is installed. The macOS and
Windows workflow steps add native projection, temporary-certificate signing, installation, launch,
and platform verification.

`tools/projection-peers/run-homebrew-interop.sh` creates a temporary tap, serves the bottle over
HTTP through `HOMEBREW_BOTTLE_DOMAIN`, pours it, and runs `brew test`. The broader peer runner adds
`makepkg` plus `pacman -U`, AppImage extraction on Ubuntu 24.04 and Fedora, Flatpak builder, and
Snap structure checks to the existing `dpkg`, `rpm`, `apk`, OCI, and Helm checks. Peer images are
digest-pinned and unavailable peers report an explicit skip.
