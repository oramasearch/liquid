# VFS conformance proof (#301)

Recorded on native macOS on 2026-09-08. The direct driver is a test calibration reference;
passing it means agreement with liquid's own core. The Linux libfuse baseline remains owed
against #299. See the [run and extension guide](vfs.md#backend-conformance-301).

## Baseline

```sh
cmake -S . -B build -G Ninja
cmake --build build -j 6
ctest --test-dir build --output-on-failure -V -R '^vfs-conformance-'
```

```text
VFS conformance backend=direct backend_required=true
[==========] Running 9 tests from 1 test suite.
[----------] Global test environment set-up.
[----------] 9 tests from VfsConformance
[ RUN      ] VfsConformance.getattr_readdir_open_read_use_recorded_input_tree
[       OK ] VfsConformance.getattr_readdir_open_read_use_recorded_input_tree (357 ms)
[ RUN      ] VfsConformance.create_write_release_capture_bytes_and_attribution
[       OK ] VfsConformance.create_write_release_capture_bytes_and_attribution (469 ms)
[ RUN      ] VfsConformance.truncate_chmod_and_open_trunc_preserve_file_type_and_bytes
[       OK ] VfsConformance.truncate_chmod_and_open_trunc_preserve_file_type_and_bytes (436 ms)
[ RUN      ] VfsConformance.rename_unlink_update_names_bytes_and_stable_identity
[       OK ] VfsConformance.rename_unlink_update_names_bytes_and_stable_identity (377 ms)
[ RUN      ] VfsConformance.synthetic_mtimes_distinguish_sources_outputs_and_mutations
[       OK ] VfsConformance.synthetic_mtimes_distinguish_sources_outputs_and_mutations (373 ms)
[ RUN      ] VfsConformance.readonly_observed_mount_refuses_mutations_without_capture
[       OK ] VfsConformance.readonly_observed_mount_refuses_mutations_without_capture (214 ms)
[ RUN      ] VfsConformance.mutable_mount_preserves_readonly_dependency_inputs
[       OK ] VfsConformance.mutable_mount_preserves_readonly_dependency_inputs (223 ms)
[ RUN      ] VfsConformance.xattrs_get_list_set_remove_and_report_invalid_requests
[       OK ] VfsConformance.xattrs_get_list_set_remove_and_report_invalid_requests (276 ms)
[ RUN      ] VfsConformance.read_hydrates_missing_range_through_queue_and_persistent_cache
[       OK ] VfsConformance.read_hydrates_missing_range_through_queue_and_persistent_cache (425 ms)
[----------] 9 tests from VfsConformance (3154 ms total)

[----------] Global test environment tear-down
[==========] 9 tests from 1 test suite ran. (3154 ms total)
[  PASSED  ] 9 tests.
```

No direct case skipped. The `libfuse` registration is retained and reports:

```text
mount: named backend unavailable: macFUSE was not found at build time; install macFUSE and rebuild liquid: liquid #6
[  PASSED  ] 0 tests.
[  SKIPPED ] 9 tests
VFS conformance baseline NOT established: incomplete operation coverage
```

## Deliberate failure and restoration

Only the direct driver's `st_size` assignment was changed from `file->size` to `file->size + 1`.
The production libfuse files and the case expectations were unchanged.

```sh
cmake --build build --target liquid-vfs-conformance -j 6
build/tests/liquid-vfs-conformance --backend=direct \
  --gtest_filter=VfsConformance.getattr_readdir_open_read_use_recorded_input_tree
```

Exit status: `1`. Output:

```text
VFS conformance backend=direct backend_required=true
Note: Google Test filter = VfsConformance.getattr_readdir_open_read_use_recorded_input_tree
[==========] Running 1 test from 1 test suite.
[----------] Global test environment set-up.
[----------] 1 test from VfsConformance
[ RUN      ] VfsConformance.getattr_readdir_open_read_use_recorded_input_tree
tests/vfs/test_conformance.cpp:372: Failure
Expected equality of these values:
  attributes.st_size
    Which is: 16
  static_cast<off_t>(recorded_input.size())
    Which is: 15
getattr: expected st_size to equal recorded input bytes
Google Test trace:
tests/vfs/test_conformance.cpp:224: backend=direct

[  FAILED  ] VfsConformance.getattr_readdir_open_read_use_recorded_input_tree (336 ms)
[----------] 1 test from VfsConformance (336 ms total)

[----------] Global test environment tear-down
[==========] 1 test from 1 test suite ran. (336 ms total)
[  PASSED  ] 0 tests.
[  FAILED  ] 1 test, listed below:
[  FAILED  ] VfsConformance.getattr_readdir_open_read_use_recorded_input_tree

 1 FAILED TEST
```

The assignment was restored, the target rebuilt, and the named suite rerun:

```text
[  PASSED  ] 9 tests.
100% tests passed, 0 tests failed out of 2
vfs-conformance-libfuse (Skipped)
```

## Full native suites

```sh
ctest --test-dir build --output-on-failure
```

```text
99% tests passed, 1 tests failed out of 685
Total Test time (real) = 336.53 sec
The following tests FAILED:
    676 - cli-offline (Failed) liquid-cli liquid-offline
```

Twelve tests skipped for unavailable integration peers, mounts or optional scenarios. The only
failure is the work order's known #296 exception. Its nested `guides-resolution` failure was:

```text
File "tests/guides/run.py", line 22, in available_port
    listener.bind(("127.0.0.1", 0))
PermissionError: [Errno 1] Operation not permitted
```

`sysctl -n vm.loadavg` when this failure was inspected: `{ 36.68 44.81 30.31 }`.
Both active HotUpdateDemo tests passed; its optional restart-hook scenario skipped. No #282
failure appeared in this static run.

The shared suite used a fresh maintenance database on the local PostgreSQL server:

```sh
createdb -h 127.0.0.1 -U "$USER" liquid_issue301_proof_20260908_1906
export LIQUID_TEST_POSTGRES_URL=postgresql://$USER@127.0.0.1:5432/liquid_issue301_proof_20260908_1906
export CMAKE_BUILD_PARALLEL_LEVEL=6
cmake -S . -B build-shared -G Ninja -DBUILD_SHARED_LIBS=ON
cmake --build build-shared -j 6
ctest --test-dir build-shared --output-on-failure -V -R '^vfs-conformance-'
ctest --test-dir build-shared --output-on-failure
```

```text
VFS conformance backend=direct backend_required=true
[==========] Running 9 tests from 1 test suite.
[  PASSED  ] 9 tests.
100% tests passed, 0 tests failed out of 2
vfs-conformance-libfuse (Skipped)

99% tests passed, 2 tests failed out of 685
Total Test time (real) = 348.58 sec
The following tests FAILED:
    361 - InlineScanning.blocked_event_discloses_only_safe_coordinates (Failed)
    676 - cli-offline (Failed) liquid-cli liquid-offline
```

Twelve tests skipped. `cli-offline` again failed on the sandboxed resolution guide's loopback
bind (#296). The unrelated FSEvents startup failure belongs to the #282 family:

```text
Value of: opened.has_value()
  Actual: false
Expected: true
FSEvents did not arm for workspace

C++ exception with description "capture did not open" thrown in the test body.
[  FAILED  ] InlineScanning.blocked_event_discloses_only_safe_coordinates (6629 ms)
```

Load when inspected: `{ 53.07 46.58 34.15 }`. After the full suite ended, load was checked again
and was below the work order's threshold: `{ 31.51 40.29 33.83 }`. The isolated rerun passed:

```sh
sysctl -n vm.loadavg
ctest --test-dir build-shared --output-on-failure \
  -R '^InlineScanning\.blocked_event_discloses_only_safe_coordinates$'
```

```text
1/1 Test #361: InlineScanning.blocked_event_discloses_only_safe_coordinates ... Passed 1.45 sec
100% tests passed, 0 tests failed out of 1
Total Test time (real) = 1.46 sec
```

This is recorded as host contention under the work order's exception; the reviewer reruns the
suite alone before merging. No conformance case failed outside the intentional mutation.

After verification, `git fetch origin`, `git rebase origin/main` and
`git submodule update --init --recursive` completed. The base remained
`e5db43f2dd33a29868d7dcddc93600e47dd03236`; no incoming files or submodule pointers changed,
so a clean rebuild after rebase was unnecessary.

Documentation checks: zero broken internal links, 27 research topics, 24 source records,
and 25 feature pages. All 177 original expectation messages were preserved. All added C++ files
passed the repository's clang-format check, and production `src/vfs` has no diff.

## Outstanding coverage

Linux proof deferred: this work order excludes Linux lane attempts and macFUSE installation.
The direct driver does not prove kernel cache coherence, mount lifecycle under cancellation,
crash atomicity, handles across rename/unlink, hard links, symlink traversal, locks, mmap,
durability barriers, all xattr flags, directory mutations, or Windows operation behavior.
The real backends in #302, #303 and #304 must each pass the common cases themselves.
