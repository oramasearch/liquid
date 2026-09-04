# The object model and the metadata layer

**What.** How a liquid repository is represented: which records live in the content store
(immutable, content-addressed), which state lives in the CRDT metadata document (mutable,
concurrently edited), how the two reference each other, and how repository composition,
conflicts, undo and locks fall out of that. This is the design for liquid issues #2 (the object
model) and #3 (the CRDT layer); the implementation follows it literally, and the API is the one
in [`api-map.md`](api-map.md).

**Why.** features/01 fixes the split (bytes in the chunk store, namespace in the CRDT) and
features/02 fixes the behaviour (changes with identity, moves recorded not inferred, conflicts as
values, everything undoable, the working copy is a change, repositories as composable values).
What neither fixes is the shape of the records and the document, and the 2026-09-03 survey of
`automerge-cpp` found properties liquid must design around rather than assume (tracked as
https://github.com/oramasearch/automerge-cpp/issues/13): its change hashes do not cover
operation content, its default actor id is zero, it has no commit metadata, no incremental save,
no patches for remote changes, no rollback, no sync wire format, and no history compaction.
Everything below that says "liquid's own" is there because of one of those.

**Status: implemented for liquid #2 and #3.** The private automerge-cpp wrapper, canonical CBOR
codec, versioned sync framing and drip journal are implemented. Repository creation and opening,
working-tree reads and writes, recorded moves and removals, snapshots and changes, references,
composition, conflict values, operation inversion, advisory locks, and the repository status are
implemented over the local store.

## The two layers, precisely

| | Content store (liquid #1) | Metadata document (liquid #3) |
|---|---|---|
| Holds | chunks; and the **records** below, serialised canonically | the live namespace: the working tree, refs, locks, configuration, nested-repository pointers, the operation log |
| Mutability | immutable, content-addressed by `digest` | mutable, converged by automerge-cpp |
| Identity | the digest | liquid's own `node_id` per path (below), automerge `ObjId` internally |
| Scale | billions of chunks | one small repository document plus one document per directory (see *Document boundary*) |
| References | records reference each other by digest; never the document | the document references content by digest text; never the reverse |

## Records in the store

All records are encoded as **canonical CBOR** (RFC 8949 §4.2: definite lengths, sorted keys,
shortest integers) so the same value always has the same digest on every platform. Every record
starts with a `kind` and a `v` (format version).

```
blob        { v, kind:"blob", size, chunks:[digest…] }
tree        { v, kind:"tree", entries:[ { name, mode, kind:blob|tree|repo, id:node_id, ref:digest | repo_ref } … ] }   # sorted by name, bytewise
change      { v, kind:"change", parents:[ change_ref… ], author:actor, at:time, message,
              root:digest(tree), heads:[bytes…], conflicts?:[conflict…], symbols?:digest, sig:bytes }
repo        { v, kind:"repo", repo_id, name, created:time, root_change:change_ref? }
attestation, action, result   # kinds reserved now; records defined in Phase 2 (features/10, /04)
```

- **`node_id`** is 16 random bytes minted when a path is created. It is stored in the tree entry
  and in the document entry, and it is what survives a move: a move deletes the old key and puts
  the same `node_id` under the new key. This is liquid's own because automerge has no move
  operation and cannot re-parent an object; the CRDT sees two edits, liquid sees one move.
- **`change_ref` = `{ repo_id, id: digest(change) }`.** A change's identity is the digest of its
  canonical record, computed by liquid, **not** automerge's `ChangeHash` (which today does not
  cover operation content). Because the parent reference carries a `repo_id`, a change can have
  parents in two different repositories, which is what `combine` needs.
- **`heads`** are the automerge document heads the change was cut at; they tie the immutable
  record to the document's history so `at(change_id)` can rebuild the document state too, not
  only the tree.
- **`conflicts`** (liquid #40) and **`symbols`** (liquid #47) are the two fields that cannot be
  added later, so they are in the record from the first commit, optional and empty.
- **`sig`** is the TAAP signature over the record with `sig` absent (liquid #4).
- A `repo` record identifies a repository independently of where it lives; `repo_id` is 16 random
  bytes minted at `create`.

Trees and blobs are values: two repositories with the same content share every record and every
chunk, because the store is one.

## The metadata document

The **repository document** (one per repository; directories are their own documents, see *Document boundary*). Its root map:

```
{
  "repo":    repo_id (bytes),  "format": 1,
  "tree":    map                 # the working tree: the current change in progress
  "refs":    map name → change_id text        # "main" → "blake3:…"
  "locks":   map path-pattern → { by: actor, since: time, authority: "advisory"|"lake" }   # liquid #42
  "nested":  map path → { repo_id, pinned: change_id text }                              # nest/split
  "config":  map                 # lock patterns, encryption rules, chunking overrides
  "ops":     list of op          # the operation log, liquid #41
}
```

`tree` is a map of **directory objects**: each directory is an automerge map whose keys are
entry names and whose values are entry maps `{ kind, id: node_id, mode, ref: digest text }` for
files, or a nested directory map for subdirectories. Files reference blobs by digest text; the
document never holds bytes. A directory's automerge `ObjId` is internal; `node_id` is the
identity liquid exposes.

Every mutation of the document is a transaction, and every transaction is a **drip**
(features/22): the transaction's ops, the actor, the time and the signature form the drip
record. The document's history *is* the drip stream. A liquid `change` is cut from it: a named,
reviewable aggregation whose `root` is the tree snapshot written to the store at that moment.

Rules liquid's wrapper enforces on top of automerge-cpp, until the upstream issues close:

1. **Actor id is set, always**, before the first transaction: `digest(taap_id ‖ device_id)[0:16]`,
   stable per identity per device and unique across replicas. Never the library default.
2. **Commit metadata is liquid's**: message, time and author live in the drip and change records,
   not in automerge's `Change` (which cannot carry them).
3. **Observation is by diff**: after `merge`/`apply_changes`/sync, liquid compares heads and
   walks the changed objects itself; nothing relies on patches from remote changes.
4. **Persistence** is the whole document saved into the store as a blob at every change cut,
   plus liquid's drip journal between cuts; incremental save replaces the journal when it exists.
   A cut increments the repository's journal epoch, atomically installs the whole-document save,
   then resets the append-only journal. A replica behind the retained epoch starts from that cut
   instead of requesting discarded drips.
5. **Transactions never throw** past the boundary: liquid validates inputs before opening one,
   because automerge-cpp applies eagerly and cannot roll back.
6. **Sync framing is liquid's** (CBOR around `SyncMessage` and the changes it carries) until a
   wire format exists upstream; the framing is versioned like every record.

## Operations

| Operation | What happens |
|---|---|
| `write(path, bytes)` | `store.put` → blob digest; one transaction on the directory document: put or update the entry (minting `node_id` on create). That transaction is the drip. |
| `move(from, to)` / `remove(path)` | explicit operations, the API's own methods: a move within a directory is one transaction; across directories two, carrying the same `node_id`; `remove` deletes the entry and records the operation. |
| `cut(message)` | the explicit change cut: save touched directory documents, write the `tree` snapshot, build and sign the `change` record, update the ref, advance `head`; the working copy continues as a new change. |
| move | one transaction: delete the old key, put the entry with the **same** `node_id` under the new key. Recorded, never inferred. |
| cut a change | write the tree snapshot to the store (walk `tree`, emit `tree` records bottom-up), build the `change` record with `parents = heads of the ref`, `heads = document heads`, sign, `store.put`; set the ref. |
| `at(change_id)` | read the record; the snapshot is its `root` tree from the store. Rebuilding the document at that point uses `heads` and automerge's `get_at`, which is expensive today (upstream issue 9) and is not on the read path. |
| `log` | walk `parents` from a ref; identity is the digest, so a change transplanted to another branch is recognised as itself. |
| merge | automerge converges the document; liquid then runs structural merge (Phase 5) or, in Phase 1, a three-way merge on entries by `node_id`: same `node_id` changed on both sides with different `ref` is a **conflict value** appended to the resulting change's `conflicts`, never a broken working tree. |
| `nest(source, path)` | the entry at `path` becomes `kind: repo` with the source's `repo_id` and pinned change; `nested[path]` mirrors it. The nested repository keeps its own document and its own history; its files are visible because `root()` descends into nested repositories through the store. |
| `split(path, into)` | new `repo` record and document; the subtree's entries move (same `node_id`s) into it; the original keeps `path` as a `repo` entry. History of the subtree is preserved by construction: the old changes still reference the old trees, and the new repository's `root_change` names the change it was split from. |
| `combine(other, at)` | one change with parents in both repositories (`change_ref` carries `repo_id`); path collisions become conflict values. |
| `undo(op_id)` | every operation above appends an `op` record to `ops` (`{ id, kind, at, by, inverse }`); undo applies the inverse as a new operation. Undo never rewrites history. |
| `lock(path)` | `locks[pattern]` in the document, advisory offline; authoritative when lake holds the pattern (Phase 6). A write to a path locked by another actor is refused before any transaction opens. |

## Document boundary

**Decided (2026-09-04): one automerge document per directory.** The measurement below, made for
liquid #3 with one document per repository, rules that layout out: a million-entry document costs
2.1 ms per write, a 708-second load and 10.6 GiB resident, and the growth is nonlinear because
`load()` replays the whole history (automerge-cpp issue 4 and 9 on
https://github.com/oramasearch/automerge-cpp/issues/13). Directories are small; repositories are
not.

The layout from here:

- The **repository document** holds `repo`, `format`, `refs`, `locks`, `nested`, `config`, `ops`,
  the `node_id` of the root directory, and **`dirs`: a map from directory `node_id` to the digest of
  that directory document's latest saved bytes in the store**. That map is the mutable name→content
  indirection a content-addressed store lacks: to load a directory, look its `node_id` up in `dirs`,
  `store.get` the digest, `document::load`. A directory document is saved (and `dirs` updated in one
  repository-document transaction) at every cut and at a configurable interval; between saves the
  journal's drips replay onto the loaded document. The repository document itself is saved to the
  store at the same moments and its digest is written to `LIQUID_REPO` at the repository root,
  atomically via rename, so `open` has one fixed entry point.
- **Each directory is its own automerge document**, stored as a blob in the content store under
  a key derived from its `node_id`, holding one directory map of entries (`{kind, id, mode, ref}`
  for files; `{kind: "dir", id}` for subdirectories, whose own document is found by that `id`).
  Documents load lazily on first access and are cached by `node_id`.
- A **move within a directory** is one transaction on that document. A **move across
  directories** is two transactions (remove in the source, put in the target) carrying the same
  `node_id`; the operation log records it as one move, and a cut that observes only one half is
  a torn move the next cut completes (the `ops` record names both halves).
- **Drips** name the directory document(s) they touched; the journal is per repository. The
  journal is readable by **cursor**: `journal::read(cursor, max)` returns the next records and the
  cursor after them, `journal::end()` the current tail, and cursors are plain values consumers
  persist, so every consumer of the drip stream (capture's subscribers, the daemon, sync) resumes
  exactly where it stopped, at-least-once.
- **Cutting a change** walks directory documents from the root and emits `tree` records
  bottom-up as before; unchanged subtrees reuse their previous `tree` digest without loading
  their documents (the parent entry records the child's last tree digest, refreshed at cut).
- **Sync** exchanges the repository document and the directory documents that changed since the
  peer's heads, each with its own `sync_state`; the framing carries the `node_id` of the document
  a message belongs to.

Measured envelope of one document (M3 Max, 64 GiB, Release, `liquid-crdt-measure`):

| Entries | Transaction per write | `save()` size | `save()` time | `load()` time | Peak RSS |
|---:|---:|---:|---:|---:|---:|
| 100,000 | 117.179 µs | 2,046,818 bytes | 338.496 ms | 3,800.777 ms | 1,171.562 MiB |
| 1,000,000 | 2,086.627 µs | 20,176,218 bytes | 3,944.885 ms | 708,573.076 ms | 10,613.234 MiB |

A directory above roughly 10,000 entries is the warning threshold; `liquid status` reports such
directories so the cost is visible, and a later sharding of a single huge directory stays
possible because entries are addressed by `node_id`, not by position. Reproduce a row after a
Release configure with `cmake --build build-measure --target liquid-crdt-measure &&
./build-measure/tests/liquid-crdt-measure 100000 1000`.

## Implementation notes

`LIQUID_REPO` contains the digest text of the latest repository checkpoint. Its replacement is
written through Corosio to a sibling temporary file and atomically renamed. Each loaded directory
is a private CRDT document cached by its `node_id`; saving a checkpoint writes the document bytes
to the store and refreshes its `dirs` entry. A checkpoint occurs at `cut` and after the configured
drip interval (64 by default); an uncut write is recovered from the repository journal.

File content is represented by a canonical `blob` record whose chunk list currently contains the
content-store object written by `store::put`. Trees and changes are canonical records as well;
the change digest is the digest of its canonical bytes and is never an automerge hash. The
fixture in `tests/fixtures/records/empty-tree.hex` pins the canonical empty-tree encoding.

## How to verify

- Canonical encoding: supported RFC 8949 Appendix A values and bytewise map-key ordering are
  checked against `tests/fixtures/cbor/rfc8949-supported.txt`.
- Identity: create, move, move back; the entry's `node_id` never changes; the log shows two
  moves and no delete/create pairs.
- Composition: `split` then `nest` the result back; `root()` is byte-identical to before;
  `log` of the nested repository shows the pre-split history.
- Conflict: two replicas write the same path; after sync the change carries one conflict value
  and both `ref`s are recoverable from it.

The Phase 1 merge helper preserves both differing object references in a `conflict` value. The
entry remains readable from the local directory document while callers resolve that value.

## How to change

A new record kind is a row in the records block, a `kind` string, a version, and a fixture. A
new document key is a line in the root map and a rule for its merge semantics. Anything that
changes an existing record's canonical form bumps `v` and keeps the old decoder.
