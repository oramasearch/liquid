# VFS tests

The portable tests cover full materialisation, prefix selection, chunk-limited reads, drive folder
and size policy, extended-attribute mappings, and runtime queue latency. The mount scenario runs
when a FUSE backend and device are available and otherwise reports a skip with the platform
diagnosis. On Linux it also sets, gets, and lists the drive attributes through the mounted path.
An unavailable remote fixture verifies that an unhydrated read returns `EAGAIN` and persists the
path-specific offline reason.
See [`docs/library/vfs.md`](../../docs/library/vfs.md) for the complete scenario.

Tree and mount fixtures allocate their named directories beneath the unique process root specified
in the parent [`README.md`](../README.md).
