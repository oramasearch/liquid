# CLI tests

`scenario.cmake` drives the built `liquid` executable through the Phase 1 command-line scenario
without watcher capture: import files with `init`, inspect status, cut, move, nest a second
repository, split a subtree, combine it, inspect JSON history, undo the move, reject a locked
mutation, and dry-run collection. `help-docs.cmake` ensures every documented command is present
in the command table's `--help` output by extracting the command cell directly instead of
splitting a Markdown row as a CMake list. `public-includes.cmake` rejects a CLI source that
includes anything outside the public `liquid/` header tree. The `cli-why-json` and
`cli-cache-stats-json` cases exercise machine-readable diagnostics against an isolated empty
cache.
