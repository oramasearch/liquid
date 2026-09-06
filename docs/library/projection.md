# Package and image projection

Issues #28 and #76 implement Tier A projection from a result's checked install tree, or from an
explicit install-tree digest, into `.deb`, `.rpm`, `.apk`, OCI image layouts, Helm charts, macOS
packages and disk images, and Windows MSIX and winget manifests. Projection does not convert one
ecosystem package into another: every writer consumes the same canonical file tree and metadata
request.

## Public contract

`liquid/projection.hpp` defines `projection_request`, `package_metadata`, service values, target
options (including OCI entrypoint and command vectors), and
`project(repository&, projection_request)`. Sources are result IDs or install-tree
digests. A bare tree requires an explicit architecture because no producing action exists to
supply it. A result supplies its action platform, output tree, source attestations, completion
epoch, and ABI verdicts.

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

The committed install-tree fixture pins each portable target and macOS package digest in
`tests/test_projection.cpp` and is run by both static and shared suites on macOS and by the Linux
proof lane. Tests cover the package
readers, format-registry recognition, deterministic gzip, repeated projection, OCI referrers,
signed `projection/1`, `platform_signature/1` schema, pinned Appx and winget manifests, digest-only
Helm values, and native `helm lint` where Helm is installed. The macOS and Windows workflow steps
add native projection, temporary-certificate signing, installation, launch, and platform
verification.

`tools/projection-peers/run-projection-interop.sh` is the distribution-tool proof for `dpkg`,
`rpm`, `apk`, OCI import, and Helm/k0s. Its images are digest-pinned. The script exits with an
explicit skip when the required container runner is absent.
