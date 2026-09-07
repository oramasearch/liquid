# Structural merge

Liquid issue #69 adds a structural three-way merge after per-directory CRDT convergence. The CRDT
still selects one whole-file blob for a `node_id`; liquid inspects the base, local, and received
blob references before accepting that selection as the working value.

## Algorithm

The repository finds the closest common change reachable from the local and received `main`
references. A file is a structural-merge candidate when the same `node_id` exists at the base and
both sides, both sides changed its blob reference, and the resulting references differ. With no
common change, liquid keeps the entry-level conflict behaviour.

For a candidate with a registered grammar, the frontend graph parses all three blobs. Definition
units are matched by stable name and fingerprint. One-sided replacements and removals become base
range edits. New definitions are anchored before or after definitions shared with the base. Edits
to distinct definition ranges combine; overlapping or adjacent ranges produce a structural
conflict. The proposal is parsed again before it can replace the CRDT-selected entry.

Files without a grammar retain an entry-level conflict of kind `text`, and their reason says that
text has no grammar. A grammar without a pinned definition-unit query uses the same explicit text
fallback instead of aborting convergence. Merge parsing and edit-script composition run in the
graph or through `runtime::offload`; the repository does not create worker threads or synchronously
wait inside a coroutine.

Every successful convergence records a `merge` operation. Its metadata contains
`algorithm=structural-three-way-v1`, the grammar digest, path, and the base, left, and right blob
references. Further files use numbered metadata prefixes.

## Suspicious proposals

A proposal is stored as a conflict of kind `suspicious_merge`, rather than installed as the file,
when any of these checks fires:

- the proposal parse contains an `ERROR` or `MISSING` node;
- opposite sides introduce a definition with the same name;
- both sides edit or remove the same definition; or
- one side still references a syntactic symbol removed by the other.

The conflict record carries the proposal blob, every reason, and the named definition or file node.
Ordinary structural overlap has no proposal. The CRDT-selected blob remains readable until an
explicit resolution is committed.

## Resolution and transfer

`repository::resolve_conflict(id, resolution)` accepts `accept_proposal`, `left`, `right`, or caller
bytes. It writes the selected blob, removes the unresolved conflict, and cuts a change containing a
resolution binding. `liquid conflicts [--json]` lists identifiers, kinds, paths, reasons, and nodes.
`liquid resolve <id> --accept|--left|--right|--from <file>` applies the matching resolution.

Conflict records remain version 1. `proposal`, `reasons`, and `node` are optional on decode, so
existing records continue to load. The ordinary object-reachability and sync paths carry proposal
blobs and new conflict kinds.

## Merge-quality measurement

`liquid-merge-measure` imports Git history and classifies merge commits as clean and identical,
clean but different, conflict raised, or suspicious raised. Git revisions are matched by path
because Git import creates fresh CRDT node identifiers for historical snapshots. The classifier
reconstructs every changed path across base, both parents, and the human result, including
one-sided changes and merge-only edits. A populated state directory resumes from its completed
import. The suite runs a fresh import against the committed fixture history. The public-corpus
table records the exact repository and commit range used for the issue #69 acceptance run.

| Repository | License | Commit range | Merges | Clean identical | Clean different | Conflict | Suspicious | Load average |
|---|---|---|---:|---:|---:|---:|---:|---:|
| committed fixture | repository fixture | `afa637a3c249..5e507d146aa8` | 1 | 1 | 0 | 0 | 0 | 3.55713 |
| [fmt](https://github.com/fmtlib/fmt) | MIT | `ec27d5d8a168..2d4d8a13c2b4` | 166 | 131 | 1 | 8 | 26 | 4.89453 |
| [CLI11](https://github.com/CLIUtils/CLI11) | BSD-3-Clause | `2db15139cb34..c1cfe00d2f3d` | 20 | 13 | 1 | 5 | 1 | 4.6626 |
| [spdlog](https://github.com/gabime/spdlog) | MIT | `681e40ce46c5..57cb5fb7a8ff` | 582 | 514 | 6 | 21 | 41 | 3.77979 |

The dangerous cell is clean but different: a result accepted without review that does not match
the human merge. Corpus runs keep that count visible even when every other category is expected.
