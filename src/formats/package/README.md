# Package writer primitives

`common.cpp` provides the deterministic SHA-256, MD5, zstd, tar, and cpio operations shared by
the Tier A package and image writers. `codecs.cpp` exposes the Debian, RPM, and Alpine readers to
the built-in format registry through the ordinary linked-module ABI.

These functions are synchronous CPU work. Projection coroutines call them only through
`runtime::offload`. Format and compression guarantees are documented in
`docs/library/projection.md`.
