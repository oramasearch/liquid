# Deterministic store simulation

The harness generates rooted puts, unrooted orphan puts, reads, reopens, lease renewals,
collections, and compaction-only passes from a seed. It interrupts one backend write, atomic disk
replacement, or pack removal and checks that every acknowledged rooted object remains readable after recovery and index
rebuild, with no pending compaction left behind.

The same schedule runs against the disk backend and the in-memory backend. The memory backend owns
all store objects without creating backend files and supports injected coroutine latency and one-shot
failures.
