# Build tests

`test_executor.cpp` exercises public action execution and the private disk-cache invariants. Tests
that require observation and sandbox enforcement run only when FUSE is configured; the prescribed
privileged Linux container runs them for real. Portable tests cover action-index persistence,
rebuild, and the read-only publication default.

`fixture.c` is the hermetic toolchain program materialised into each test repository. Its commands
copy one input, print the fixed environment, attempt denied access, or exercise timeout cleanup.
