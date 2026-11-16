# Version control

> Evidence: [`sources.md`](sources.md)

## The incumbent: Git's object model

Git is four object types in a content-addressed store — blob, tree, commit, tag —
assembled into a DAG, with packfiles applying delta compression for storage. This
model has three properties worth keeping and one that causes most of the pain.

**Worth keeping.** Content addressing means identical content is stored once, and
an object's name *is* its integrity check. The commit DAG is a genuinely good
model of "history is a partial order, not a line." Distribution is a consequence
of content addressing rather than a bolted-on feature: any two clones can
reconcile because names are global.

**The problem: Git versions *snapshots*, not *changes*.** A commit records a whole
tree. The "change" a commit represents is computed after the fact by diffing
against a parent. This is why:

- Rename detection is a **heuristic run at diff time**, not recorded fact.
- Merges are three-way diffs over reconstructed changes, so the same logical
  change reaching a branch by two routes conflicts with itself.
- Cherry-pick and rebase produce *new* commits with no recorded identity link to
  the original, which is why the same fix applied to two branches then merged is a
  classic conflict.
- History rewriting is the only tool for many operations, and it is destructive
  and coordination-hostile.

## The specific limitation: you cannot merge two codebases

This is a real, structural limitation, not a UX gap. `git merge` requires a common
ancestor. Two independently developed repositories have none, so Git refuses
outright unless told `--allow-unrelated-histories`, and even then it produces a
merge with two roots and no meaningful content reconciliation — every file is
"added by them", and any path colliding in both trees is a conflict with no base.

The existing workarounds are all bad:

| Approach | What actually happens |
|---|---|
| `git subtree` | Grafts a subtree with a synthesised merge. History is preserved but the subtree is second-class: no independent identity, awkward to push changes back, `--squash` throws away history entirely. |
| Submodules | Not a merge at all. A pointer to a commit in another repo, with a separate clone step, detached HEADs, and the notorious "forgot to update the submodule" failure mode. Widely hated. |
| `git filter-repo` | Rewrites history to relocate paths, then merge. Destroys all existing object IDs, so every clone and every reference to a commit breaks. |
| Monorepo migration tools | Bespoke, one-shot, and lossy. |

**The root cause:** Git has no concept of a *repository* as a composable value. A
repository is a bag of refs over a shared object store, and "combining" two of
them is not an operation the model supports. Any system that wants first-class
composition must make the unit of composition explicit from the start.

## The alternatives and what each proves

**Darcs — patch theory.** Models a repository as an unordered *set* of patches
with a commutation relation: if two patches are independent, they can be
reordered. This makes cherry-picking principled — moving a patch between branches
preserves its identity — and makes many merges disappear entirely, because the
same patch arriving twice *is the same patch*. The fatal flaw was performance:
merge could hit exponential blowup ("the exponential merge problem") because
resolving conflicts required exploring permutations.

**Pijul — patch theory that scales.** Reformulates Darcs on a sound
category-theoretic footing: a repository state is a pushout in a category of
patches, and the representation is a graph of file *lines* with vertices that can
be alive or dead. Conflicts become explicit first-class states in the data
structure rather than markers in a file. Merge is associative and commutative —
the order you merge in cannot change the result, which Git cannot promise.
Pijul's practical problem is ecosystem, not theory.

**Jujutsu (jj) — the pragmatic win.** Keeps Git's storage (it can use a Git
backend and interoperate) while fixing the working-copy and operation model:

- **The working copy is a commit.** There is no staging area to reason about; edits
  are automatically amended into a real commit.
- **First-class conflicts.** A conflicted merge is a committable state, not a
  broken working directory. You can merge, commit the conflict, and resolve later
  — or resolve it in a different order.
- **The operation log.** Every repository mutation is recorded, so *any* operation
  is undoable, including ones Git makes unrecoverable.

jj is the strongest evidence that Git's *storage* is not the problem — its
*operation model* is. That is an important signal for liquid: the content-addressed
DAG is worth keeping; the surrounding semantics are not.

**Sapling / EdenFS (Meta).** Attacks scale rather than semantics: a virtual
filesystem materialises files lazily, so a checkout of a massive monorepo is O(1),
and Watchman tells the tools what changed without scanning. Proves that *the
working copy should be virtual* at scale.

**Piper/CitC (Google), Perforce.** Centralised, workspace-based, and still the
right answer for very large binary-heavy trees — see
[02-large-files-and-binary-assets](../02-large-files-and-binary-assets/).

## Structural and semantic diff

Line diffing is the wrong primitive for source code. The 2024–2026 state of the
art:

- **difftastic / diffsitter** parse both sides with tree-sitter and diff the ASTs,
  so reindentation, brace movement and wrapping produce empty diffs.
- **Mergiraf** does structured *merge*, resolving conflicts that are textual but
  not semantic (two functions added to the same file in different branches).
- **GumTree** is the research baseline for AST edit scripts.

The catch is that structural diff needs a grammar per language, and the fallback
must be graceful. It also interacts with liquid's ambition to diff inside binary
containers — see [03-structural-diff-and-formats](../03-structural-diff-and-formats/).

**Unison** goes furthest: code is stored by the hash of its *dependency-closed
AST*. A definition's name is metadata pointing at a hash. Consequences: renames
are free and never conflict; there is no build step because everything is already
compiled and cached by hash; dependency conflicts vanish because two versions of a
library are just two different hashes that coexist. The cost is that it requires
its own language and a codebase manager rather than files, which is why nobody has
adopted the idea outside Unison — and why it is such a tempting model for a system
that is already redesigning storage.

## What people love and hate

**Love:** offline-first and fast local operations; branching that is genuinely
cheap; the DAG as a shared mental model; `git bisect`; the fact that content
addressing makes corruption detectable; and GitHub's network effect.

**Hate, consistently, across HN/Reddit/surveys:**

1. **Submodules.** Near-universal loathing. The most-cited single pain point.
2. **The staging area** as a concept that must be taught before anything works.
3. **Merge conflicts** that are textual rather than semantic, and the same conflict
   recurring on every rebase (`rerere` exists precisely because this is so bad).
4. **History rewriting** as the answer to too many questions, with force-push as
   the coordination hazard it implies.
5. **Large files.** Git is actively bad at them and LFS is a poor patch.
6. **The CLI.** Inconsistent verbs, overloaded `checkout`, confusing reset modes.
7. **Monorepo scale** — clone times, `git status` latency, repack memory.
8. **No way to compose repositories**, as above.

## Implications for liquid

1. **Keep content addressing; discard snapshot-only semantics.** Store a Merkle
   DAG, but record *change identity* explicitly so cherry-pick, rebase and
   duplicate-arrival merges are principled rather than heuristic. Pijul shows this
   is achievable without Darcs' blowup; jj shows it can be done while remaining
   interoperable.

2. **Make repository composition a first-class operation.** This is the headline
   differentiator. A repository must be a *value* that can be embedded in another
   repository, split out of one, and merged with an unrelated one, with history
   preserved and object identity intact on both sides. Both the "submodule-like
   embedding" requirement and the "merge two codebases" requirement are the same
   underlying feature: **a repository must nest**. Design the object model so that
   a subtree can carry its own provenance and independently addressable history,
   rather than adding a submodule mechanism later.

3. **First-class conflicts and an operation log.** Take these directly from jj.
   A conflict is a value; every operation is undoable. This removes most of the
   fear that drives Git's worst workflows.

4. **Record renames and moves; never re-derive them.** liquid controls the client,
   so it can observe the move at the moment it happens.

5. **The working copy should be virtual by default.** Sapling/EdenFS proves this is
   how large trees stay usable. It also composes with liquid's lazy artifact
   fetching — the same materialisation machinery serves both.

6. **Structural diff and merge as the default, text as the fallback.** Since liquid
   is already parsing source for incremental compilation
   ([08](../08-incremental-compilation/)), the AST is available. Reuse it: the
   same tree that drives fine-grained rebuilds should drive diff and merge.

7. **Do not fight the network effect head-on.** jj's Git backend is why anyone
   tried it. liquid needs an import/export path that is lossless enough that
   adoption is not a one-way door.
