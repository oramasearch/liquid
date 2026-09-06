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

`offline.cmake` runs every test carrying the `liquid-cli` label through `offline_runner.cpp`,
which invokes the same macOS or Linux sandbox launcher used by build wrapping with networking
denied. It first pins exit 69 and the exact diagnostic for push, pull, remote execution, and the
remote cache; any unclassified network use then fails inside the sandbox. The outer test carries
the `liquid-offline` label for local and hosted selection.
The help/documentation comparison also pins the `scheduler`, `worker`, `diff`, and `docs` command
names.
`configuration.cmake` checks a directory, lowers source origins in text and JSON, and formats the
same document twice to pin idempotence.
The help/documentation comparison also pins the `scheduler` and `worker` command names.
The scenario also pins `remote_bytes` in the status JSON contract.
It maps an anchor from the imported change through a later rename and checks the returned path and
flags.
It lists history through that rename, restores the imported version as a new drip, and cuts the
restoration without changing an earlier change.
It also parses `search --explain` as JSON and pins the scan choice, literal classification,
freshness input, and three-alternative shape for the small imported working set.

`format-diff.cmake` creates two changes through the public repository API and proves that
`diff --formats` prints a ZIP member change and an HDF5 dataset change, including codec and tier.

`symbols-python.cmake` creates a Python source file and proves `liquid symbols` selects the locked
Python grammar and reports `origin=syntactic`.

`retain-explain.cmake` reads the fixture's retained change digest and proves the command names the
persisted rule, lease ID, and expiry.

The SBOM command tests exercise SPDX and CycloneDX output for the wrapped CMake fixture and
check each format's identifying field.
`delta.cmake` drives create, apply, and catalog fetch through the executable and compares target,
applied, and fetched file digests.
`diff.cmake` cuts two changes and checks that the public repository diff is rendered by the CLI.
`secret.cmake` seals file input to raw CBOR and base64, unseals both forms through the executable,
and proves that the plaintext is absent from stderr.

`cli-identity-codec` runs `liquid identity show` and `liquid identity rotate` with an isolated home,
then passes the stored initial and successor documents to a fixture that uses only the public
identity encode, decode, and verification APIs.

The admission process test invokes `attest image` and verifies its printed digest through
`attest verify`, then starts the built `serve admission` command over TLS and explicit insecure
HTTP and posts committed Kubernetes fixtures.
`cli-keys` creates an encrypted store through `store_options::custody`, then drives `keys show`,
`rotate`, `revoke`, `rewrap`, and `split`. It checks the rotation disclosures, the one-line mutation
reports, all emitted Shamir shares, and the final persisted custody kind.
