# Deterministic store simulation

**Status: implemented for the Phase 1 chunk store (liquid #55).** The simulator is the first
subject of a reusable test harness. Sync and reconciliation state machines join it when their
implementations land.

## What and why

`tests/sim/` runs a seeded schedule against a real local store, interrupts one selected disk
operation, destroys the store object without a close, and opens the directory again. It exercises
rooted puts, orphan puts, gets, range reads, lease renewals, collections, and explicit reopens.
Input sizes range from 40 bytes to 4 MiB; previously
generated inputs are deliberately reused to exercise deduplication.

The harness has an in-memory model of acknowledged, leased objects. After recovery it proves
**acknowledged and rooted ⊆ visible ⊆ correct**: every acknowledged rooted object is readable and byte-identical,
and every recovered orphan that is readable has the expected digest and bytes. An interrupted put
may be visible because a complete content-addressed object is correct even if its response did not
reach the caller. Collection in liquid #8 reclaims those orphans. Unknown digests return
`errc::not_found`, and a new put must work after every recovery.

## Running and replaying

The normal CTest run executes 200 seeds. The `sim-long` label permits a larger run without adding
another test target:

```sh
ctest --test-dir build -R StoreSimulation --output-on-failure
LIQUID_SIM_SEEDS=2000 ctest --test-dir build -L sim-long --output-on-failure
```

Every verbose run prints `seed`, `fault_step`, and `orphans` for each completed seed. A failure
also prints a one-seed replay command. To run a known seed directly:

```sh
LIQUID_SIM_SEED=0 ctest --test-dir build -R StoreSimulation --output-on-failure -V
```

The schedule is generated only from `std::mt19937_64` output and explicit integer mappings. It
does not use wall-clock input, thread timing, or implementation-defined random distributions. The
harness first runs the schedule with a counting injector, derives the selected fault step from the
same seed, then reruns it in a clean directory with that step interrupted. A seed therefore selects
the same operation sequence and fault point on every platform.

## Fault points and pack recovery

`src/store/faults.{hpp,cpp}` is private test instrumentation. The normal process-global injector
pointer is null, so a point costs one atomic load and branch. Tests can install an injector that
throws before or after every logical file write, manifest rename, and collected-pack removal. The store translates that
exception into its ordinary error result; the simulator then drops the store object to model an
abrupt process stop.

Packs recover independently of the index. Open packs consist only of append-only verified frames.
The recovery scan accepts complete frames before a torn tail and starts the next write in a new
pack. Sealed packs retain their table and footer. After each crash recovery, the harness also
removes `index/` and verifies the pack-driven index rebuild has every acknowledged object. See
[store.md](store.md) for the format and the `store_options::durable` power-loss trade-off.

The default CTest job, including the Linux sanitizer job, runs this harness. CI checkout currently
depends on the organization `ORAMA_SUBMODULES_TOKEN`; until that secret is configured, the matrix
and sanitizer jobs fail before tests begin.

## Adding a subject

Keep a subject's state model independent of its implementation, and generate its complete schedule
before executing it. Add its operations and recovery assertions to `tests/sim/`, use the existing
seed/environment protocol, and make injected operation boundaries explicit. A new subject must:

1. print the seed and fault step on failure;
2. replay exactly with `LIQUID_SIM_SEED`;
3. preserve the default run's bounded seed count and add long coverage through `sim-long`; and
4. document its acknowledgement and orphan semantics here and in its implementation note.

Do not make a simulator pass by changing a discovered production behavior without recording the
separate decision or issue that authorizes that behavior.
