# First-class conflict values

**What.** Liquid records an unresolved merge as an immutable, content-addressed object instead of
placing markers in file bytes or keeping process-local merge state. Text, structural, and
suspicious CRDT producers all emit the same `conflict/1` record. Repository status, changes,
sync, drive reconciliation, the CLI, and build input materialisation consume that record.

**Why.** A conflict can survive a cut, push, clone, and process restart only when it is part of the
ordinary object graph. Keeping the selected working value separately lets users inspect unrelated
paths and prepare a resolution without losing the base or either competing value.

**Status: implemented for liquid #40 and structural merge in #69.** Multiplayer leasing remains
#30.

## Record

`conflict/1` is canonical CBOR and therefore has an ordinary `digest`:

```text
{
  v: 1,
  kind: "conflict",
  base: digest,
  left: digest,
  right: digest,
  region: {
    path: text,
    bytes: { offset: uint, length: uint }
      | nodes: { first: node_id, last: node_id }
  },
  merge_kind: "text" | "structural" | "crdt-suspicious",
  explanation: text,
  kind?: "entry" | "suspicious-merge",
  proposal?: digest,
  reasons?: [text ...],
  node?: node_id
}
```

The path and explanation are non-empty. The path is repository-relative. A record contains exactly
one range form. Text
producers use a byte range over the competing blob contents. Structural and CRDT producers may use
stable node identifiers when the affected value is a tree region rather than a byte interval.
`base`, `left`, and `right` are collector references, so every named input stays reachable while
the conflict is reachable. A suspicious merge also retains its proposal. The optional fields keep
old records decodable as entry conflicts whose single reason is `explanation`. The decoded public
value also carries its computed `id`; that field is not serialized into the record it names.

## Trees, changes, and working state

A tree entry at an unresolved path has `kind: conflict` and uses the conflict digest for both its
object identifier and reference. Other entries remain ordinary blobs, trees, or nested repository
values. A build or materialisation that walks into the conflict entry returns `errc::conflict`
with the repository path and conflict digest. A tree that does not contain that entry materialises
normally.

The shared-library test target calls the same materialisation function as the static target, so
the conflict refusal is verified for both library link modes.

The live repository retains the CRDT-selected blob, tree, or nested repository entry alongside the
conflict reference. Reads and edits use that selected entry. Checkpoints persist enough selection
metadata to reconstruct the working tree after reopen while subsequent cuts continue to publish a
conflict tree entry.
If a converged CRDT directory omits a conflicted file path, and when a received change restores a
paged conflict tree entry, file reads use that persisted selected blob until the conflict is
resolved. Materialisation still rejects the conflict tree entry, so selecting a working value does
not hide an unresolved conflict from a build.
Path history follows the stable node retained by a conflict tree entry. This keeps a drive path and
its deterministic alternate name independently visible in history while the conflicting path names
the conflict value for its unresolved revision.

Change record version 3 adds two canonical lists:

```text
conflicts:   [ { path: text, id: digest(conflict) } ... ]
resolutions: [ { conflict: digest(conflict), value: digest } ... ]
```

Versions 1 and 2 retain their legacy inline reference decoder. New changes always use version 3.
The store reference registry follows conflict records from trees and changes and follows all three
input values from each conflict record. Push and pull therefore transfer the complete graph.
Importing the `main` change rebuilds the unresolved index against the destination's converged
working entry, which makes a fresh pull behave as a clone with unresolved conflicts intact.

## Producers

All producers construct `liquid::conflict` and pass it to `repository::record_conflict`:

- the synchronized text path computes the maximal common prefix and suffix and records the
  remaining byte interval as `text`;
- repository composition records differing entries and their stable nodes as `structural`;
- structural merge at CRDT convergence records entry conflicts for overlapping edits or text files
  without grammars and `suspicious-merge` values for results that require attention;
- other CRDT integrations may continue recording `crdt-suspicious` as their merge kind.

Concurrent lock reconciliation emits `crdt-suspicious` with the competing lock records as its
three values. A lock-only path has no tree entry, so the value remains visible in status and sync
without blocking an unrelated build.

Drive reconciliation also stores its content and delete/edit collisions through this record type.
It may continue showing a deterministic alternate filename for the competing value, but status and
retention use the conflict object rather than a drive-only representation.

## Inspection and resolution

`liquid status` prints one `conflict=<path> <digest>` line for every unresolved path. Its JSON form
uses `"conflicts":[{"path":...,"id":...}]`. List or inspect full values with:

```sh
liquid conflicts --json
liquid conflict show path/to/file
```

The command prints the conflict digest, path, merge kind, base, left, right, range, and explanation.
To choose content, first write or edit the path so its selected working entry is the intended
result, then run:

```sh
liquid conflict resolve path/to/file
```

Resolution removes the path from the unresolved index and immediately cuts an ordinary version-3
change. Its `resolutions` entry binds the prior conflict digest to the selected value digest. The
new tree contains the selected value, and the old conflict remains reachable through history.
Structural conflicts can instead be resolved by digest:

```sh
liquid resolve <conflict-id> --accept
liquid resolve <conflict-id> --left
liquid resolve <conflict-id> --right
liquid resolve <conflict-id> --from prepared-file
```

`--accept` is valid only when the conflict carries a proposal.

## Verification

- `Records.conflict_records_are_canonical_content_addressed_values` pins canonical round-trip and
  digest behavior; the adjacent invalid-region case checks the exclusive range rule.
- Repository synchronization tests exercise text conflicts, reopen, conflict tree entries, and a
  resolution change that references both the conflict and selected value.
- Repository composition exercises the structural producer; `cli-conflict` exercises the
  CRDT-suspicious producer, status, show, and resolve.
- `Sync.push_and_pull_clone_preserve_unresolved_conflicts` verifies graph transfer and an unrelated
  readable file.
- `BuildConflicts.only_a_reached_conflicted_path_stops_materialisation` verifies the build boundary.

## Compatibility and limits

Old change records with inline conflict references remain decodable but cannot be inspected as a
standalone conflict object; they report that a new merge is required. Multiplayer lease policy is
outside this record and resolution model.
