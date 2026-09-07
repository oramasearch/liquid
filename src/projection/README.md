# Projection implementation

`projection.cpp` validates sources, OS base and overlay paths, ABI verdicts, relocation,
action-platform metadata, build-trace layer evidence, and binary-derived dependencies. It also
signs and stores `projection/1`, including digest-pinned base and overlay tree claims for OS
targets.
`package.cpp` dispatches CPU work through `runtime::offload` to the registered Debian, RPM, and APK
writers. `rules.cpp` owns dependency capabilities and trace-distance layer assignment. `oci.cpp`
builds layouts, image layers, subject-bound attestation manifests, and indexes.
`debian_source.cpp` writes the Debian Tier B source tree, deterministic source archives, and `.dsc`;
its Build-Depends mapping comes from the pinned, peer-derived table under `fixtures/`.
`helm.cpp` materialises the fixed chart templates and the equivalent OCI chart artifact.
`platform.cpp` invokes Apple or Windows SDK packaging tools, pins the portable manifests, records
reproducibility, and emits unsigned signing requests. `platform_tools.cpp` is the captured native
process and file boundary. `signing.cpp` applies Developer ID or Authenticode outside projection,
writes `platform_signature/1`, and re-runs the native verifier.
`scripts.cpp` maps one validated install intent to the fixed Debian, RPM, and Alpine lifecycle
templates. `conflicts.cpp` indexes owned package paths across a request batch or an installed
inventory and derives explicit replacement relationships. Package projection embeds canonical
intent and service-unit files and binds generated script digests into `projection/1` and SPDX.
`homebrew.cpp` writes stable-gzip source and bottle archives plus their formula. `pkgbuild.cpp`
writes the Arch recipe, `.SRCINFO`, and source archive. `bundled.cpp` writes AppImage, Flatpak, and
Snap outputs with the owned SquashFS writer. `appimage_runtime.cpp` selects the embedded pinned
type 2 runtime. `attestations.cpp` writes adjacent DSSE collections for formats without a native
attachment mechanism.
`ostree.cpp` writes `archive-z2` or `bare-user` repositories through the owned OSTree encoder.
`bootc.cpp` constructs the OSTree container tar layout, component layers, bootc labels, manifest,
and subject-bound attestation referrers.

Store reads remain Capy tasks. Compression, archive construction, hashing, and filesystem writes
run behind `runtime::offload`; this directory does not start threads or block inside a coroutine.
The public contract and deterministic algorithms are in `docs/library/projection.md`.
