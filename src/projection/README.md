# Projection implementation

`projection.cpp` validates sources, ABI verdicts, relocation, action-platform metadata, build-trace
layer evidence, and binary-derived dependencies. It also signs and stores `projection/1`.
`package.cpp` dispatches CPU work through `runtime::offload` to the registered Debian, RPM, and APK
writers. `rules.cpp` owns dependency capabilities and trace-distance layer assignment. `oci.cpp`
builds layouts, image layers, subject-bound attestation manifests, and indexes.
`helm.cpp` materialises the fixed chart templates and the equivalent OCI chart artifact.

Store reads remain Capy tasks. Compression, archive construction, hashing, and filesystem writes
run behind `runtime::offload`; this directory does not start threads or block inside a coroutine.
The public contract and deterministic algorithms are in `docs/library/projection.md`.
