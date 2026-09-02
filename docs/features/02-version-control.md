# Version control

> Research: [01-version-control](../research/01-version-control/)

## What this is built on

The history, namespace and change graph live in the CRDT datastore
([`automerge-cpp`](https://github.com/homer6/automerge-cpp), see
[17](17-implementation.md)). Several features below are properties liquid gets from
that substrate rather than mechanisms it must invent: change identity (`ChangeHash`,
`ActorId`, `OpId`), stable object identity across moves, time travel, an operation
history, and a peer-to-peer sync protocol.

File *content* lives in the chunk store ([01](01-object-model-and-storage.md)).

## What liquid keeps from Git

The content-addressed DAG, cheap branching, offline-first operation, and the idea
that history is a partial order. These are correct and liquid does not deviate
from them.

## What liquid changes

### Changes have identity

Git records snapshots and computes changes by diffing afterwards. liquid records
the **change** as a first-class object with a stable identity (`ChangeHash`) that
survives rebasing, cherry-picking and transplanting between branches.

Consequences:

- A change applied to two branches and then merged is recognised as *the same
  change*, not a conflict. This is the single most common avoidable merge conflict
  in Git.
- Cherry-pick and rebase preserve provenance instead of fabricating new commits
  with no link to the original.
- "Has this fix shipped?" is answerable by identity rather than by diffing.

### Renames and moves are recorded, never inferred

liquid owns the client and observes the move when it happens; the datastore gives
every path a stable `ObjId` that survives it. Rename detection is not a diff-time
heuristic with a similarity threshold. This is the same deficiency whose absence
forced Dropbox's sync-engine rewrite ([research](../research/21-consumer-sync-and-web/)).

### Conflicts are values

A conflict is a committable state, not a broken working directory. You can merge,
record the conflict, push it, and resolve it later — or let someone else resolve
it. Taken directly from Jujutsu, which demonstrated it works.

### Every operation is undoable

The datastore's change history records every mutation of the repository, and its
time travel (`get_at()`) reads any prior state. `liquid undo` reverses any
operation, including ones Git makes unrecoverable. This removes the
fear that drives the worst Git workflows.

### The working copy is a change

No staging area. Edits are automatically recorded into the current change. The
staging area is the concept that must be taught before anything else works, and it
buys less than it costs.

### Every write is a drip

liquid versions at the granularity of the **write**, not the commit.

Every save to disk is captured as a **drip** — the atom of history. Drips are
automatic, continuous and never explicit. A **change** is a named, reviewable
aggregation of drips.

| | Captured | Named | Reviewed |
|---|---|---|---|
| **Drip** | Automatically, on every write | No | No |
| **Change** | From drips | Yes | Yes |

This is why history is complete rather than curated: nothing is lost between saves,
`liquid undo` reaches any prior state, and the build invalidates the minimum work per
drip ([04](04-build-engine.md)) instead of per commit.

Three consequences the design must handle, covered in [21](21-liquify-ide.md): build
outputs and dependency directories are excluded by declared rules; drips compact once
their change is named; and **secret scanning runs on the drip, not on the commit** —
because a credential pasted and deleted seconds later was still written to disk.

## Repository composition

**This is the headline feature.** A repository is a *value* that can be nested,
split and combined. Both "first-class submodules" and "merge two codebases" are
the same underlying capability.

### Nesting

```
liquid nest <source-repo> <path>
```

Embeds a repository at a path inside another. The nested repository:

- keeps its **own history and its own object identity**;
- is visible as ordinary files in the working tree, with no separate clone step and
  no detached HEAD;
- can be updated in place, with changes attributed to the correct repository;
- can be pushed back to its origin without a separate workflow.

There is no `.gitmodules`, no "forgot to update the submodule", and no second
command to run after checkout.

### Splitting

```
liquid split <path> --into <new-repo>
```

Extracts a subtree into an independent repository, **preserving the history of that
subtree and the object identities within it**. The original repository can retain
it as a nested repository, so the split need not be a divorce.

### Merging unrelated repositories

```
liquid combine <repo-a> <repo-b> [--at <path>]
```

Merges two codebases with no common ancestor. Both histories are preserved and
remain traversable. Path collisions are surfaced as ordinary conflicts — which are
values, so they can be recorded and resolved incrementally.

This is impossible in Git by construction: `git merge` requires a common ancestor,
and `--allow-unrelated-histories` produces a merge where every file is "added by
them" with no base for reconciliation. Making a repository a composable value is
what makes this work.

## Structural merge

Merge operates on structure, not lines. Two functions added to the same region of a
file by two branches are not a conflict. See [03](03-diff-and-merge.md).

## Large files and binaries

No special handling required — see [01](01-object-model-and-storage.md). For
unmergeable binary assets, liquid supports **exclusive checkout**:

```
liquid lock <path>
liquid unlock <path>
```

Declared per path pattern in repository configuration. A lock is a coordinated
claim against the designated authority for that path. This is Perforce's model, and
it is the correct concurrency model for content that cannot be merged. Without it,
liquid is unusable for art and hardware teams.

## Interoperability

liquid imports from and exports to Git losslessly enough that adoption is not a
one-way door. Jujutsu's Git backend is the reason anyone tried Jujutsu; liquid needs
the same on-ramp.

- `liquid import git <url>` — full history, preserving commit identity mapping.
- `liquid export git` — produces a Git repository.
- A liquid repository can be used with a Git remote for teams mid-migration.

Nested repositories and recorded change identity have no Git equivalent; export
degrades them to subtrees and ordinary commits, and says so.

## Open questions

- **The merge algorithm.** Pijul's patch-commutation model is the most principled
  and the hardest to implement; a change-identity-aware three-way merge is more
  tractable. Not yet decided.
- **Nested repository push semantics** when a change spans the boundary between a
  parent and a nested repository — atomic across both, or two operations?
- **Distributed lock authority.** Locking requires a coordination point, which sits
  awkwardly with offline-first operation. The likely answer is that locks are
  advisory offline and authoritative when connected, but this needs specification.
