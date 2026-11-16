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
`unbarriered()` counts them. A barrier while frozen makes nothing durable. `liquid-tests` compiles
it too, for the session durability test in `tests/test_collaboration.cpp`.

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
loses every unbarriered mutation right after a cut returns and requires the cut to survive, and
`RepositoryStateSimulation`, the seeded crash
schedule over repository operations. See
`docs/library/simulation.md#repository-state`.
