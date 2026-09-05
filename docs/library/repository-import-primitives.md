# Repository import primitives

**What.** The repository API exposes the primitives importers need without giving them direct
access to the store implementation.

## `record`

`repository::record(change_input)` snapshots the working tree and creates a signed change with
the caller-provided parent references, author, timestamp, message, metadata, and optional ref to
advance. Parents carry both a repository identity and a change identifier, so an imported merge or
combined repository can retain its complete graph. `cut(message)` records the current head as the
sole parent and advances `main`.

Change metadata is canonical CBOR text-to-text data. It is signed as part of the change body and
is intended for stable foreign-format identifiers such as a Git object name.

`repository::record_from_tree(change_input, digest)` has the same change and signing semantics,
but takes an already stored tree record as its snapshot. It lets a history importer retain each
historical snapshot without replaying that history through the working-tree documents.

## Local trees

`repository::import_tree(path, import_options)` is the local working-tree importer shared by
`liquid init`, `liquid import <path>`, and the build wrapper. It reports the optional one-drip
import identifier and file and byte counts. `.liquidignore` and caller patterns exclude paths;
links are skipped unless `follow_symlinks` is selected. The working tree is compared by blob
identity while it is walked, so an unchanged import produces no drip and a non-empty drip names
only the changed paths.

## Repository metadata

`set_metadata`, `metadata`, and `metadata_keys` persist opaque byte values in the repository
document. Importers use the `git.commit.<sha1>` and `git.change.<change-id>` keys for their
bidirectional identity mapping. Prefix enumeration is sorted and resumes an interrupted import
without scanning history.

## History traversal

`log()` traverses all locally stored reachable parents, returning each change once in
child-before-parent topological order. Equal timestamps are ordered by change identifier. A
`log_options::from` value selects another starting change; `first_parent` provides Git's familiar
linear traversal for callers that need it.

## Historical objects

`tree(digest)` decodes a stored tree record, and `blob(digest)` resolves a stored blob record to
its complete bytes. Together with `at(change_id)`, these operations let an exporter walk a
historical snapshot without assuming that the working copy is at that change.

## Modes and nested repositories

`write_options::mode` preserves executable (`0100755`) and symbolic-link (`0120000`) entries;
symbolic-link content is the link target. `nest_ref(path, repo_ref)` records a Git submodule as a
nested-repository placeholder without requiring its source repository to be present.

## Status

All importer primitives in the Phase 1 API map are implemented.
