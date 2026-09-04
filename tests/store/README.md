# Store collection tests

`test_gc.cpp` covers leased roots, expiry and renewal, shared chunks, tree and change reference
extraction, epoch high-water retention through forced index reconstruction, dry runs, whole-pack
deletion, 30% and 70% live-pack compaction boundaries, byte-identical frame copies,
compaction-only collection, and reconstruction from committed tombstones.
`gc_cli_fixture.cpp` creates a repository for the `liquid gc --explain` and
`liquid gc --compact --compact-below` integration tests, which exercise the repository-owned
checkpoint leases.
`test_gc_sim_regression.cpp` replays seed 140: a collection or compaction crash must not remove a
rooted object during a subsequent index reconstruction.
