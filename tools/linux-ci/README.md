# Linux verification image

The Linux proof image carries the native compilers, FUSE, PostgreSQL, PC/SC headers, and `swtpm`
needed by the full suite. Hardware-key tests remain deterministic through command-level transports;
the TPM package permits the external Linux lane to exercise a software TPM without adding
tpm2-tss to libliquid.

This is a build and test image, intentionally carrying the full toolchain and running as root.
The four service images under `deploy/` use separate Ubuntu runtime layers and UID/GID 65532;
they do not inherit this image. Their contract and verification are in
[`../../docs/build/service-images.md`](../../docs/build/service-images.md).
