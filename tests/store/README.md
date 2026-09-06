# Store collection tests

`test_gc.cpp` covers leased roots, expiry and renewal, shared chunks, tree and change reference
extraction, epoch high-water retention through forced index reconstruction, dry runs, whole-pack
deletion, 30% and 70% live-pack compaction boundaries, byte-identical frame copies,
compaction-only collection, reconstruction from committed tombstones, and a forever ref rule that
keeps its reachable graph after repository reopen.
`gc_cli_fixture.cpp` creates a repository for the `liquid gc --explain`, `liquid retain explain`, and
`liquid gc --compact --compact-below` integration tests, which exercise the repository-owned
checkpoint leases.
`test_gc_sim_regression.cpp` replays seed 140: a collection or compaction crash must not remove a
rooted object during a subsequent index reconstruction.

Store fixtures allocate their named directories beneath the unique process root specified in the
parent [`README.md`](../README.md).
