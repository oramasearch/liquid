# Multiplayer and agents

> Research: [25-multiplayer-and-agent-collaboration](../research/25-multiplayer-and-agent-collaboration/) ·
> [24-editors-and-ide-architecture](../research/24-editors-and-ide-architecture/) ·
> [21-consumer-sync-and-web](../research/21-consumer-sync-and-web/)

Many people — and many AI agents — changing the same things at the same time, fluidly.

## Where this sits: novel, and unproven

Stated plainly, because it is the first thing a reviewer will ask.

**No mainstream coding tool does human-agent CRDT co-editing.** As of September 2026,
Claude Code, Cursor, Codex, Devin and GitHub Copilot all handle concurrency by
**isolation** — a separate git worktree, branch, container or cloud VM per agent — and
reviewable diffs. Working demonstrations of the CRDT approach exist (Electric's
agent-as-Yjs-peer, AgentRoom, CodeCRDT, ODocs) and it is starting to be productised,
but there is no widely deployed example. Google Docs with Gemini stages *suggestions*
for accept/reject; it does not expose the model as an autonomous peer.

So liquid's position is genuinely differentiated and genuinely unproven. Both halves
matter.

**But isolation does not solve the problem — it defers it.** The research is direct
about this: isolation prevents silent filesystem overwrites, and overlapping work then
reappears as merge conflicts or, *more dangerously, as cleanly merged but semantically
inconsistent code.* Two agents independently changing an API contract and its callers
merges cleanly in git and is wrong.

That reframes liquid's risk. The semantic-inconsistency problem **already exists** in
every worktree-isolated agent workflow today. liquid does not introduce it. What liquid
must do is **detect it**, which git cannot.

And the current isolation model leaks in ways worth knowing:

- **Claude Code** agent-team teammates share the parent working tree unless deliberately
  isolated — so stale reads, whole-file rewrites and `git add -A` can overwrite another
  actor's edits. Anthropic recommends partitioning files precisely because teammates are
  not worktree-isolated.
- **Cursor** documented 2025–2026 bugs including incomplete applies, deleted files not
  applying, and worktree sessions jumping back to the main checkout.
- **Git isolation does not isolate data.** Every worktree still points at the same
  `DATABASE_URL`, so one agent's migration or truncate hits everyone. Isolation must
  extend to data namespaces, not just files.

## Multiplayer is a property of the store, not a feature of the editor

Because the datastore is a CRDT ([17](17-implementation.md)) and every write is a drip
([22](22-change-capture.md)), concurrent editing is not something liquify implements.
It is what the storage layer already does, and it therefore applies **everywhere**:

- source code in the editor ([21](21-liquify-ide.md));
- documents and spreadsheets in the Drive surface ([15](15-consumer-sync-and-web.md));
- datasets, notebooks and model configs ([18](18-lake-service.md));
- the namespace itself — two people reorganising folders concurrently.

There is no separate collaboration server, no second synchronisation protocol, and no
distinction between "collaborative documents" and "everything else."

## Humans and agents are the same kind of participant

Every writer is an **actor** with an identity — an `ActorId` in the datastore, bound to
a principal in the identity layer ([19](19-identity.md)). A person, a laptop, a CI
runner and an agent are all actors. Merge, attribution, history and undo work
identically for all of them.

That symmetry is what makes fluid human-agent collaboration possible at all: an agent's
edit is not a special event requiring a lock, a branch or a handoff. It is a drip,
alongside everyone else's.

## What agents need that humans do not

Symmetry in the data model does **not** mean identical treatment operationally. Agents
differ from humans in ways that matter:

| | Humans | Agents |
|---|---|---|
| **Rate** | A few writes per minute | Thousands per second |
| **Breadth** | One or two files | Whole trees |
| **Authority** | Their own | Should be **less** than the dispatcher's |
| **Error mode** | Occasional, local | Fast, wide, confidently wrong |
| **Reversal unit** | A file, a change | An entire session |

Five consequences, each of which liquid must implement rather than assume:

### 1. The private speculative replica

**Never feed a model's token stream into the live document.** This is the central design
recommendation from the research, and the pattern Electric's 2026 demonstration uses.

An agent gets a **private replica** — a snapshot it can generate against, revise, and
discard freely. Only the accepted result becomes **one bounded update** to the shared
document.

This solves several problems at once: half-written syntax never becomes visible to
collaborators; token streaming collapses into a coherent transaction; and the
tombstone churn of generate-then-rewrite loops stays private rather than entering shared
history.

Supporting controls, all of which liquid needs:

| Control | Behaviour |
|---|---|
| **Batch and coalesce** | Group adjacent inserts and deletes into transactions; merge updates before publication. |
| **Flow control** | Bound unacknowledged operations per peer; pause generation when persistence or rendering lag crosses a watermark. |
| **Rate limits** | Token buckets per agent, per document, per tenant, plus maximum update size. **Must pause or reject explicitly** — silently dropping CRDT operations breaks causal dependencies. |
| **Agent-aware rendering** | Throttle highlighting, diagnostics and tests independently from ingestion. Show *"agent changing lines 80–140"*, not an animation of 10,000 keystrokes. |

**Avoid whole-file rewrites.** Replacing an entire file creates enormous churn and
tombstones compared with a range- or AST-level edit, and destroys authorship
granularity. liquid's structural layer ([03](03-diff-and-merge.md)) should let agents
express edits at the node level.

### 2. Attribution, recorded not inferred

Today's conventions are commit trailers — `Co-Authored-By: Claude`, `Made-with: Cursor`,
`Co-authored-by: Codex` (added February 2026), `Generated with Devin`. All are
**tool-level markers only**: they identify a vendor label and not the session, the
prompt, the tool calls, which files were generated versus merely edited, or how much a
human revised afterwards. `Co-Authored-By` is also contested on its own terms — it was
designed for human joint authorship, and GitHub renders the named identity as a person.

liquid records the thing itself. Every drip carries its actor; for an agent that means
**which agent, running which model, dispatched by which principal, under which
authority** — as a signed attestation, the same mechanism as build provenance
([10](10-supply-chain.md)).

"Who wrote this line, and were they a person?" is answerable years later because it is a
*record*, not a trailer someone can forget or disable.

### 3. Scoped authority

An agent holds **strictly less** authority than the principal who dispatched it —
workload identity, not a copy of the human's credentials ([19](19-identity.md)).
Path-scoped write permission, no access to secrets it was not granted
([11](11-secrets-and-encryption.md)), and no ability to push, deploy or publish without
an explicit gate.

The default is that an agent can *propose* freely and *promote* nothing.

### 4. Reversibility as a unit

"The agent made 40 edits and 3 are wrong" is the normal case, not the exception.

Existing tools checkpoint partially. Claude Code's `/rewind` restores around user
prompts but tracks only direct `Write`/`Edit`/`NotebookEdit` operations — **not
shell-created changes, most subagent edits, directory operations, or concurrent changes
from elsewhere.** Those fall back to git.

liquid has no such gap, because capture is at the **filesystem layer**
([22](22-change-capture.md)): a file written by a shell command inside an agent's
session is a drip exactly like one written through an editor API. An agent's whole
session is an addressable range; `liquid undo` reverses it atomically, or selectively by
file or by drip. Issue #30 supplies a delegation's session identifier; absent an
explicit identifier, each open repository handle generates a random 16-byte value.

Collaborative undo follows **local intent**. Each operation records its own before and
after values, and undo derives the inverse from that record rather than from the
document's current head. If later local or remote work changed the same value, the
undo report marks that path skipped. Concurrent work on another path remains
untouched and converges normally after synchronization.

### 5. Review before promotion

Agent work becomes a **change** ([02](02-version-control.md)) reviewed in lake
([18](18-lake-service.md)) like any other. Because review attaches to change identity
rather than to a branch, an agent revising its own work does not orphan the review.

## The danger that gets worse with agents

**Convergence is not correctness**, and agents make the gap matter far more.

A CRDT will merge two concurrent edits without complaint. For text this can interleave —
concurrent insertion of `hello` and `world` can converge to `hweolrllod`. Automerge uses
RGA for text, which is the family with this anomaly.

Worth stating precisely: 2025 research showed that **avoiding every possible form of
interleaving is impossible**. Fugue and FugueMax provide *maximal* non-interleaving, not
perfection. So this is a hazard to be managed, never eliminated — which is why the
defences below matter more than the choice of algorithm.

With humans this is rare: two people seldom edit the same function in the same second.
**With agents it is likely** — they work fast, broadly, and often on exactly the region
another agent was asked to fix. The result can be syntactically valid and semantically
wrong, with no conflict raised anywhere.

liquid's defences, in order:

1. **Structural merge over the CRDT result** ([03](03-diff-and-merge.md)). The CRDT
   guarantees nothing is lost; structural merge decides whether the merged tree is
   meaningful.
2. **Surface converged-but-suspicious regions for review** rather than accepting them
   silently. "No conflict" must never mean "no attention required."
3. **Semantic leasing.** File, symbol, AST-node or range claims, *even though the CRDT
   can technically merge overlaps.* The research's formulation is the right one: **a
   lease protects intent, not convergence.** Two agents dispatched at the same symbol is
   usually an orchestration mistake, and liquid can refuse it before the merge rather
   than reconcile it after. AgentRoom reaches the same conclusion with file claims —
   implicitly acknowledging convergence alone is insufficient.
4. **Advisory locks** for genuinely unmergeable content ([02](02-version-control.md)).
5. **Guard against stale reasoning.** An agent may reason against an obsolete snapshot
   while human edits arrive. *Deterministic merge cannot repair decisions made from
   stale semantics* — so an agent's update should carry the version it reasoned from,
   and be re-validated if the base moved materially.

## Presence for non-human actors

Showing what an agent is doing *while it does it* is the difference between fluid
collaboration and a surprise commit. Presence covers agents as first-class:

- which actors are active, and where;
- what an agent is currently touching, live;
- **interrupt and steer** in flight, rather than waiting for it to finish and reverting.

Cursors and selections anchor to stable character identities rather than offsets, so
they survive concurrent edits ([research](../research/24-editors-and-ide-architecture/)).

## Why this matters beyond code

An agent workforce operating on enterprise data needs exactly three things liquid
provides structurally: **availability** (the data is reachable and current),
**provenance** (what the agent did and on what basis), and **reversibility** (undo when
it is wrong). That is the same argument as [23](23-metis-and-zetta.md), applied to the
actors rather than the data.

Most agent tooling today has no principled answer to "what exactly did the agent change,
under whose authority, and can I undo precisely that?" liquid does, because drips,
actors and attestations were already required for other reasons.

## Open questions
- **Tombstone growth under machine-rate writers**, now partly quantified. In one
  benchmark, 260,000 operations cost Automerge ~44 MB and 1.8 s versus Yjs ~10 MB and
  1.1 s — because **Automerge deliberately retains richer history**, which is exactly
  what supports version-control-like features. That is the right trade for liquid and it
  is not free. The mitigation is **epoch compaction**: periodically materialise a
  canonical snapshot and start a new history epoch once peers acknowledge it, with
  clients older than the epoch resyncing rather than replaying. This needs specifying,
  and it is the same mechanism the general history-growth risk in
  [17](17-implementation.md) requires.
- **Whether agent drips should be a separate stream** that is promoted into the main
  history on acceptance, rather than landing in it directly.
- **Text CRDT choice.** Automerge's RGA has known interleaving anomalies; Fugue/FugueMax
  (Loro) provide maximal — not total — non-interleaving. Whether that difference
  justifies revisiting the datastore decision is open, and the private-replica pattern
  above reduces how much it matters.
- **Presence at scale** — dozens of agents in one workspace is a UI problem nobody has
  solved.
- **Data isolation for agents.** Git worktrees do not isolate a database; liquid should
  extend isolation to data namespaces, in the manner of copy-on-write database
  branching. Currently unspecified.
