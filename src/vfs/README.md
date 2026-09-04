# VFS implementation

This directory contains the private FUSE backend, serialized request queue, repository adapter,
chunk hydration cache, and platform-independent full materialisation implementation. Public types
remain in `include/liquid/vfs.hpp`; contributor guidance and verification live in
[`docs/library/vfs.md`](../../docs/library/vfs.md).
