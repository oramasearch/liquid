# The full suite in an unprivileged Linux container

liquid's Linux proof runs the whole CTest suite inside an unprivileged container: the cluster
BuildKit lane (Ubuntu 24.04, clang 19, CMake 3.31, Release, every target built, `ctest -j 1`,
running as root but without extra capabilities). Such a container has no `/dev/fuse` and refuses
new user and network namespaces. Before [#348](https://github.com/oramasearch/liquid/issues/348)
the tests that need those capabilities failed there instead of skipping, so a Linux run could not
tell an environment gap from a defect.

## What the container lacks and how each test detects it

| Capability | Detected by | Tests that skip without it |
|---|---|---|
| A usable `/dev/fuse` | `tests/fuse_probe.hpp` opens `/dev/fuse` read-write at run time. The build-time `LIQUID_BUILD_HAS_FUSE` flag only says libfuse was found when building. | 30 build executor gtests (`BuildCache`, `BuildPlan`, `BuildEvents`, `BuildTests`, `Build`, `BuildVerification`, `BuildReplay`) and 2 configuration gtests through `LIQUID_TEST_REQUIRE_FUSE()`; `HotUpdateDemo.TierThreeInvokesRestartHook`; `vfs-conformance-libfuse` (exit 77 before any case); `symbols-cli-build` and `symbols-cli` through `liquid-fuse-probe` and `tests/cli/require-fuse.cmake` |
| User, mount and network namespaces | `liquid-offline-runner` runs `unshare --user --map-root-user --mount --net /bin/true` and exits 77 with the reason when it fails | `cli-offline`, `cli-log-url` |

Every skip prints its reason (`FUSE is unavailable at run time: cannot open /dev/fuse: No such file
or directory`, `this host does not permit user, mount and network namespaces ...`), and CTest
reports it as `Skipped`, never as passed. On a host where the capability exists nothing is
skipped and each test runs in full; nothing was weakened to make the container pass.

## Classification of the failures on `dev`

The baseline on `dev` at `b72fc9f` (2026-10-21) failed 41 of 801 tests, the same 41 as the
`v0.2.0` proof:

| Failures | Cause | Kind | Resolution |
|---|---|---|---|
| 32 executor and configuration gtests, `vfs-conformance-libfuse`, `symbols-cli-build`, `HotUpdateDemo.TierThreeInvokesRestartHook` | No `/dev/fuse` | Environment | Skip with the reason ([#362](https://github.com/oramasearch/liquid/issues/362)) |
| `cli-offline`, `cli-log-url` | `unshare` not permitted; a rerun then found the previous run's input link | Environment, plus a test that did not clean up | Skip with the reason; `log-url.cmake` starts from an empty sandbox and the runner reuses a correct link ([#363](https://github.com/oramasearch/liquid/issues/363)) |
| `cli-help-docs` | `help-docs.cmake` used `IN_LIST` without CMP0057, an error under `cmake -P` on CMake 3.x | Defect in a test script | Fixed; `cmake-script-policies` now fails on any such script ([#364](https://github.com/oramasearch/liquid/issues/364)) |
| `Projection.generated_values_cannot_change_shell_structure` | libstdc++ collapses repeated separators in `generic_string()` and path comparison, so `/usr//bin/tool` was accepted as a canonical install path | Real Linux defect | Fixed in `src/install/records.cpp`, which now checks the path as written ([#356](https://github.com/oramasearch/liquid/issues/356)) |
| `HotUpdateDemo.TierOnePatchChangesARunningCapyLoop` | The daemon reports `outcome=patched tier=0` after two deliveries; the test reads only the latest detail. Not caused by FUSE or namespaces | Real Linux defect | Open: [#365](https://github.com/oramasearch/liquid/issues/365) |
| `BuildPlan.daemon_preserves_the_repository_plan_and_event_sequence` (SEGFAULT) | After the test fails, the `daemon` is destroyed without `shutdown()`; at process exit a blocking accept offload resumes on a destroyed I/O context | Real defect, hidden by the FUSE skip in the container | Open: [#374](https://github.com/oramasearch/liquid/issues/374) |

The single-process `liquid-tests --gtest_filter='*'` run that the lane makes before CTest also
timed out once, in `AdmissionServer.refuses_when_the_network_source_is_down`
([#376](https://github.com/oramasearch/liquid/issues/376)). CTest runs that test in its own process
and it passes.

## Result with the change

The same lane on the #348 branch: 802 tests (the new `cmake-script-policies` included), 801 passed
or skipped with a reason, 68 skipped (30 before), none Not Run, and one failure,
`HotUpdateDemo.TierOnePatchChangesARunningCapyLoop`, the open defect
[#365](https://github.com/oramasearch/liquid/issues/365).

## Running it

The lane is `codex-work/build/liquid-linux-test/run-liquid-linux-tests.sh` (outside this
repository): `BUILD_TARGET=all CTEST_JOBS=1 run-liquid-linux-tests.sh <worktree> <label> '*' 1 '.'`.
It lists every skipped test with `SKIPPED_TEST` lines, so the skips can be read after each run.
