# The object model and the metadata layer

**What.** How a liquid repository is represented: which records live in the content store
(immutable, content-addressed), which state lives in the CRDT metadata document (mutable,
concurrently edited), how the two reference each other, and how repository composition,
conflicts, undo and locks fall out of that. This is the design for liquid issues #2 (the object
model), #3 (the CRDT layer), and #41 (operation-log undo); the implementation follows it
literally, and the API is the one in [`api-map.md`](api-map.md).

**Why.** features/01 fixes the split (bytes in the chunk store, namespace in the CRDT) and
features/02 fixes the behaviour (changes with identity, moves recorded not inferred, conflicts as
values, everything undoable, the working copy is a change, repositories as composable values).
What neither fixes is the shape of the records and the document. The 2026-09-03 survey of
`automerge-cpp` tracked the missing capabilities in
https://github.com/oramasearch/automerge-cpp/issues/13. The dependency tip adopted by liquid #136
closes that epic: change hashes and chunk encoding are upstream-compatible, public failures are
checked, transactions roll back, commit metadata and incremental saves exist, remote changes
return patches, text indices count scalar values, sync uses the upstream wire format, and compact
checkpoints support both full and shallow loading.

**Status: implemented for liquid #2, #3, #30, #40, #41, #42, #68, #69, #195's history/conflict persistence, and #205.**
The private automerge-cpp wrapper, canonical CBOR codec, versioned sync framing and drip journal
are implemented. Repository creation and opening, working-tree reads and writes, recorded moves
and removals, snapshots and changes, references, composition, persisted conflict values, path
history and restore, session-scoped operation inversion, historical document reads, exclusive
checkout locks, and repository status are implemented over either a repository-local store or a
caller-owned store shared by multiple repositories.

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
blob/1      { v:1, kind:"blob", size, chunks:[digest…], chunk_sizes:[uint…],
              format_entries?:[digest…] }
blob/2      { v:2, kind:"blob", digest, size, chunks:[digest…], chunk_sizes:[uint…],
              remote?:{source,instance}, format_entries?:[digest…] }
tree        { v, kind:"tree", entries:[ { name, mode, kind:blob|tree|repo|conflict,
              id:digest, node:node_id, ref:digest, nested?:repo_ref } … ] }   # sorted by name, bytewise
conflict/1  { v:1, kind:"conflict", base:digest, left:digest, right:digest,
              region:{path, bytes:{offset,length}|nodes:{first,last}},
              merge_kind:text|structural|crdt-suspicious|suspicious-merge,
              explanation:text, proposal?:digest, reasons?:[text...], node?:text }
change/1    { v:1, kind:"change", parents:[ change_ref… ], author:actor, at:time, message,
              root:digest(tree), conflicts?:[conflict…], symbols?:digest, sig:bytes }
change/2    { v:2, kind:"change", parents:[ change_ref… ], author:actor, at:time, message,
              root:digest(tree), document:{refs,locks,nested},
              heads:[{directory:node_id,values:[bytes…]}…], conflicts?:[conflict…],
              symbols?:digest, sig:bytes }
change/3    { v:3, kind:"change", parents:[ change_ref… ], author:actor, at:time, message,
              root:digest(tree), document:{refs,locks,nested}, heads:[...],
              conflicts:[{path,id:digest(conflict)}...],
              resolutions:[{conflict:digest,value:digest}...], symbols?:digest, sig:bytes }
repo        { v, kind:"repo", repo_id, name, created:time, root_change:change_ref? }
attestation { v, kind:"attestation", subject:digest, profile, subject_kind?, attributes?,
              attribute_disclosure_commitments?, detectors:[text...], findings:uint, at:time,
              issuer:actor, allowances:[scan allowance...], sig:bytes }
action      { v, kind:"action", inputs:digest(tree), command:[text...], cwd, env, outputs,
              platform, toolchain:digest(tree), sandbox:digest, timeout_s?, remote_eligible? }
result      { v, kind:"result", action:digest, outputs, manifest, producer, provenance?, sboms?, sig }
sbom        { v, kind:"sbom", format:text, document:bytes }
hot-update-evidence { v:1, kind:"hot-update-evidence", result, artifact, changed_input,
                      units:[{kind,name,signature,source,output}...], abi_descriptor? }
patch       { v:1, kind:"patch", supersedes:result_id, target:result_id, tier:1|2,
              symbols:[text...], object:digest }
hot-update-outcome { v:1, kind:"hot-update-outcome", supersedes:result_id,
                     target:result_id, tier:0|3|4, outcome:"patched"|"restart", symbols, evidence }
query-graph { v:1, kind:"query-graph", nodes:[digest(query-node)…] }
query-node  { v:1, kind:"query-node", key, fingerprint, durability,
              dependencies:[{key,fingerprint}…], value:digest }
package/1   { v:1, kind:"package", name, versions, variants, dependencies, provides,
              conflicts, compilers, targets, patches }
concretised_spec/1 { v:1, kind:"concretised_spec", name, version, variants, compiler?, target,
                     dependencies:[{name,type,target,virtual}…], patches, reasons }
```

**Tree entry names are single safe path components** (liquid #386). A name is refused when it is
empty, `.` or `..`; contains `/`, `\` or NUL; or is a spelling of `.git` that Git, macOS or Windows
treats as `.git`: any letter case, with HFS+ ignorable code points (U+200C to U+200F, U+202A to
U+202E, U+206A to U+206F, U+FEFF) removed, with trailing dots or spaces, followed by a `:` stream
name, or the short name `GIT~1`. The rule is applied in four places: `decode_tree` refuses such a
record as `errc::corrupt`; `encode(tree)` and every repository write path refuse it as
`errc::invalid_argument`; the Git tree parser refuses it during `liquid import git`; and both
materialisers (`vfs::materialise` and the build materialiser) check every name again before they
touch the filesystem. The materialisers also create each entry exclusively and never follow a
symbolic link, so two names that a case- or normalisation-insensitive filesystem treats as one
(`README` and `Readme` on APFS, or precomposed and decomposed `é`) fail with
`errc::already_exists` instead of one overwriting the other. The rule lives in
`src/objects/entry_name.hpp`; the no-follow writes in `src/fs/contained.hpp`.

The query graph uses its own store directory and atomic text `ROOT`. Query keys and dependency
keys are canonical CBOR; dependencies retain read order. Query values are separate store objects,
so opening the graph loads root and node metadata while value bytes remain shallow until demand.
Only a completed query replaces `ROOT`; interrupted computations leave the previous complete graph
readable.

`format_entries` retains logical blobs decoded from a Tier 1 or Tier 2 container. It is absent for
ordinary and legacy blob records, preserving their canonical encoding. Collection follows these
digests while the parent container is reachable; the container's original chunks remain the
authoritative bytes.

- **`node_id`** is 16 random bytes minted when a path is created. It is stored in the tree entry
  and in the document entry, and it is what survives a move: a move deletes the old key and puts
  the same `node_id` under the new key. This is liquid's own because automerge has no move
  operation and cannot re-parent an object; the CRDT sees two edits, liquid sees one move.
- **`change_ref` = `{ repo_id, id: digest(change) }`.** A change's identity is the digest of its
  canonical liquid record, **not** automerge's upstream-compatible `ChangeHash`. The identifiers
  have different domains: the liquid record includes the tree, TAAP author, signature, conflicts,
  and repository parents. Because the parent reference carries a `repo_id`, a change can have
  parents in two different repositories, which is what `combine` needs.
- **`document` and `heads`** in `change/2` capture refs, lock paths, nested-repository pointers,
  and the heads of every directory document at the cut. They tie the immutable record to the full
  CRDT archives so `document_at(change_id)` can validate the historical point without consulting
  or modifying an active shallow document. Version 1 changes remain readable for tree history;
  they do not have a historical document projection.
- **`conflicts`** names unresolved `conflict/1` records. Version 3 replaces the legacy inline
  references with one digest per path and adds **`resolutions`**, which binds a resolved conflict
  digest to the selected value. See [`conflicts.md`](conflicts.md).
- **`symbols`** (liquid #47) remains an optional record reference.
- **`sig`** is the TAAP signature over the record with `sig` absent (liquid #4).
- Attestation schemas, subject kinds, freshness rules, and selective-disclosure commitments are
  registered by liquid #62. `scan_result/1` (liquid #50) retains its original record bytes; it is
  referenced by each scanned drip and records detector ids, finding count, and configured
  exceptions, never matched content. See [`attestations.md`](attestations.md) and
  [`secret-scanning.md`](secret-scanning.md).
- Each result's `sboms` array reaches signed `sbom/1` attestations. Their `document` attribute
  reaches a canonical `sbom` record containing the base SPDX or CycloneDX JSON. See
  [`sbom.md`](sbom.md).
- Hot-update evidence is associated with its result in repository metadata. Signed patch records
  form a `supersedes` chain back to the base artifact; restart records preserve the classifier
  evidence for the non-patching outcome. See [`hot-update.md`](hot-update.md).
- A `repo` record identifies a repository independently of where it lives; `repo_id` is 16 random
  bytes minted at `create`.
- Package declarations and complete concretisations use the same canonical-CBOR rules. A
  concretised spec's digest includes every bound dimension and dependency edge and is its reusable
  build identity. See [`resolution.md`](resolution.md).

Trees and blobs are values: two repositories with the same content share every record and every
chunk, because the store is one.

## Repository placement

`repository_options::store` may point to an open caller-owned `store`. In that form, the path
argument to `repository::create` and `repository::open` is an opaque repository identifier rather
than a checkout path. libliquid hashes that identifier into
`<store>/repositories/<identifier-hash>/`, where it keeps that repository's `LIQUID_REPO` and
`LIQUID_JOURNAL` entry points. Repository checkpoints and directory documents remain ordinary
content-addressed objects in the shared store. The caller must keep the store alive until every
repository opened on it has been destroyed.

When `repository_options::store` is null, the path retains its original meaning and libliquid
opens `<path>/.liquid-store`. `repository_options::storage` configures that owned store. A split
always creates an independent filesystem destination with its own store, including when the source
borrows a shared store; see [`split.md`](split.md) for ownership, history, and failure behavior.

Local writes retain `blob/1` identity. A remote output uses `blob/2`: `digest` identifies the
complete byte stream while the parallel chunk arrays define independently verifiable range
fetches. The optional location is routing metadata, not part of the content digest. An action is
version 2 only when `remote_eligible` is true, so default-off actions retain their version 1 bytes.

## The metadata document

The **repository document** (one per repository; directories are their own documents, see *Document boundary*). Its root map:

```
{
  "repo":    repo_id (bytes),  "format": 1,
  "tree":    map                 # the working tree: the current change in progress
  "refs":    map name → change_id text        # "main" → "blake3:…"
  "locks":   [{ path, holder: actor, since: time, reason, authority: "advisory"|"lake" } …]
  "nested":  map path → { repo_id, pinned: change_id text }                              # nest/split
  "config":  {
    "encrypt": { "paths": [ { "path": glob, "key_id": text,
                                "mode": "leaf"|"file", "encrypted_regex": text } … ] }
    "retention": { "rules": [
      { "type": "refs", "name": text, "glob": glob, "forever": true } |
      { "type": "refs"|"results", "name": text, "duration_s": uint } |
      { "type": "size_cap", "name": text, "bytes": uint }
    ] },
    "agents": { "require_base": bool, "actors": [ taap_id ... ] }
  }                              # ordered path rules; first matching path/ref rule wins
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

Repository write drips use signed body version 2. In addition to the liquid-selected `base`, the
body may carry `reasoned_from = { change, cursor? }`, which is the state the caller read before it
planned the write. It also carries every written path, and `revalidated = disjoint|forced` when a
moved base was explicitly accepted. The decoder continues to accept version 1 bodies, including
redaction records.

`config.agents.require_base` applies only to the TAAP identifiers in `config.agents.actors` until
delegation supplies classification. A listed writer without `write_options::base` receives
`errc::invalid_argument` before content storage or a directory transaction begins. Unlisted
writers retain the ordinary base-optional API.

A write base has moved when the current head differs or the repository journal has advanced beyond
the optional cursor. The material path set is the path being written plus `depends_on`, and path
comparison includes ancestors and descendants. `refuse` rejects any moved base;
`accept_if_disjoint` proceeds only when no later drip touched that set; `force` records that the
caller re-planned. `explain_stale` returns the current head, later-drip count, changed paths, and
the same UTF-8 line hunks as `diff(..., { .text = true })` without appending a drip or changing the
working tree.

Rules liquid's wrapper enforces on top of automerge-cpp:

1. **Actor id is set, always**, before the first transaction: `digest(taap_id ‖ device_id)[0:16]`,
   stable per identity per device and unique across replicas. Never the library default.
2. **Commit metadata is carried twice for different purposes**: the automerge change receives the
   drip message and millisecond timestamp through `CommitOptions`; the signed liquid drip and
   change records remain the authority for attribution.
3. **Remote observation uses patches**: apply, merge, and sync receive use the
   `*_with_patches` entry points. Sync batches are received into blank checked documents before
   those documents merge into populated candidates. Every returned `std::expected` is checked
   before the candidate document replaces the active document.
4. **Persistence checks its checkpoints**: `save_compact()` output is preferred at every cut and
   every 64 drips. It is immediately reopened as a sync-capable document; a compact form that fails
   that check is replaced with complete upstream history bytes. Later journaled drips replay on
   the checked checkpoint.
5. **Errors remain values across the wrapper**: local operation failures become
   `errc::invalid_argument`; malformed saves, changes, merges, and sync input become
   `errc::corrupt`. A failed external application leaves the active document unchanged.
6. **Sync messages use automerge's v1 wire format.** Liquid's repository transport associates
   each message with its directory `node_id`; the inner message bytes come from automerge-cpp.

## Operations

| Operation | What happens |
|---|---|
| `write(path, bytes, options)` | Validate the caller-stated base and read set, then FastCDC logical chunks → `store.put` per chunk → blob digest; one transaction on the directory document: put or update the entry (minting `node_id` on create). That transaction is the drip. |
| `move(from, to, options)` / `remove(path, options)` | Validate the same write options, then perform explicit operations: a move within a directory is one transaction; across directories two, carrying the same `node_id`; `remove` deletes the entry and records the operation. |
| `cut(message)` | the explicit change cut: save touched directory documents, write the `tree` snapshot, build and sign the `change` record, update the ref, advance `head`; the working copy continues as a new change. |
| move | one transaction: delete the old key, put the entry with the **same** `node_id` under the new key. Recorded, never inferred. |
| cut a change | write the tree snapshot to the store (walk `tree`, emit `tree` records bottom-up), build the `change` record with `parents = heads of the ref`, `heads = document heads`, sign, `store.put`; set the ref. |
| `at(change_id)` / `document_at(change_id)` | `at` reads the immutable `root` tree. `document_at` loads full-history compact archives, resolves every recorded head through the wrapper's `get_at`, and returns the recorded refs, locks, and nested pointers. Historical reads never touch the active shallow documents. |
| `log` | walk `parents` from a ref; identity is the digest, so a change transplanted to another branch is recognised as itself. |
| merge | Automerge converges the directory document, then liquid finds the common change and structurally merges base, local, and received blobs for each concurrently changed `node_id`. Disjoint definition edits replace the selected entry. Overlap, grammarless text, and suspicious proposals become conflict values. The operation records the algorithm, grammar digest, path, and three inputs. |
| `nest(source, path)` | the entry at `path` becomes `kind: repo` with the source's `repo_id` and pinned change; `nested[path]` mirrors it. The nested repository keeps its own document and its own history; its files are visible because `root()` descends into nested repositories through the store. |
| `split(path, into)` | new `repo` record and document; the subtree's entries move (same `node_id`s) into it; the original keeps `path` as a `repo` entry. The destination owns the referenced chunks and preserves original signed history through a recorded source parent. Nested splits retain the nested repository identity and pinned history. See [`split.md`](split.md). |
| `combine(other, at)` | one change with parents in both repositories (`change_ref` carries `repo_id`); path collisions become conflict values. |
| `operations()` / `undo(op_id)` | every mutation appends an `op` record to `ops` (`{ id, kind, from, to, at, by, session, inverse }`). `operations()` returns them newest first; `undo` applies the selected inverse as a new operation and returns applied and skipped paths. Undo never rewrites history. |
| `preview_edit(request, attribution)` / `apply_edit(preview)` | Preview records exact range-local replacements and file digests. Apply checks every input before writing one drip per file, then groups every entry effect into one attributed `structural_edit` operation so one undo restores the complete edit. |
| `undo_session(session, options)` | reverses the matching session newest first, optionally restricted to paths or drip IDs. Each touched directory is one wrapper transaction; candidates are published only after every transaction succeeds. |
| `lock(path, reason)` / `unlock(path)` / `locks()` | A configured path pattern permits acquisition; the repository stores the holder, time, reason, and advisory or lake authority. Writes by the holder continue. A write by another actor is refused with the complete lock details before a transaction opens. Signed offline claims reconcile through conflict values; an injected connected callback asks lake before acquisition or release. |
| `fork_session(options)` / `open_session(id)` | Fork the current repository document at a recorded change into `sessions/<id>` on the same store. Content chunks remain shared; only the private repository document, directory checkpoints, and journal are distinct. Session writes never enter the shared drip journal. |
| `session::publish(options)` / `discard()` | Diff the private head against its fork tree, validate every scope and the fork-point base, then persist the net result once with one signed `publish` drip per touched directory. Atomic publication restores all candidate documents on failure. Publish or discard closes the private state; discard also releases its leases. |
| `lease(claim, ttl)` / `release(claim)` / `leases()` | Persist file, symbol, or byte-range intent in `leases`. Symbol claims resolve through the recorded symbol ranges. Overlapping claims and writes by another actor/session return `errc::locked` naming the holder. Expired claims are removed on observation. |
| `report_presence(value)` / `presence()` | Maintain actor, session, path, optional range, activity, and timestamp in memory only. Values expire after 30 seconds and never enter checkpoints or journals. |
| `compact(epoch_options)` / `epochs()` | Compact every loaded directory checkpoint, record the epoch number, document heads, timestamp, and compact byte count, then retire pre-epoch operations and drips. A cursor older than the retained journal receives the checkpoint archive. |
| `history(path)` | walk snapshots newest first, discover the path's `node_id`, and follow that identity through moves; return values, deletions, actors, timestamps and messages. |
| `restore(path, change)` | read the historical blob and append an ordinary write drip at the requested path; no ref or earlier change is modified. |

### Operation records and inverses

The operation vocabulary is `write`, `mkdir`, `move`, `remove`, `set_ref`, `record`, `nest`,
`split`, `combine`, `lock`, `unlock`, `redact`, `merge`, `set_mode`, `structural_edit`, and `undo`. A cut and the
explicit `record` APIs use `record`, because their mutation is the reference-head transition.
`import_drips` records the remote batch as one `merge` operation; its range identifies the batch,
and imported operation records remain unchanged.

Each operation stores its own before and after values for affected entries, refs, locks, and the
working head. Its inverse is therefore derived from the operation's recorded before-state, never
from the newest operation in the document. A non-newest undo changes a value only when the current
value still equals that operation's recorded after-state. Otherwise `undo_report.skipped` names the
operation, path, and reason, preserving newer local or remote intent. The successfully applied
parts are appended as `undo` operations; neither source operations nor imported records are
rewritten. Redaction records the replacement transition, but purged secret bytes cannot be
restored and are reported as skipped.

Every open repository handle has a 16-byte session ID. `repository_options::session` supplies it;
otherwise the handle generates a random value. All mutations authored through that handle carry
the same ID. A speculative agent session has its own ID and delegation. Session undo plans the
whole selection before opening directory transactions, commits one transaction per touched
directory, and publishes no candidate if any transaction fails. Path filters select operations
touching that path or its descendants; drip filters select exact operation IDs.

The repository document stores `leases` beside `locks` and `epochs` beside refs and operations.
A lease key is `{path, kind, symbol?, range?}` and its value is `{by, session, since, expires,
authority}`. Offline authority is advisory. `repository_options::authorize_lease` is the
task-returning authoritative-peer hook: the complete proposed lease is sent there first, any
refusal leaves local state unchanged, and acceptance records `lake` authority. Lake-side policy is
owned by the corresponding lake work.

Session metadata contains the actor, complete delegation, fork tree, fork change, and fork cursor.
It lets the CLI reopen a session in another process without copying chunks. Publish renews live
leases owned by that actor/session. Discard releases them and removes the exact private state
directory.

When `min_ack_peers` is nonzero, `repository_options::acknowledge_epoch` receives the candidate
epoch and returns the count of known peers whose cursors cover its heads. An absent source or a
count below the requested minimum returns `errc::busy` before the epoch is recorded or history is
retired.

## Public tree comparison

Issue #166 adds `repository::diff` overloads for two changes, two tree-record digests, or two
already loaded `tree` values. The result is sorted by repository-relative path. Each
`tree_diff_entry` carries a vector of independent `tree_diff_kind` values, so one path can be both
moved and modified or both mode-changed and conflicted. `path` is the destination path when the
entry exists on the new side; a move also carries `old_path`.

Entries match by `node_id`, not by name. A delete and recreate at the same path is therefore one
removed entry and one added entry, while a rename of the same node is a move. Directory record
digests change whenever a descendant changes, but that does not report a modification for every
ancestor directory. Blob entries carry `old_blob` and `new_blob` when those sides exist. Modes are
also present per side, including on pure mode changes.

Change comparison attaches the exact before and after conflict-reference vectors stored in the
two change records. A conflict-only change produces its own `conflicted` entry even when the tree
record did not change. Tree comparison has no change record and therefore no conflict values.

Text work is opt-in through `tree_diff_options::text`. Without it, diff reads tree records but does
not materialise blob contents. With it, added, removed, and modified blobs are checked as UTF-8 and
receive `line_hunks`, using the existing `line_change` shape from build diagnostics. Binary values
leave `line_hunks` disengaged. The line algorithm returns separate hunks for separated edits; for inputs
whose line matrix would exceed 4,000,000 cells it returns the exact common-prefix/common-suffix
envelope as one bounded-memory hunk.

Container work is independently opt-in through `tree_diff_options::formats`. A recognised changed
blob receives a `container_diff` naming the codec and observed tier, then added, removed, and
modified logical paths with optional nested container diffs. Source work is independently opt-in
through `tree_diff_options::structural` and uses the supplied `frontend_graph`. The former reserved
`structural` field now carries the language, grammar-lock digest, formatting-only flag, and ordered
edits. Every entry labels its effective structural, text, or binary tier and explains a fallback.
See [structural-diff.md](structural-diff.md). Format details and limits are in
[formats.md](formats.md).

## Tree import

`repository::import_tree(root, import_options)` walks a local directory one directory at a time,
hashes every included regular file through the object store, and compares the resulting blob record
identity with the current working tree. Only added, removed, content-changed, or mode-changed
entries alter the tree. A non-empty import records one `import` drip whose entry list contains only
the changed paths; an unchanged tree returns no drip. `tree_import_report` returns that optional
shared `op_id` drip identifier plus `files_seen`, `changed`,
`bytes_hashed`, and `bytes_written`.

`import_options::ignore_patterns` supplements `.gitignore` and `.liquidignore` at the imported
root. Patterns use the repository glob rules; a leading slash anchors at that root and a trailing
slash selects the directory and its descendants. A `/**` rule also excludes that directory itself,
so the walk never opens its descendants. Rules are evaluated in order; a later `!` rule includes a
path again, including the directory selected by an earlier `/**` rule. `wrap` uses that form for the requested preconfigured build directory, which is an input
to the wrapped CMake build even when the repository ignores build directories. By default symbolic
links are skipped.

The paged walker represents a new directory with no prior page rather than a synthetic digest, so
adding a configured build directory does not attempt to load a nonexistent tree object.

Rules from the imported root are evaluated first and caller-supplied rules last, allowing a caller to
include one configured directory without weakening the root's other exclusions.
`follow_symlinks` follows them, and `message` is retained with the recorded operation. `liquid
init`, local `liquid import <path>`, and `liquid wrap` use this one primitive; Git history import
remains its separate historical-object path.

The repository regression covers an unchanged import, a one-file change with one changed tree
entry, a removal, both ignore sources, and a synthetic 1,024-file import under a 512 MiB RSS
ceiling on macOS and Linux.

The walk retains the directory stack and changed entries, never a collection of all discovered
paths. On a reopened repository it reads and replaces one stored tree page at a time, rather than
reconstructing the prior tree before comparing it. The imported drip participates in the 64 KiB
repository journal budget documented in [`build-engine.md`](build-engine.md); reaching that budget
checkpoints the already-paged tree. Thus importing is bounded by `O(depth + changed entries +
journal budget)`, rather than the size of either tree.

Recording a change after a paged import reuses an unloaded root page directly. It does not turn the
unloaded entry map into an empty tree, so configured inputs retained by a deferred open remain in
the recorded snapshot.

## Anchors

Issue #174 adds text positions that follow a file from one change to another without changing the
object model or introducing a text CRDT. A `text_anchor` stores a source `change_id`, path, line,
and column. Lines and columns are one-based. `repository::map_anchor` returns the corresponding
`mapped_anchor` at a target change; `map_anchors` accepts many anchors from the same source change
and preserves their input order.

Mapping is a projection over the line hunks from one `repository::diff` call with text enabled:

- An anchor before the first hunk or between hunks keeps its column and accumulates the line-count
  changes from earlier hunks. Insertions or removals above it therefore shift the line without
  reading the file again.
- An anchor inside a replacement selects the same relative line in the replacement, clamped to
  its last line, retains its column, and sets `approximate` because line hunks do not contain a
  character-level correspondence.
- An anchor inside a hunk with no replacement lines sets `deleted`, reports the target insertion
  line with column one, and cannot be treated as an exact position. Removing the complete file
  also sets `deleted`; its source path and coordinates remain available for diagnostics.
- A rename or move follows the tree entry's `node_id`, changes the returned path, and sets `moved`.
  `moved` also reports a changed line number; `approximate` is independent, so a same-number
  replacement can be approximate without being moved.
- A content-changing entry whose blobs are not UTF-8 text returns `errc::unsupported`. A batch
  containing different source changes or a zero line/column returns `errc::invalid_argument`.

The batch groups anchors by source path, scans the diff entries once, sorts only the anchors for
each matched path, and advances through that path's hunks once. Its mapping cost is proportional to
the produced diff and anchors, not the file length or one full diff per anchor. The diff algorithm
remains responsible for materialising changed text and producing hunks.

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
  `store.get` the digest, and open either checkpoint encoding as a full document. A directory document is saved (and `dirs` updated in one
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

### automerge-cpp adoption measurement

The issue #136 comparison used a Release build on an Apple M3 Max with 64 GiB of memory, macOS
14.6.1, and Homebrew Clang 22.1.8. `liquid-repository-open-measure prepare` created 100 directories
and distributed 10,000 writes across them with the default checkpoint interval of 64. The final
partial interval remained in `LIQUID_JOURNAL`. Each row is the median of three fresh-process
`open` runs over independent copies of that prepared repository; peak RSS therefore excludes the
preparation process.

| automerge-cpp | Open samples | Peak-RSS samples | Median open | Median peak RSS |
|---|---|---|---:|---:|
| Before: `4dd7955` (`v0.6.0`) | 563.955, 567.660, 586.194 ms | 257.000, 260.906, 275.375 MiB | 567.660 ms | 260.906 MiB |
| After: `06408d3` | 525.087, 472.571, 473.059 ms | 263.516, 262.562, 262.484 MiB | 473.059 ms | 262.562 MiB |

The measured open became 16.7% faster. Peak RSS was effectively unchanged (+1.656 MiB, +0.6%).
Directory documents remain lazy, so this measures repository-document recovery, the final journal
interval, checkpoint publication, and store setup. Those measurements record the shallow-loading
implementation used by liquid #136; liquid #195 instead validates the bounded drive workload with
sync-capable active directory documents.

Reproduce the workload with:

```sh
cmake -S . -B build-measure -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-measure --target liquid-repository-open-measure
build-measure/tests/liquid-repository-open-measure prepare build-measure/repository 100 10000
cp -R build-measure/repository build-measure/open-1
build-measure/tests/liquid-repository-open-measure open build-measure/open-1
```

## Implementation notes

`LIQUID_REPO` contains the digest text of the latest repository checkpoint. Its replacement is
written through Corosio to a sibling temporary file and atomically renamed. A current checkpoint
stores the root as a tree-page digest rather than embedding the recursive working tree in the
repository document. Nested repository entries retain their local child index because their
objects live in a separate repository store; directory pages remain paged. `LIQUID_JOURNAL` is a
fixed-size pointer to a paged checkpoint; signed drip bytes are bounded to 64 KiB before another
checkpoint is published. Each loaded directory is a private CRDT document cached by its `node_id`;
saving a checkpoint prefers `save_compact()` bytes, verifies that they reopen as a full document,
and falls back to complete upstream history bytes when needed. The checked bytes refresh the
`dirs` entry and replace the active instance. Journal publication persists only cached directories;
a checkpoint synchronizes reachable directories before publication. A checkpoint occurs at `cut`
and after the configured interval (64 by default). Session undo follows the same bounded journal
publication path rather than serializing a repository document directly. The checkpoint digest and
heads are published before journal entries
covered by those heads are retired. On open, each directory document is loaded sync-capable and
restored lazily against the journaled working tree before its next mutation. Matching entries are
left intact, so recovery is idempotent across process restarts.

The actor derivation's device input is the repository checkout path. Two clones of the same
repository therefore author under different automerge actors and may create valid concurrent
sequences; reopening the same checkout keeps its actor stable.

Compact checkpoint bytes can reconstruct a full document with `load()`; states that cannot do so
are persisted directly as complete history bytes. Active documents therefore accept synchronization
dependencies and historical queries through one checked load path. Retiring older stored archives
is valid only after no supported peer or historical query can name those heads.

Checkpoints written before this dependency bump do not load because the pre-#15 private change
encoding and hashes are intentionally incompatible. No repository data exists in that format. If
one did, recreate each directory document from its retained journal and the canonical repository,
tree, and blob records, then cut a new compact checkpoint with this version. Do not reinterpret an
old hash as a new one.

The wrapper contains no text mutation or text-position conversion today. Its index-bearing code is
limited to list operation records and passes element indices unchanged. Future text support must
pass Unicode scalar-value positions and counts to `splice_text`, cursors, marks, and patch handling;
UTF-8 byte offsets are not valid automerge text indices.

File content is represented by a canonical `blob` record whose parallel chunk digest and size
lists describe FastCDC logical chunks. The size list lets VFS range reads locate an offset without
loading earlier chunks. A version 2 remote blob also records the whole-content digest and source
location so a chunk range can be fetched and independently checked. The decoder remains compatible
with the earlier single-chunk record by deriving its one size from the blob total. Trees and changes are canonical records as well; the
change digest is the digest of its canonical bytes and is never an automerge hash. The fixture in
`tests/fixtures/records/empty-tree.hex` pins the canonical empty-tree encoding.

## How to verify

- Canonical encoding: supported RFC 8949 Appendix A values and bytewise map-key ordering are
  checked against `tests/fixtures/cbor/rfc8949-supported.txt`.
- Identity: create, move, move back; the entry's `node_id` never changes; the log shows two
  moves and no delete/create pairs.
- Recovery: a cross-directory move survives a process restart before the next cut; replayed
  journal data restores the destination before the next snapshot is cut.
- Lazy loading: a three-level tree reloads only the directory containing an updated entry;
  the directory-cache load counter is asserted by the repository test.
- Undo: undoing a recorded move restores the old path without minting a new `node_id`.
- Composition: `split` then `nest` the result back; `root()` is byte-identical to before;
  `log` of the nested repository shows the pre-split history.
- Conflict: create a repository, write and cut `a.txt`, clone it twice, write different values in
  each clone, and exchange directory-document sync frames both ways. Each working copy keeps its
  CRDT-selected entry and reports one `a.txt` conflict value containing the base, left and right
  references. The next cut carries that value, repository reopen preserves it, and an ordinary
  resolution write clears it and creates a version-3 resolution reference. Push the unresolved
  change through a sync server, pull it into a fresh repository, and assert that both the conflict
  and an unrelated file remain available. See [`conflicts.md`](conflicts.md).
- Path history: write two versions, move and delete the path, then query by the moved name. Restore
  the first change and assert that it adds one pending drip while the prior log head is unchanged.

Text synchronization, repository composition, drive reconciliation, and CRDT integrations use the
same content-addressed conflict value. The selected entry remains readable from the local directory
document while callers prepare and record a resolution.

## How to change

A new record kind is a row in the records block, a `kind` string, a version, and a fixture. A
new document key is a line in the root map and a rule for its merge semantics. Anything that
changes an existing record's canonical form bumps `v` and keeps the old decoder.
