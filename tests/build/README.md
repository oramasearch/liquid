# Build tests

`test_executor.cpp` exercises public action execution and the private disk-cache invariants. Tests
that require observation and sandbox enforcement run only when FUSE is configured; the prescribed
privileged Linux container runs them for real. Portable tests cover action-index persistence,
rebuild, the read-only publication default, SLSA provenance derivation and verification, tamper
rejection, and DSSE/in-toto shape plus digest round-trip.

`test_why.cpp` builds signed action/result histories without a mount and proves input digests and
line ranges, drip attribution, argv, environment, toolchain, sandbox, never-built, imported-index,
and cumulative-statistics explanations. The Linux `test_wrap.cpp` scenario additionally proves that
a real observed header miss is available through `build::why` and that completed-wrap action and
byte counts agree. It also tightens policy after a recorded build and proves `why` reports the
current refusal before treating the previous lookup as a hit or ordinary miss.

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

The real identical-execution cache test verifies every result's provenance and asserts that a hit
reuses the original attestation digest.

`sleep_copy.c` is a dynamic test tool used only by the privileged Linux wrap scenario. Two
invocations with different sleeps form a known output dependency. The scenario verifies the
longest dependent chain, exact phase-sum accounting, retained trace loading, Chrome JSON structure,
and `why_slow` target selection.
`test_policy.cpp` covers fail-closed refusal, warning reports, approved-builder filtering,
invalid-signature and unknown-profile rejection, two-distinct-identity reproducibility quorum,
toolchain traversal, subject-index rebuild after reopen, pre-spawn refusal in the Linux mount
environment, and policy-bound candidate keys.
