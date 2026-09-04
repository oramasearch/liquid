# VFS implementation

This directory contains the private FUSE backend, serialized request queue, repository adapter,
chunk hydration cache, and platform-independent full materialisation implementation. Public types
remain in `include/liquid/vfs.hpp`; contributor guidance and verification live in
[`docs/library/vfs.md`](../../docs/library/vfs.md).

The libfuse3 backend parses process-level options before constructing its session, runs the
configured request loop through the runtime blocking adapter, and interrupts that loop only for a
recorded explicit stop.
