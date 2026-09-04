# Deterministic store simulation

The harness generates rooted puts, unrooted orphan puts, reads, reopens, lease renewals,
collections, and compaction-only passes from a seed. It interrupts one write, rename, or pack
removal and checks that every acknowledged rooted object remains readable after recovery and index
rebuild, with no pending compaction left behind.
