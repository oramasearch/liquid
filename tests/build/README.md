# Build tests

`test_executor.cpp` exercises public action execution and the private disk-cache invariants. Tests
that require observation and sandbox enforcement run only when FUSE is configured; the prescribed
privileged Linux container runs them for real. Portable tests cover action-index persistence,
rebuild, and the read-only publication default.

`fixture.c` is the hermetic toolchain program materialised into each test repository. Its commands
copy one input, print the fixed environment, emit random bytes, attempt denied access, or exercise
timeout cleanup. Verification tests prove a pure agreement, signature-valid agreement and
disagreement attestations, rate-one sampling, durable divergent quarantine, and critical output
diagnostics.

`wrap/` holds unchanged CMake, Autotools, recursive Make, and compilation-database projects built
with the container's system toolchain. `test_wrap.cpp` proves inner and outer hits, selective header
invalidation with an explanation, fixed-root reuse in a second checkout, cached configure probes,
parallel compilation-database seeding, and sandbox-only hook variables. The Linux CLI case runs
`liquid verify --all` and checks that every wrapped action agrees.
