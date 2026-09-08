# Standalone repository splits

`repository::split(path, into)` and `liquid split <path> <into>` accept an ordinary directory
or a nested repository. `into` is always a filesystem destination with its own `.liquid-store`,
including when the source handle borrows a shared store. Reopening the result requires no source
handle, source checkout, or shared-store option.

## Data and history

The operation copies and verifies the complete object graph needed by the working subtree and
retained history: trees, blob records, content chunks, nested changes, and historical directory
archives. A missing or unreadable object is an error. The destination never substitutes a remote
location for a local copy. Content-defined chunk and object identities are unchanged.

An ordinary directory receives a new repository identity and a signed split change whose parent
is the source's existing head. The log follows that recorded origin and exposes the original
signed changes with their existing IDs, authors, messages, and timestamps. No original change is
rewritten: historical snapshots retain their original root paths, and stable-node file history
follows the file back through those paths. Consequently the copied history can include sibling
paths from the original repository. Splitting is not a way to remove confidential history.

A nested repository keeps its repository identity and pinned history. Nesting first copies the
child's referenced data and history into the parent, so splitting later in another process does
not need the original child checkout. The parent retains the split subtree as a nested entry;
the split operation records its before and after values for undo.

Two nested paths can refer to the same immutable tree after a split is combined back into its
parent. The persisted nested-entry archive stores that tree once and restores it for both paths;
the tree records retain each path's own nested repository reference.

## Publication and errors

The destination must not already exist, and its parent directory must exist. Existing files,
directories, repositories, and symbolic links are refused without replacement. Work is staged in
a uniquely created sibling directory, checkpointed, closed, renamed, and reopened before success
is reported. Failures remove the staged or newly published destination. No incomplete repository
marker is left at the requested path.

Copying keyed object identities without an independent destination key policy is refused rather
than exporting decrypted bytes. Missing nested history, unavailable content, and invalid paths
also fail with a nonzero CLI status. The source is only changed after the destination is complete.

## Verification

`cli-split` runs both issue #2 reproductions through separate CLI processes. It moves the ordinary
source, removes the original nested checkout before splitting, deletes the parent before reading
the nested result, compares materialised bytes, checks both prior-history messages, and checks
failure cleanup. Repository regressions additionally check historical file bytes, stable file
and change IDs, and independent reopening after the source's borrowed store is removed.
They also reopen a parent after combining its just-split repository, which verifies that shared
nested tree archival does not encode duplicate records.

Run the focused tests with:

```sh
ctest --test-dir build --output-on-failure -R 'cli-split|Repository.split'
```

The full static and shared suites remain required. The source-capture issue #282, installed-library
linking #285, ordinary-repository projection #286, and platform mount paths are outside this change.
