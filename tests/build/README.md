# Build tests

`test_executor.cpp` exercises public action execution and the private disk-cache invariants. Tests
that require observation and sandbox enforcement run only when FUSE is configured; the prescribed
privileged Linux container runs them for real. Portable tests cover action-index persistence,
rebuild, the read-only publication default, mixed-cost size-cap eviction, SLSA provenance derivation
and verification, tamper rejection, DSSE/in-toto shape plus digest round-trip, shared-model
SPDX/CycloneDX structure, and fully marked system-binary inference.

`test_distribution.cpp` measures tiny, short, long, and uncertain timing histories against the
cost model and asserts local, remote, and race choices with exact estimates. Its second-store test
executor uses #17's shared on-disk remote source to reconstruct the source and toolchain in a new
directory, matches a local output, groups four short actions through one batch call, and proves a
race cancels its delayed remote loser while adding one published result.

`test_why.cpp` builds signed action/result histories without a mount and proves input digests and
line ranges, drip attribution, argv, environment, toolchain, sandbox, never-built, imported-index,
execution scheduling inputs, and cumulative-statistics explanations. The Linux `test_wrap.cpp`
scenario additionally proves that
a real observed header miss is available through `build::why` and that completed-wrap action and
byte counts agree. It also tightens policy after a recorded build and proves `why` reports the
current refusal before treating the previous lookup as a hit or ordinary miss.

`fixture.c` is the hermetic toolchain program materialised into each test repository. Its commands
copy one input, print the fixed environment, emit random bytes, attempt denied access, or exercise
timeout cleanup. Its deterministic failure command writes fixed output, stdout, and stderr before
returning a nonzero exit for replay coverage. Verification tests prove a pure agreement,
signature-valid agreement and
disagreement attestations, rate-one sampling, durable divergent quarantine, and critical output
diagnostics.

Replay cases prove byte-identical reproduction of that recorded failure, action selection after a
cache hit, opt-in verification recording, byte-zero diagnostics for an altered recorded output,
and hydration of a missing input through a `remote_blob_source` backed by a second on-disk liquid
store. The Linux CLI case also requires valid JSON and confirms default replay does not record.

The mixed-cost retention case writes a cheap large result and an expensive small result into the
real store, persists both index rows, exceeds the configured physical cap, and proves collection
removes only the lower execution-cost-per-byte result.

`wrap/` holds unchanged CMake, Autotools, recursive Make, and compilation-database projects built
with the container's system toolchain. `test_wrap.cpp` proves inner and outer hits, selective header
invalidation with an explanation, fixed-root reuse in a second checkout, cached configure probes,
cross-project configure facts with zero feature probes on reuse, foreign-target refusal before
compiler entry, parallel compilation-database seeding, and sandbox-only hook variables. The Linux
CLI case runs `liquid verify --all`, checks that every wrapped action agrees, then replays one action
through `liquid replay --json`.
The CMake fixture also compares its generated SBOM components with every target manifest digest
and toolchain-tree leaf.

`test_facts.cpp` is portable coverage for canonical probe normalization, durable records and
bundles, and exact isolation across changed toolchain, sysroot, and target keys.

The real identical-execution cache test verifies every result's provenance and asserts that a hit
reuses the original attestation digest.

`sleep_copy.c` is a dynamic test tool used only by the privileged Linux wrap scenario. Two
invocations with different sleeps form a known output dependency. The scenario verifies the
longest dependent chain, exact phase-sum accounting, retained trace loading, Chrome JSON structure,
recorded scheduling fields, and `why_slow` target selection.
`test_policy.cpp` covers fail-closed refusal, warning reports, approved-builder filtering,
invalid-signature and unknown-profile rejection, two-distinct-identity reproducibility quorum,
toolchain traversal, subject-index rebuild after reopen, pre-spawn refusal in the Linux mount
environment, and policy-bound candidate keys.

`test_remote_outputs.cpp` uses a second on-disk liquid store as a counting remote source. It proves
zero-byte unused results, exact chunk-range hydration and cache reuse, on-demand materialisation,
tri-state availability, and metadata-only consumption by a consecutive remote result. The Linux
executor fixture marks one action eligible and verifies `/etc/hostname` is refused and reported.
