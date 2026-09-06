# CLI tests

`scenario.cmake` drives the built `liquid` executable through the Phase 1 command-line scenario
without watcher capture: import files with `init`, inspect status, cut, move, nest a second
repository, split a subtree, combine it, inspect JSON history, undo the move, reject a locked
mutation, and dry-run collection. It reads signed drips through `log --ops --json`, recorded
operations through `ops --json`, and reverses a selected session path while checking the applied
report. `help-docs.cmake` checks command and flag names in both directions against generated
command help, and rejects an undeclared flag before any handler runs. `reference-docs.cmake`
regenerates the committed Markdown and JSON reference; `man-pages.cmake` checks the master and
per-command manual pages. `public-includes.cmake` rejects a CLI source that includes anything
outside the public `liquid/` header tree or the CLI's own metadata header. The `cli-why-json` and
`cli-cache-stats-json` cases exercise machine-readable diagnostics against an isolated empty
cache.
The help/documentation comparison also pins the `scheduler`, `worker`, `diff`, and `docs` command
names.
The scenario also pins `remote_bytes` in the status JSON contract.
It maps an anchor from the imported change through a later rename and checks the returned path and
flags.
It also parses `search --explain` as JSON and pins the scan choice, literal classification,
freshness input, and three-alternative shape for the small imported working set.

`symbols-python.cmake` creates a Python source file and proves `liquid symbols` selects the locked
Python grammar and reports `origin=syntactic`.

`retain-explain.cmake` reads the fixture's retained change digest and proves the command names the
persisted rule, lease ID, and expiry.

The SBOM command tests exercise SPDX and CycloneDX output for the wrapped CMake fixture and
check each format's identifying field.
`delta.cmake` drives create, apply, and catalog fetch through the executable and compares target,
applied, and fetched file digests.
`diff.cmake` cuts two changes and checks that the public repository diff is rendered by the CLI.

`cli-identity-codec` runs `liquid identity show` and `liquid identity rotate` with an isolated home,
then passes the stored initial and successor documents to a fixture that uses only the public
identity encode, decode, and verification APIs.
