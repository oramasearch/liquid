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

## Status

`record(change_input)` and repository metadata are implemented. The remaining importer primitives
are documented here as they land.
