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

## Status

`record(change_input)`, repository metadata, and full-DAG history traversal are implemented. The
remaining importer primitives are documented here as they land.
