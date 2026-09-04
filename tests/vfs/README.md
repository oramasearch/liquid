# VFS tests

The portable tests cover full materialisation, prefix selection, chunk-limited reads, and runtime
queue latency. The mount scenario runs when a FUSE backend and device are available and otherwise
reports a skip with the platform diagnosis. See [`docs/library/vfs.md`](../../docs/library/vfs.md)
for the complete scenario.
