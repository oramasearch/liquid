# Projection implementation

`projection.cpp` validates sources, OS base and overlay paths, ABI verdicts, relocation,
action-platform metadata, build-trace layer evidence, and binary-derived dependencies. It also
signs and stores `projection/1`, including digest-pinned base and overlay tree claims for OS
targets.
`package.cpp` dispatches CPU work through `runtime::offload` to the registered Debian, RPM, and APK
writers. `rules.cpp` owns dependency capabilities and trace-distance layer assignment. `oci.cpp`
builds layouts, image layers, subject-bound attestation manifests, and indexes.
`helm.cpp` materialises the fixed chart templates and the equivalent OCI chart artifact.
`scripts.cpp` maps one validated install intent to the fixed Debian, RPM, and Alpine lifecycle
templates. `conflicts.cpp` indexes owned package paths across a request batch or an installed
inventory and derives explicit replacement relationships. Package projection embeds canonical
intent and service-unit files and binds generated script digests into `projection/1` and SPDX.
`ostree.cpp` writes `archive-z2` or `bare-user` repositories through the owned OSTree encoder.
`bootc.cpp` constructs the OSTree container tar layout, component layers, bootc labels, manifest,
and subject-bound attestation referrers.

Store reads remain Capy tasks. Compression, archive construction, hashing, and filesystem writes
run behind `runtime::offload`; this directory does not start threads or block inside a coroutine.
The public contract and deterministic algorithms are in `docs/library/projection.md`.
