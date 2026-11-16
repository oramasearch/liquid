# Hot-update tests

`test_demo.cpp` drives the installed runtime shape end to end on macOS and Linux: an `input.txt`
drip makes the daemon rebuild the configured target, classify its prepared frontend evidence,
create and deliver a signed tier-1 patch to a Capy demo process, and report the outcome. The same
process prints the replacement leaf-function value after its next quiescence point.
A tier-2 check loads an additive object without requiring an existing entry slot. The tier-3 case
runs the restart hook through the configured sandbox backend and verifies the outcome arguments.
It skips explicitly when macOS has no macFUSE, or when `/dev/fuse` cannot be opened at run time
(a Linux container without the device, [#362](https://github.com/oramasearch/liquid/issues/362));
a Linux host with FUSE executes it with libfuse3.

The portable classification and unsigned-patch refusal cases remain in `tests/test_hot_update.cpp`.

The tier-1 listener uses `socket_fixture.hpp` independently of the repository path, so deep
worktrees and long temporary roots do not lengthen its socket address. Runtime boundary coverage
checks the largest accepted pathname and the first rejected byte, including the diagnostic's path
and native capacity. See [socket fixtures](../../docs/build/socket-fixtures.md).
