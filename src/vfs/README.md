# VFS implementation

This directory contains the private FUSE backend, serialized request queue, repository adapter,
chunk hydration cache, and platform-independent full materialisation implementation. Public types
remain in `include/liquid/vfs.hpp`; contributor guidance and verification live in
[`docs/library/vfs.md`](../../docs/library/vfs.md).

The private repository adapter retains shared-library visibility because build diagnostics and
their portable fixtures use the same stored-entry lookup without exposing it as a public header.

The libfuse3 backend parses process-level options before constructing its session, runs the
configured request loop through the runtime blocking adapter, and interrupts that loop only for a
recorded explicit stop. Writable observed mounts support build output creation, replacement,
renaming, truncation, permissions, and synthetic source/output modification times. Explicit
shutdown probes a fresh path so kernel attribute caching cannot leave the request loop asleep.
Hydration passes a version 2 blob's whole-content digest and logical chunk offset to the store.
Remote ranges therefore enter the existing serialized queue and persistent chunk cache without a
second VFS path; full materialisation uses the same store-backed content identity.

Read-only observed mounts resolve entries from the action's recorded input-tree digest, while
mutable capture mounts continue to use the live repository tree. Recorded declared outputs receive
virtual file and directory placeholders only for read-only execution so bind targets exist without
altering the captured input snapshot. Snapshot entries retain remote blob metadata so replay uses
the configured source when it reads a recorded remote input.
