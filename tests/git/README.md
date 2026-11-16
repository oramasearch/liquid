# Git interoperability tests

`test_object_store.cpp` covers verified loose objects, packed objects and deltas, tree names,
and complete history import/export with the system Git executable. Hand-written pack and
index fixtures exercise malformed delta chains without Git filtering their bytes first.
Subprocess checks isolate inputs that exhausted the old reader's stack or threw during
allocation; one reads a maximum-depth chain on a 512 KiB thread, the macOS pool default.

Build `liquid-git-tests` and run `ctest --test-dir <build> --timeout 600 -R
'GitObjectStore|GitInteroperability' --output-on-failure`. Resource limits and compatibility
are documented in [Git interoperability](../../docs/library/git-interop.md).
