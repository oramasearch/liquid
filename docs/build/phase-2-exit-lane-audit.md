# Phase 2 exit: the lane audit for issue #299

The Phase 2 exit asks liquid to wrap an unmodified project, reuse its cache on a second machine,
attest the result, and catch injected non-determinism
([#299](https://github.com/oramasearch/liquid/issues/299)). This page records why that
demonstration has not run: **no build machine available to the project can run a wrapped
build**, because a wrapped build observes its tools through a FUSE mount and none of the
candidates has one.

The same prerequisite blocks the Phase 4 exit
([#300](https://github.com/oramasearch/liquid/issues/300)), whose incremental build runs through
`wrap`, and the libfuse baseline of the VFS conformance cases
([`../library/vfs.md`](../library/vfs.md#backend-conformance-301)).

## Status on 2026-10-12

Each finding of the first audit (2026-09-08) was checked again when `v0.1.0` was cut. All of them
still hold.

| Candidate | Finding |
|---|---|
| The macOS development machine | macFUSE is not installed: no `/Library/Filesystems/macfuse.fs`, and `pkg-config` finds no `fuse`. The release's test run skipped its 12 FUSE-dependent and peer-dependent tests. |
| The container build lane (rootless BuildKit, not privileged) | `/dev/fuse` is absent. `fusermount3` is installed, which does not give access to a device that is not there. |
| A native Linux build host | Not reachable when checked. |
| Hosted CI | The checkout of the private submodules fails, so nothing runs. |

The container build lane builds liquid and runs its tests on Linux. It has no `/dev/fuse`, so it
does not run the wrapped-build cases, and the whole suite does not pass there yet
([#348](https://github.com/oramasearch/liquid/issues/348),
[#356](https://github.com/oramasearch/liquid/issues/356)).

## What closes it

A Linux machine with a usable `/dev/fuse` (a privileged container or a host), or macFUSE on the
macOS machine, and a second compatible machine for the cache-transfer clause. With those, run the
#299 demonstration end to end and record it here.
