# Deterministic store simulation

The harness generates rooted puts, unrooted orphan puts, reads, reopens, lease renewals,
collections, and compaction-only passes from a seed. It interrupts one backend write, atomic disk
replacement, or pack removal and checks that every acknowledged rooted object remains readable after recovery and index
rebuild, with no pending compaction left behind.

The same schedule runs against the disk backend and the in-memory backend. The memory backend owns
all store objects without creating backend files and supports injected coroutine latency, one-shot
failures, the conditional create, a key listing, and freezing at a crash point. It also models
barrier durability (liquid #343): `lose_unbarriered()` drops every mutation since the last
successful barrier, as a crash of a backend that keeps only what a barrier covered would, and
`unbarriered()` counts them. A barrier while frozen makes nothing durable. `crash(durability, seed)`
(liquid #344) keeps a log of every mutation since the last barrier and keeps, after a crash, all of
it, none, a prefix of the issue order, or a seeded subset in which each key keeps a prefix of its
own writes, which is `store_backend`'s ordering contract. `liquid-tests` compiles it too, for the
session durability tests in `tests/test_collaboration.cpp`.

`run_store_simulation` takes a `crash_durability`; `StoreCrashDurability` runs the schedule on the
memory backends under `prefix` and `subset`. `StoreCompactionCrashDurability` and
`StoreCompactionRecoveryCrash` crash one compaction, and its recovery, at every fault point and
require no orphan pack.

`write_once_backend.hpp` decorates either backend with the range contract of Zetta's object storage
(liquid #332): `put_range` only appends at the object's current end, a retry of bytes already held
succeeds unchanged, and a rewrite or a gap fails `errc::conflict`. The schedule also runs over it on
both backends (`WriteOnceDisk`, `WriteOnceMemory`). `test_store_backend` returns it over the disk
backend when `LIQUID_TEST_WRITE_ONCE_BACKEND` is set, which is how the index, store and collection
suites run a second time over it.

`test_repository_state.cpp` (liquid #326) proves a shared-store repository's own state goes through
the backend: the conditional create, the scoped disk listing, a full repository lifecycle on the
in-memory backend with no file under any directory, the unchanged disk layout, journal crashes,
`RepositoryCollectionCrash` (liquid #337), which crashes one collection at each fault point of its
object map and manifest publication and reopens, `RepositoryStateDurability` (liquid #343), which
loses every unbarriered mutation right after a cut returns and requires the cut to survive,
`RepositoryStateSimulation`, the seeded crash schedule over repository operations, and, under the
subset crash mode (liquid #344), `RepositoryCrashDurability` and
`RepositoryCollectionCrashDurability`. See `docs/library/simulation.md#repository-state` and
`docs/library/simulation.md#ordering-between-barriers`.
