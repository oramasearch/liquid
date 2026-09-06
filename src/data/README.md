# Data branching implementation

`data.cpp` implements the public PostgreSQL branch lifecycle without linking a database client
library. PostgreSQL client programs run through the existing platform sandbox launcher on an
offload task with a fixed environment. The repository adapter exposes only persisted typed
configuration, private promotion fingerprints, live records, the active handle session, and the
repository-owned action directory.

The internal session-environment resolver and database probes retain shared-library visibility so
the static and shared test suites exercise the same implementation.

Client resolution prefers versioned PostgreSQL installation directories before `PATH`, avoiding
Debian's `pg_wrapper` scripts so the captured toolchain always contains the real client binaries.

Template copies are the portable lifecycle implementation. The filesystem capability probe and
automatic selector cover the reflink decision; explicit reflink requests report `unsupported`
until the declaration carries the local cluster control metadata described in
[`docs/library/data-branching.md`](../../docs/library/data-branching.md).
