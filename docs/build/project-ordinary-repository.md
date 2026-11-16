# Ordinary-repository projection verification

Issue #286 routes changes and trees to their owning repository and retains build-result
projection. The [executable guide](../guides/project-from-files.md) uses ordinary files and
public commands only. The [projection contract](../library/projection.md) describes input kinds.
No format writer was changed, no filesystem mount was used, and no build-wrapper command ran.

## Environment and build

macOS arm64, Homebrew Clang 22.1.8. The unchanged baseline was built from `283e636669533c4d1ff874766c1bfd1d3ad267d3`.
The baseline executable was saved before rebuilding the CLI changes. Commands ran with a local
identity and temporary directories inside this worktree. Native suites run sequentially, with
build parallelism limited to six jobs and a fresh database on the local PostgreSQL server.

```sh
cmake -S . -B build -G Ninja && cmake --build build -j 6
```

The baseline and changed static builds completed with exit 0. The shared build uses
`cmake -S . -B build-shared -G Ninja -DBUILD_SHARED_LIBS=ON` and
`cmake --build build-shared -j 6`.

## Before: the issue reproduction

Each reproduction starts in its own empty directory with a separate identity and a nonexistent
build-cache location. `PATH` selects the saved unchanged executable. The shell trace records
commands and their output.

### Debian

```text
+ set -eux
+ mkdir package
+ cd package
+ mkdir -p share/example
+ printf 'payload\n'
+ liquid init .
initialised .
++ liquid cut release
+ change=change=blake3:071ecd57601857262fdf5c2091e662bd5a423b01386061db15cc152623cb9145
+ liquid project deb blake3:071ecd57601857262fdf5c2091e662bd5a423b01386061db15cc152623cb9145 --name example --version 1.0.0 --arch arm64 --out ../out
error: not_configured: repository has no recorded build configuration; run liquid wrap <command> first ($REPO/.cache/issue-286/before/package)
exit=1
```

### OCI

```text
+ set -eux
+ mkdir package
+ cd package
+ mkdir -p share/example
+ printf 'payload\n'
+ liquid init .
initialised .
++ liquid cut release
+ change=change=blake3:a3ac4dc54ae4c00f8d2147e25544e7ce1f30517d4397fd3573a80e01cf56b3da
+ liquid project oci blake3:a3ac4dc54ae4c00f8d2147e25544e7ce1f30517d4397fd3573a80e01cf56b3da --name example --version 1.0.0 --arch arm64 --out ../out
error: not_configured: repository has no recorded build configuration; run liquid wrap <command> first ($REPO/.cache/issue-286/before-oci/package)
exit=1
```

### Tree input

The saved baseline has no tree-digest command. The added `tree` command reads the unchanged
public `root_digest()` API on the baseline repository; projection still runs in the saved
baseline executable. Both formats reject that valid tree before inspecting it.

```text
++ $REPO/build/liquid tree
+ tree=blake3:394a59172c9ef3f911013e8d8d1ac19de8e6d6622f663a705de559f19e4d90ad
+ liquid project deb blake3:394a59172c9ef3f911013e8d8d1ac19de8e6d6622f663a705de559f19e4d90ad --name example --version 1.0.0 --arch arm64 --out ../tree-out
error: not_configured: repository has no recorded build configuration; run liquid wrap <command> first ($REPO/.cache/issue-286/before/package)
+ printf 'deb exit=%s\n' 1
deb exit=1
+ liquid project oci blake3:394a59172c9ef3f911013e8d8d1ac19de8e6d6622f663a705de559f19e4d90ad --name example --version 1.0.0 --arch arm64 --out ../tree-image
error: not_configured: repository has no recorded build configuration; run liquid wrap <command> first ($REPO/.cache/issue-286/before/package)
+ printf 'oci exit=%s\n' 1
oci exit=1
```

## After: changes, trees, and repeated bytes

`PATH` selects the rebuilt CLI. The beginning is the issue reproduction verbatim, followed by
OCI projection and both targets from the digest printed by `liquid tree`. Both `cmp` commands
returned zero. The configured build-cache location remained absent.

```text
+ set -eux
+ mkdir package
+ cd package
+ mkdir -p share/example
+ printf 'payload\n'
+ liquid init .
initialised .
++ liquid cut release
+ change=change=blake3:8b2ebbbf6ac790878812f9952831e4f1ec838aed0fbe105f9916e6911a7b797b
+ liquid project deb blake3:8b2ebbbf6ac790878812f9952831e4f1ec838aed0fbe105f9916e6911a7b797b --name example --version 1.0.0 --arch arm64 --out ../out
artifact: ../out/example-1.0.0-1.arm64.deb sha256:c203d1ad7006f3d2473f274d057532135a4d60c6176bc349a7cb5b0e91a010ab application/vnd.debian.binary-package
attestation: blake3:370e9d2cb1d1a23597533fa79494641905a066d826f01431ead677df13b68658
+ liquid project oci blake3:8b2ebbbf6ac790878812f9952831e4f1ec838aed0fbe105f9916e6911a7b797b --name example --version 1.0.0 --arch arm64 --out ../image
artifact: ../image/example-1.0.0.oci sha256:fa29624ee2c84e01b6c673ed143c220bd764e6464bcc00a84572b214bdac44b0 application/vnd.oci.image.manifest.v1+json
attestation: blake3:60568710b10db0428894acb2e678d16fbc0246fdb904d91b3ce3d5bbd1c9276e
++ liquid tree
+ tree=blake3:ea22a60b6d8d53b9d07d54a5380c2efbdadfc7a8d6ff444d128f47442d3978f3
+ liquid project deb blake3:ea22a60b6d8d53b9d07d54a5380c2efbdadfc7a8d6ff444d128f47442d3978f3 --name example --version 1.0.0 --arch arm64 --out ../tree-out
artifact: ../tree-out/example-1.0.0-1.arm64.deb sha256:c203d1ad7006f3d2473f274d057532135a4d60c6176bc349a7cb5b0e91a010ab application/vnd.debian.binary-package
attestation: blake3:370e9d2cb1d1a23597533fa79494641905a066d826f01431ead677df13b68658
+ liquid project oci blake3:ea22a60b6d8d53b9d07d54a5380c2efbdadfc7a8d6ff444d128f47442d3978f3 --name example --version 1.0.0 --arch arm64 --out ../tree-image
artifact: ../tree-image/example-1.0.0.oci sha256:fa29624ee2c84e01b6c673ed143c220bd764e6464bcc00a84572b214bdac44b0 application/vnd.oci.image.manifest.v1+json
attestation: blake3:60568710b10db0428894acb2e678d16fbc0246fdb904d91b3ce3d5bbd1c9276e
+ liquid project deb blake3:8b2ebbbf6ac790878812f9952831e4f1ec838aed0fbe105f9916e6911a7b797b --name example --version 1.0.0 --arch arm64 --out ../repeat-out
artifact: ../repeat-out/example-1.0.0-1.arm64.deb sha256:c203d1ad7006f3d2473f274d057532135a4d60c6176bc349a7cb5b0e91a010ab application/vnd.debian.binary-package
attestation: blake3:370e9d2cb1d1a23597533fa79494641905a066d826f01431ead677df13b68658
+ cmp ../out/example-1.0.0-1.arm64.deb ../tree-out/example-1.0.0-1.arm64.deb
+ cmp ../out/example-1.0.0-1.arm64.deb ../repeat-out/example-1.0.0-1.arm64.deb
exit=0
```

## Executable guide

`guides-projection` executes the exact marked shell block in
[package ordinary files](../guides/project-from-files.md). Its captured output:

```text
initialised .
artifact: ../out/example-1.0.0-1.arm64.deb sha256:c203d1ad7006f3d2473f274d057532135a4d60c6176bc349a7cb5b0e91a010ab application/vnd.debian.binary-package
attestation: blake3:0a7f49ea622469c1975c9cdd55ce69c3ff9d66917787430d2b2236744c71cb36
artifact: ../image/example-1.0.0.oci sha256:fa29624ee2c84e01b6c673ed143c220bd764e6464bcc00a84572b214bdac44b0 application/vnd.oci.image.manifest.v1+json
attestation: blake3:ff4475082486aff2ac335bb8ddaff951fbd8d4c65e2671de8c376cb5fbd6a5ea
artifact: ../tree-out/example-1.0.0-1.arm64.deb sha256:c203d1ad7006f3d2473f274d057532135a4d60c6176bc349a7cb5b0e91a010ab application/vnd.debian.binary-package
attestation: blake3:0a7f49ea622469c1975c9cdd55ce69c3ff9d66917787430d2b2236744c71cb36
guides-run executed 1 marked shell blocks
```

## Existing result-ID compatibility

The public-API test fixture records a result in a separate build repository. The same result
is projected by both the saved baseline and changed binary; the package bytes are identical.
This fixture is only for the existing build-result compatibility check. The customer guide
needs no fixture or knowledge of build-cache storage.

```text
$ $REPO/.cache/issue-286/liquid-before project deb blake3:c06eab04431c5cda77766db69d0ced4ea4fccf90f8e453ef76ff3249d4f84f30 --name example --version 1.0.0 --arch arm64 --out $REPO/.cache/issue-286/before-result
artifact: $REPO/.cache/issue-286/before-result/example-1.0.0-1.arm64.deb sha256:dfd7126f56c09ac7fb1a4c6e82dbd02690bc8e67b506be68f41a73511c71a0a8 application/vnd.debian.binary-package
attestation: blake3:34172404752190af91ff28dff78f9aa9f96df17677c27a8600dba72ad9ea3408
attestation: blake3:d4fac4ca421139452e1ad2a31fb9f5da0180e6af27c27ae365b821c797a9745c
exit=0
$ $REPO/build/liquid project deb blake3:c06eab04431c5cda77766db69d0ced4ea4fccf90f8e453ef76ff3249d4f84f30 --name example --version 1.0.0 --arch arm64 --out $REPO/.cache/issue-286/after-result
artifact: $REPO/.cache/issue-286/after-result/example-1.0.0-1.arm64.deb sha256:dfd7126f56c09ac7fb1a4c6e82dbd02690bc8e67b506be68f41a73511c71a0a8 application/vnd.debian.binary-package
attestation: blake3:34172404752190af91ff28dff78f9aa9f96df17677c27a8600dba72ad9ea3408
attestation: blake3:d4fac4ca421139452e1ad2a31fb9f5da0180e6af27c27ae365b821c797a9745c
exit=0
before/after result-ID package bytes: identical
```

## Regression coverage

`cli-project` covers ordinary changes and trees, `.deb` byte identity, OCI/RPM/APK artifacts,
Debian source changes, explicit unsupported inputs, existing result IDs, producing install
trees, and a checkout with only a build repository. It asserts that ordinary projection does
not create the configured build-cache directory. `guides-projection` executes the user guide.

```sh
ctest --test-dir build --output-on-failure -R '^(cli-project|guides-projection)$'
```

```text
Test project $REPO/build
    Start 651: cli-project
1/2 Test #651: cli-project ......................   Passed    2.43 sec
    Start 655: guides-projection
2/2 Test #655: guides-projection ................   Passed    0.34 sec

100% tests passed, 0 tests failed out of 2

Label Time Summary:
liquid-cli    =   2.77 sec*proc (2 tests)

Total Test time (real) =   2.79 sec
```

## Initial native proof before rebase

The initial static build completed with exit 0. The static suite failed.
All executed projection cases, `cli-project`, and `guides-projection` passed in the full suite.
The shared suite was not started because the work order requires stopping when a proof
failure reproduces in isolation. No tests were weakened, and no commit, push, rebase, or
pull request was made.

```sh
ctest --test-dir build --output-on-failure
```

Temporary files used the worktree’s `.cache/issue-286/tmp` directory. The database URL
selected a fresh database on `127.0.0.1`; it was removed after verification. The complete
local log is `.cache/issue-286/ctest-static.log`.

```text

97% tests passed, 20 tests failed out of 687

Label Time Summary:
conformance       =   3.39 sec*proc (2 tests)
large             =   5.59 sec*proc (1 test)
liquid-cli        =  18.24 sec*proc (30 tests)
liquid-offline    =   9.89 sec*proc (1 test)
sim-long          =  35.26 sec*proc (6 tests)
vfs               =   3.39 sec*proc (2 tests)

Total Test time (real) = 284.51 sec

The following tests did not run:
	 68 - Http2Interop.GetsCapabilitiesFromNativeLinkOverH2c (Skipped)
	 69 - Http2Interop.GetsCapabilitiesFromNativeLinkOverTls (Skipped)
	100 - NodeStore.LocalMetadataMountsWhenItsContentStoreIsUnavailable (Skipped)
	101 - NodeStore.ReusesChunksAcrossTwoImagePullsAndPrefetchesEvictedContent (Skipped)
	138 - Projection.emit_peer_artifacts_when_requested (Skipped)
	139 - Projection.emit_debian_source_peer_when_requested (Skipped)
	140 - Projection.emit_native_homebrew_peer_when_requested (Skipped)
	144 - RemoteInterop.CasAndActionCacheRoundTripWithConfiguredPeer (Skipped)
	400 - VfsMount.hydrates_captures_and_reuses_cache (Skipped)
	474 - BuildWrap.mount_backed_scenarios_are_skipped_without_linux_fuse (Skipped)
	542 - HotUpdateDemo.TierThreeInvokesRestartHook (Skipped)
	680 - vfs-conformance-libfuse (Skipped)

The following tests FAILED:
	  9 - Drip.restart_replays_the_persisted_journal (Failed)
	 25 - Daemon.single_instance_attach_and_shutdown (Failed)
	 26 - Daemon.lock_and_unlock_publish_the_current_lock_set (Failed)
	 27 - Daemon.attribution_query_crosses_the_wire (Failed)
	 28 - Daemon.exits_when_idle_and_refuses_late_attach (Failed)
	 29 - Daemon.drip_invalidates_only_changed_paths_and_advances_targets (Failed)
	 30 - Daemon.consumer_lag_crosses_high_watermark_and_resumes_below_low_watermark (Failed)
	 31 - Daemon.stalled_target_pauses_above_high_watermark_and_resumes_after_delivery (Failed)
	 32 - Daemon.idle_capacity_validates_and_reports_graph_divergence (Failed)
	 33 - Daemon.command_starts_on_first_use_and_attaches_to_same_process (Failed)
	376 - SearchIndex.daemon_drip_indexes_new_working_tree_content (Failed)
	405 - BuildPlan.repository_without_configuration_has_named_error (Failed)
	530 - StructuralEdit.daemon_round_trip_and_cli_json_use_the_public_records (Failed)
	540 - HotUpdateDemo.TierOnePatchChangesARunningCapyLoop (Failed)
	645 - cli-scenario (Failed)                             liquid-cli
	647 - cli-conflict (Failed)
	660 - cli-locks (Failed)
	664 - cli-blame (Failed)
	666 - cli-format-diff (Failed)                          liquid-cli
	678 - cli-offline (Failed)                              liquid-cli liquid-offline
Errors while running CTest
```

### Isolated reruns and stop diagnosis

The initial drip failure said `FSEvents did not arm for workspace`. It passed when rerun alone
with one-minute load below 40:

```text
{ 18.00 31.43 34.53 }
Test project $REPO/build
    Start 9: Drip.restart_replays_the_persisted_journal
1/1 Test #9: Drip.restart_replays_the_persisted_journal ...   Passed    3.06 sec

100% tests passed, 0 tests failed out of 1

Total Test time (real) =   3.07 sec
```

The initial daemon failures said `daemon socket path is too long`. To keep all scratch files
inside the authorized worktree, a short macOS inode alias was tried for the same temporary
directory. `samefile` confirmed that alias and the worktree directory were the same directory.
The isolated daemon case still failed, this time when its canonical-path operation rejected the
alias. The load was below 40:

```text
{ 18.50 31.08 34.37 }
Test project $REPO/build
    Start 25: Daemon.single_instance_attach_and_shutdown
1/1 Test #25: Daemon.single_instance_attach_and_shutdown ...***Failed    0.06 sec
Running main() from $REPO/build/_deps/googletest-src/googletest/src/gtest_main.cc
Note: Google Test filter = Daemon.single_instance_attach_and_shutdown
[==========] Running 1 test from 1 test suite.
[----------] Global test environment set-up.
[----------] 1 test from Daemon
[ RUN      ] Daemon.single_instance_attach_and_shutdown
$REPO/tests/test_daemon.cpp:238: Failure
Value of: started
  Actual: false
Expected: true
filesystem error: in canonical: No such file or directory ["/.vol/16777231/355843252/lq-4063-1168895906-0/liquid-daemon-test-0"] [""]

[  FAILED  ] Daemon.single_instance_attach_and_shutdown (29 ms)
[----------] 1 test from Daemon (29 ms total)

[----------] Global test environment tear-down
[==========] 1 test from 1 test suite ran. (29 ms total)
[  PASSED  ] 0 tests.
[  FAILED  ] 1 test, listed below:
[  FAILED  ] Daemon.single_instance_attach_and_shutdown

 1 FAILED TEST


0% tests passed, 1 tests failed out of 1

Total Test time (real) =   0.07 sec

The following tests FAILED:
	 25 - Daemon.single_instance_attach_and_shutdown (Failed)
Errors while running CTest
```

The work-order stop rule applies: the unrelated daemon proof could not be made to pass under
the worktree-only constraint. The existing offline test also failed in the full run: both
`guides-resolution` and `guides-projection` encounter the guide runner’s loopback-port allocation
inside the offline sandbox. That failure was not rerun after the daemon gate stopped work.

## Initial deferred state

- Shared CTest, final rebase, push, and PR creation were deferred until the socket fixture landed.
- The known daemon and offline defects were outside issue #286.
- Debian source-policy requirements and overlay repository limits retain their existing scope;
  no packaging format, build-wrapper, or mount work was added.

## Files changed

- `CLAUDE.md`
- `README.md`
- `docs/README.md`
- `docs/build/README.md`
- `docs/cli/README.md`
- `docs/features/README.md`
- `docs/guides/README.md`
- `docs/library/README.md`
- `docs/library/projection.md`
- `docs/library/status.md`
- `docs/reference/cli/README.md`
- `docs/reference/cli/cli.json`
- `docs/reference/cli/project.md`
- `src/README.md`
- `src/cli/README.md`
- `src/cli/project.cpp`
- `src/cli/tree.cpp`
- `src/main.cpp`
- `tests/CMakeLists.txt`
- `tests/README.md`
- `tests/cli/README.md`
- `docs/build/project-ordinary-repository.md`
- `docs/guides/project-from-files.md`
- `docs/reference/cli/tree.md`
- `tests/cli/project.py`
- `tests/cli/project_fixture.cpp`

## Rebased native proof

The branch was rebased onto `origin/main` at `490222c`, which includes issue #308's bounded
socket fixture. Both build directories were deleted and configured again because the rebase
included CMake changes. Static and shared builds both completed with `-j 6`.

```sh
cmake -S . -B build -G Ninja && cmake --build build -j 6
cmake -S . -B build-shared -G Ninja -DBUILD_SHARED_LIBS=ON && cmake --build build-shared -j 6
ctest --test-dir build --output-on-failure
ctest --test-dir build-shared --output-on-failure
```

Both CTest runs passed 692 of 693 tests. `cli-project`, `guides-projection`, every daemon and
FSEvents case, and the new direct VFS conformance test passed. The only failure in each run was
the known clean-main `cli-offline` failure from issue #296. Its nested offline suite fails when
`tests/guides/run.py:64` binds a loopback port for each guide, including guides that do not need
one. The static load average recorded after the run was `{ 12.32 54.12 42.98 }`; the shared value
was `{ 14.47 20.39 28.42 }`.

```text
Static: 99% tests passed, 1 tests failed out of 693
Shared: 99% tests passed, 1 tests failed out of 693

The following tests FAILED:
    684 - cli-offline (Failed)  liquid-cli liquid-offline

offline CLI suite failed with 1:
  sandboxed liquid-cli tests failed with 8:
  PermissionError: [Errno 1] Operation not permitted
  tests/guides/run.py:64: environment["LIQUID_GUIDE_PORT"] = str(available_port())
```

The fresh PostgreSQL databases created for the static and shared runs were removed after each
suite. Linux proof is deferred: the work order's macOS static and shared suites are the required
native proof, and the optional Linux lane helper was unavailable.

An isolated static rerun of the only failing test reproduced the known offline limitation at a
one-minute load of `13.25`:

```text
1/1 Test #684: cli-offline ... Failed
sandboxed liquid-cli tests failed with 8:
  14/30 Test #660: guides-resolution ... Failed
  15/30 Test #661: guides-projection ... Failed
PermissionError: [Errno 1] Operation not permitted
tests/guides/run.py:64: environment["LIQUID_GUIDE_PORT"] = str(available_port())
```
