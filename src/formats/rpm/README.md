# RPM packages

The writer owns the RPM lead, signature header, metadata header, and `newc` cpio payload. It emits
SHA-256 file and header digests and compresses the payload with pinned zstd. Requirements and
provides include the ABI-derived RPM soname form. OpenPGP is deliberately absent: release trust is
carried by the adjacent TAAP projection attestation. The minimal reader validates both headers and
enumerates the cpio payload with file bytes and modes. Generated `%pre`, `%post`, `%preun`, and
`%postun` values and their `/bin/sh` interpreter tags live in the RPM header and round-trip through
the reader.
