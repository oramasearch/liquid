# Store collection tests

`test_gc.cpp` covers leased roots, expiry and renewal, shared chunks, tree and change reference
extraction, epoch high-water retention through forced index reconstruction, dry runs, whole-pack
deletion, 30% and 70% live-pack compaction boundaries, byte-identical frame copies,
compaction-only collection, reconstruction from committed tombstones, and a forever ref rule that
keeps its reachable graph after repository reopen. Delta cases retain transitive bases and repack
chains before an index rebuild and repository reopen.
`gc_cli_fixture.cpp` creates a repository for the `liquid gc --explain`, `liquid retain explain`, and
`liquid gc --compact --compact-below` integration tests, which exercise the repository-owned
checkpoint leases.
`test_gc_sim_regression.cpp` replays seed 140: a collection or compaction crash must not remove a
rooted object during a subsequent index reconstruction.

`test_owner.cpp` covers liquid #321 exclusive ownership: in-process refusal, bounded-wait expiry,
hand-off to a waiting opener, and option validation; and, on POSIX, real writer processes from
`owner_fixture.cpp` (bytes from `owner_fixture_content.hpp`) that take turns on one root, and a
writer killed with `SIGKILL` mid-stream while a successor waits. Each process case ends with a
frame-level pack scan, reopen, a byte-for-byte read of every acknowledged write, and a full
collection that must keep every acknowledged object.
It also covers liquid #327 injected owners on a remote-shaped backend whose single lease fence
rises with each holder: hand-off on `close()`, bounded-wait expiry, a stale holder whose lease lapses
mid-put (refused by the remote, then locally; the successor recovers, passes a collection dry run,
and keeps every acknowledged object), a revoked grant with opted-in reads and release on
destruction, a backend without `fenced`, the injected disk owner, a stale holder's refused
`put_if_absent`, and a shared-store repository whose state writes all carry the owner's fence.

Store fixtures allocate their named directories beneath the unique process root specified in the
parent [`README.md`](../README.md).
