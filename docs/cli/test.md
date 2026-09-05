# `liquid test`

`liquid test [filter]` runs the test targets stored in the current project's wrap repository. CMake
targets are discovered from `CTestTestfile.cmake` during `liquid wrap`; library consumers can also
register native targets through `repository::register_test`.

With no filter, every target runs in name order. A filter selects targets whose name or label
contains the supplied text:

```sh
liquid test
liquid test quick
liquid test wrapped-smoke
```

Each selected test is an ordinary observed action. If its previously read inputs still have the
same content, liquid reports a hit and reuses the stored exit status and output. A changed observed
input runs the test again. Output has one summary line per target:

```text
wrapped-smoke passed duration_ms=4 hit=false
```

A failure prints `failed`, then its combined stdout and stderr, and makes the command return
nonzero after the remaining selected tests finish. The same result is available as a public
`test_result` build event with `name`, `status`, `output`, `duration`, and `hit` fields.
