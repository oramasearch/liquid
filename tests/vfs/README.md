# VFS tests

The portable tests cover full materialisation, prefix selection, chunk-limited reads, drive folder
and size policy, extended-attribute mappings, and runtime queue latency. The mount scenario runs
when a FUSE backend and device are available and otherwise reports a skip with the platform
diagnosis. On Linux it also sets, gets, and lists the drive attributes through the mounted path.
An unavailable remote fixture verifies that an unhydrated read returns `EAGAIN` and persists the
path-specific offline reason. The mount uses a second TAAP identity for its locked-file write so
the expected `EACCES` exercises another holder's configured exclusive checkout.
See [`docs/library/vfs.md`](../../docs/library/vfs.md) for the complete scenario.

Tree and mount fixtures allocate their named directories beneath the unique process root specified
in the parent [`README.md`](../README.md).

The named `liquid-vfs-conformance` executable keeps backend registration in
`conformance_backend.cpp`, the test operation interface in `conformance_operations.hpp`, POSIX
syscalls in `conformance_posix.cpp`, and the core calibration implementation in
`conformance_direct.cpp`. The direct backend is test-only; every real backend must pass the same
cases in `test_conformance.cpp`. No kernel mount is used by `direct`, and no production libfuse
code changes for this harness.

The nine cases cover recorded-tree observation, attributed capture, file mutations, xattrs,
synthetic times, read-only refusal, and concurrent range hydration with persistent cache reuse.
Required backend execution fails on unavailability; optional availability returns CTest skip code
77. A required `libfuse` run on a Linux host where `/dev/fuse` cannot be opened (a container without
the device) also returns 77 before any case runs, printing the reason
([#362](https://github.com/oramasearch/liquid/issues/362)). The direct baseline and deliberate wrong-size failure are recorded in the
[proof record](../../docs/library/vfs-conformance-proof.md). Linux libfuse calibration remains
owed against #299; see the [run and extension guide](../../docs/library/vfs.md#backend-conformance-301).

`liquid-vfs-xattr-tests` includes the production FUSE implementation in its test translation unit
to call its private request handlers without exposing a library API or mounting a filesystem.
Twenty protected-path cases cover both mutation handlers on ordinary and observed mounts,
read-only roots/files/directories, and immutable `deps` paths. They assert `EROFS`, unchanged
persisted Drive settings, and denied write observations with path and PID. A writable case checks
both attribute names still persist pin and reset operations. FUSE headers and libraries are
required; a kernel FUSE device is not.
