# Package and image projection

Issues #28 and #80 implement Tier A projection from a result's checked install tree, or from an explicit
install-tree digest, into `.deb`, `.rpm`, `.apk`, OCI image layouts, and Helm charts. Projection
does not convert one ecosystem package into another: every writer consumes the same canonical
file tree and metadata request.

## Public contract

`liquid/projection.hpp` defines `projection_request`, `package_metadata`, service values, target
options (including OCI entrypoint and command vectors), and
`project(repository&, projection_request)`. Sources are result IDs or install-tree
digests. A bare tree requires an explicit architecture because no producing action exists to
supply it. A result supplies its action platform, output tree, source attestations, completion
epoch, and ABI verdicts.

The report returns artifact paths and SHA-256 digests, all source and projection attestation
digests, immutable image pins, derived and declared dependency labels, derived replacement
relationships and their paths, and whether an incompatible ABI was explicitly allowed. Projection
artifacts are ordinary files or OCI-layout directories;
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

## Install intent and generated scripts

`install_intent/1` is canonical CBOR containing services, users, groups, directories, symlinks,
cache updates, and alternatives. It has no command or shell field. Names match
`[a-z_][a-z0-9_-]*`; executable, home, directory, symlink, and alternatives paths are canonical
absolute paths containing no shell metacharacters. The public validator applies the same rules to
programmatic requests that the `.liquid` checker applies during lowering.

One intent generates four lifecycle scripts from fixed POSIX `sh` templates. Debian receives
`preinst`, `postinst`, `prerm`, and `postrm`; RPM receives the four `%pre`, `%post`, `%preun`, and
`%postun` header tags with `/bin/sh` interpreters; Alpine receives `.pre-install`,
`.post-install`, `.pre-deinstall`, and `.post-deinstall`. A single quoting function handles every
inserted value. User and group creation is guarded by `getent`, service operations are guarded by
`[ -d /run/systemd/system ]`, and cache tools are guarded by `command -v`. Repeating a script is
safe and converges on the same state.

Projection also adds the canonical intent below `usr/share/liquid/intents/` and generated systemd
units below `usr/lib/systemd/system/`. The SHA-256 digest of every generated script is recorded in
the package's signed `projection/1` statement and a package-subject `sbom/1` SPDX document.

## Ownership conflicts and replacement

The batch `project(repository&, vector<projection_request>)` prepares every request, indexes every
claimed package path, and refuses the batch with `errc::conflict` before writing artifacts. The
diagnostic groups every conflicting path by the two package names. A single request can set
`options.against` to a directory of `.deb`, `.rpm`, or `.apk` files, a dpkg status file with its
sibling `info/*.list` files, or a two-column RPM package/path dump.

Naming the existing owner in `package_metadata::replaces` permits those paths. Projection derives
the format relationship: Debian and RPM receive both conflict and replacement metadata, while
Alpine receives its replacement relationship. `projection_report::replacements` records the
package and exact paths that caused each derivation.

## Transactional projected-tree installation

`liquid install <package|install-tree> --root <dir>` extracts or links the input into
`<dir>/.liquid/generations/<n>.staging`, applies its embedded intent within that generation, renames
the completed directory to `<n>`, and atomically replaces `<dir>/current` with a relative symlink.
Regular files from a tree use same-filesystem hard links when available and copy otherwise;
package payloads are written with their recorded modes. Users and groups become generation-local
passwd/group entries, directories and symlinks stay below the generation, alternatives become
generation-local links, and services become generation-local systemd enablement links.

A process interruption before the final rename cannot expose staged bytes through `current`.
Stale `.staging` directories are discarded by the next install, while a fully staged generation
left before the switch remains complete and unselected. `liquid install --list --root <dir>` lists
complete generations and the selected one. `liquid rollback --root <dir>` atomically selects the
complete generation immediately preceding the current selection.

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

## Verification and peers

The committed install-tree fixture pins each target's digest in `tests/test_projection.cpp` and is
run by both static and shared suites on macOS and by the Linux proof lane. Tests cover the package
readers, format-registry recognition, deterministic gzip, repeated projection, OCI referrers,
signed `projection/1`, digest-only Helm values, and native `helm lint` where Helm is installed.
Committed script goldens cover all twelve lifecycle names. Property coverage refuses
metacharacters and preserves template structure for accepted values; package readers verify script
round trips and signed SBOM/projection binding. Ownership tests cover batches, package directories,
dpkg status, RPM dumps, and replacement derivation. Installer tests send `SIGKILL` after every
staged file and at the current-generation switch, prove the previous generation remains selected,
and complete the retry.

`tools/projection-peers/run-projection-interop.sh` is the distribution-tool proof for `dpkg`,
`rpm`, `apk`, OCI import, and Helm/k0s. Its images are digest-pinned. The script exits with an
explicit skip when the required container runner is absent.
