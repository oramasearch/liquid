# Deterministic store simulation

The harness generates rooted puts, unrooted orphan puts, reads, reopens, lease renewals,
collections, and compaction-only passes from a seed. It interrupts one backend write, atomic disk
replacement, or pack removal and checks that every acknowledged rooted object remains readable after recovery and index
rebuild, with no pending compaction left behind.

The same schedule runs against the disk backend and the in-memory backend. The memory backend owns
all store objects without creating backend files and supports injected coroutine latency, one-shot
failures, the conditional create, a key listing, and freezing at a crash point.

`test_repository_state.cpp` (liquid #326) proves a shared-store repository's own state goes through
the backend: the conditional create, the scoped disk listing, a full repository lifecycle on the
in-memory backend with no file under any directory, the unchanged disk layout, journal crashes, and
`RepositoryStateSimulation`, the seeded crash schedule over repository operations. See
`docs/library/simulation.md#repository-state`.
