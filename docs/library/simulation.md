# Deterministic store simulation

**Status: implemented for the Phase 1 chunk store on disk and in memory (liquid #55, #278), and for
shared-store repository state on both backends (liquid #326); the chunk store schedule also runs
behind the write-once backend on both (liquid #332).** The simulator is the first
subject of a reusable test harness. Sync and reconciliation state machines join it when their
implementations land.

## What and why

`tests/sim/` runs each seeded schedule against the disk reference backend and the in-memory test
backend, and against each again behind the write-once backend, which refuses any range that does not
append as Zetta's object storage does (liquid #332, [store.md](store.md#store-backend-interface)). It
interrupts one selected backend operation, destroys the store object without a close, and
opens the same backend again. It exercises
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
throws before or after disk writes, atomic complete-object renames, and collected-pack removal. The store translates that
exception into its ordinary error result; the in-memory implementation exposes equivalent write
and remove points while also supporting injected latency and one-shot liquid errors. The simulator
then drops the store object to model an
abrupt process stop.

Packs recover independently of the index. Open packs consist only of append-only verified frames.
The recovery scan accepts complete frames before a torn tail and starts the next write in a new
pack. Sealed packs retain their table and footer. After each crash recovery, the harness also
removes `index/` and verifies the pack-driven index rebuild has every acknowledged object. See
[store.md](store.md) for the format and the `store_options::durable` power-loss trade-off.

## Repository state

`RepositoryStateSimulation.SeededCrashRecovery` (`tests/sim/test_repository_state.cpp`, liquid
#326) runs the same probe-then-crash method one layer up, against a shared-store repository on the
disk backend and on the in-memory backend. A seeded schedule of 14 operations mixes file writes,
cuts, replication-cursor updates, store and repository reopens, and collections, with a checkpoint
every three drips so both `LIQUID_REPO` and `LIQUID_JOURNAL` publications occur. Half of the seeds
crash at a fault point on a `repositories/` key; the rest anywhere, which is mostly pack, index and
lease data. At the crash the in-memory backend is frozen, so destructors and cleanup of the crashed
process cannot write state its successor recovers.

After the crash the store and repository reopen and the harness proves: every acknowledged write
reads back byte for byte (the interrupted write may hold its new bytes, wholly); every acknowledged
change is in the log; the replication cursor is the acknowledged or the interrupted value; and the
repository accepts a write, a cut, and a collection that keeps every acknowledged file. On disk,
timestamps inside drips can move a pack rollover, so a run may reach fewer fault points than its
probe; such a seed is checked as a clean stop. Replacing the atomic marker publication with a
remove-then-put fails the first memory seed.

**Barrier durability.** The seeded schedule freezes the backend at a fault point, so everything
written before the crash survives. A real backend may also lose acknowledged writes no barrier has
covered yet. `memory_store_backend::lose_unbarriered()` models that crash: it drops every mutation
since the last successful barrier, which is a prefix of any issue order (the empty one) and so
holds under the ordering contract between barriers
([Ordering between barriers](#ordering-between-barriers), liquid #344).
`RepositoryStateDurability.CutIsDurableWhenItReturns` and
`Collaboration.published_and_discarded_sessions_stay_gone_after_a_crash` use it right after a cut,
a session publish and a session discard return (liquid #343): before those operations ended with a
barrier, the lost `LIQUID_JOURNAL` removal reverted the cut on reopen and the lost session prefix
removal let both sessions be reopened.

**Why a seed's crash point moves.** The schedule and the target step are fixed by the seed, but
drips and changes carry wall-clock timestamps (seconds, and milliseconds for some records), so
their encoded and compressed sizes, and with them pack rollovers and the number of fault points,
differ from run to run. The same target step can therefore land on a different write. Seed 17
(liquid #337) failed only when its step landed on the object map publication of a collection whose
manifest had already committed. Index changes that alter how many writes a flush or compaction
makes, such as liquid #332, shift which write a target lands on and so changed how often that seed
failed; they did not cause it.

**What a crash at a collection's `packs/objects.meta` means.** The collector publishes the
filtered object map before it commits `gc/MANIFEST` (see [gc.md](gc.md)). A crash at the map's
publication leaves the previous map and the previous manifest, so nothing was collected; a crash
after it, before the manifest, leaves a map without the collected objects over chunks that are
still live. Either way every object in the map can be read. Seed 17 found the earlier order,
which committed the manifest first, leaving the old map naming tombstoned chunks.
`RepositoryCollectionCrash` reaches that point deterministically: it records the fault points of
one collection, then crashes a fresh copy at each object map and manifest point and the point
after each, and reopens, reads, collects, and writes.

```sh
ctest --test-dir build -R RepositoryStateSimulation --output-on-failure
LIQUID_REPOSITORY_SIM_SEEDS=200 ctest --test-dir build -R RepositoryStateSimulation --output-on-failure
LIQUID_REPOSITORY_SIM_SEED=7 ctest --test-dir build -R RepositoryStateSimulation --output-on-failure -V
```

The default run is 40 seeds per backend.

The default CTest job, including the Linux sanitizer job, runs this harness. CI checkout currently
depends on the organization `ORAMA_SUBMODULES_TOKEN`; until that secret is configured, the matrix
and sanitizer jobs fail before tests begin.

## Ordering between barriers

The schedules above keep every write issued before the crash. A backend may keep less: of the
writes since the last barrier, `store_backend`'s contract lets a crash keep any subset, with each
key's own writes in issue order ([store.md](store.md#ordering-between-barriers), liquid #344).
`memory_store_backend::crash(durability, seed)` models it after the crashed process's handles are
gone. It keeps an undo state per key and a log of every mutation since the last successful barrier,
restores the barriered state, and replays the part of the log the mode keeps, chosen from the seed:

| `crash_durability` | What survives of the unbarriered log |
|---|---|
| `everything` | all of it: a crash of the process, not the backend (what the schedules above do) |
| `barriered` | none (`lose_unbarriered()`) |
| `prefix` | a prefix of the issue order, as a backend with one ordered log gives (Lake's workspace store today) |
| `subset` | for each key independently, a prefix of that key's writes: the contract |

Suites that run under it, each crashing at fault points chosen as above, freezing the backend at the
crash, then calling `crash`:

- `StoreCrashDurability` (`tests/sim/test_store_sim.cpp`): the store schedule on the memory and
  write-once memory backends, under `prefix` and `subset`, 200 seeds each by default
  (`LIQUID_SIM_SEEDS`, `LIQUID_SIM_SEED`).
- `StoreCompactionCrashDurability`: one compaction crashed at each of its fault points, 8 seeds
  each (`LIQUID_COMPACTION_CRASH_SEEDS`). Recovery must leave exactly one of the old pack and its
  replacement. `Prefix` passed before the fix; `Subset` found the compactor's marker ordering.
- `StoreCompactionRecoveryCrash`: the recovery of an uncommitted compaction crashed at each of its
  fault points under `subset`. The replacement must not outlive recovery.
- `RepositoryCrashDurability` (`tests/sim/test_repository_state.cpp`): the repository schedule on the
  memory backend under `prefix` and `subset`, 40 seeds each (`LIQUID_REPOSITORY_SIM_SEEDS`).
- `RepositoryCollectionCrashDurability`: a repository collection crashed at every fault point, 2
  seeds each under `subset` (`LIQUID_COLLECTION_CRASH_SEEDS`).
- `SessionCrashDurability` (`tests/test_collaboration.cpp`): a session's publish and its discard
  crashed at every fault point, under `everything` and, 2 seeds each, `subset`. The file holds the
  fork point's value or the session's, a session that comes back can be discarded, and the
  repository accepts a new change.

Every check is the same under each mode, so a seed that fails under `subset` and passes under
`prefix` is liquid relying on more order than the contract gives. The four found are listed in
[store.md](store.md#ordering-between-barriers), each now separated by a barrier.

```sh
ctest --test-dir build -R 'CrashDurability|CompactionRecoveryCrash' --output-on-failure
LIQUID_SIM_SEEDS=1000 ctest --test-dir build -R StoreCrashDurability --output-on-failure
```

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
