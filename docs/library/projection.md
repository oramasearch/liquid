# Package and image projection

Issues #28 and #78 implement projection from checked trees into `.deb`, `.rpm`, `.apk`, OCI image
layouts, Helm charts, OSTree commits, and bootc images. Projection does not convert one ecosystem
package into another: every writer consumes content-addressed trees and an explicit metadata
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

## Verification and peers

The committed install-tree fixture pins each target's digest in `tests/test_projection.cpp` and is
run by both static and shared suites on macOS and by the Linux proof lane. Tests cover the package
readers, format-registry recognition, deterministic gzip, repeated projection, OCI referrers,
signed `projection/1`, both OSTree repository modes, symbolic links, overlay paths, the independent
GVariant checksum, the bootc image digest, digest-only Helm values, and native `helm lint` where
Helm is installed.

`tools/projection-peers/run-projection-interop.sh` is the distribution-tool proof for `dpkg`,
`rpm`, `apk`, OCI import, and Helm/k0s. Its images are digest-pinned. The script exits with an
explicit skip when the required container runner is absent.
