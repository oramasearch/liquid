# Store collection tests

`test_gc.cpp` covers leased roots, expiry and renewal, shared chunks, tree and change reference
extraction, epoch high-water retention through forced index reconstruction, dry runs, whole-pack
deletion, and reconstruction from committed tombstones.
`gc_cli_fixture.cpp` creates a leased store for the `liquid gc --explain` integration test.
`test_gc_sim_regression.cpp` replays seed 140, fault step 265: a collection crash must not remove
a rooted object during a subsequent index reconstruction.
