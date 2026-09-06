# Linux verification image

The Linux proof image carries the native compilers, FUSE, PostgreSQL, PC/SC headers, and `swtpm`
needed by the full suite. Hardware-key tests remain deterministic through command-level transports;
the TPM package permits the external Linux lane to exercise a software TPM without adding
tpm2-tss to libliquid.
