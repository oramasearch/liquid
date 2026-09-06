# Data branching implementation

`data.cpp` implements the public PostgreSQL branch lifecycle without linking a database client
library. PostgreSQL client programs run through the existing platform sandbox launcher on an
offload task with a fixed environment. The repository adapter exposes only persisted typed
configuration, private promotion fingerprints, live records, the active handle session, and the
repository-owned action directory.

The internal session-environment resolver and database probes retain shared-library visibility so
the static and shared test suites exercise the same implementation.

Client resolution asks `pg_config --bindir` before `PATH`, avoiding wrapper scripts and recording
the canonical installed client path in each data action. The launcher executes that installed path
directly and keeps its repository-local toolchain directory for generated scripts only.

Template copies are the portable lifecycle implementation. The filesystem capability probe and
automatic selector cover the reflink decision; explicit reflink requests report `unsupported`
until the declaration carries the local cluster control metadata described in
[`docs/library/data-branching.md`](../../docs/library/data-branching.md).
