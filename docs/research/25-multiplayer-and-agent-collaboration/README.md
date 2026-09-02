# Multiplayer and agent collaboration

> Evidence: [`sources.md`](sources.md)
>
> Context for **[24-multiplayer-and-agents](../../features/24-multiplayer-and-agents.md)**.

## The state of the art, September 2026

**Nobody ships human-agent CRDT co-editing.** Claude Code, Cursor, Codex, Devin and
GitHub Copilot all handle concurrency the same way: **isolation** — a separate git
worktree, branch, container or cloud VM per task — plus reviewable diffs. Not shared
buffers.

Working demonstrations of the CRDT approach exist and the pattern is beginning to be
productised — Electric's agent-as-Yjs-peer demo, AgentRoom, CodeCRDT, ODocs — but none is
a mainstream code-development norm. Google Docs with Gemini stages *suggestions* for
accept/reject rather than exposing the model as an autonomous peer.

So a CRDT-based approach is genuinely differentiated **and** genuinely unproven.

## Isolation defers the problem rather than solving it

This is the finding that matters most, and it reframes the risk:

> Isolation prevents silent filesystem overwrites, but overlapping work reappears later
> as Git merge conflicts or, **more dangerously, as cleanly merged but semantically
> inconsistent code.**

Git catches textual line conflicts. It does not catch two agents independently changing
an API contract and all its callers — that merges cleanly and is wrong.

**The semantic-inconsistency problem therefore already exists in every worktree-isolated
agent workflow today.** A CRDT does not introduce it. What a CRDT changes is *when* the
overlap becomes visible — immediately, rather than at merge time — which is an
opportunity to detect it, provided something above the CRDT is actually looking.

## Where current isolation leaks

| Tool | Documented gap |
|---|---|
| **Claude Code** | Top-level sessions and `isolation: worktree` subagents are isolated, but **ordinary subagents and agent-team teammates can share the parent working tree**. In one shared tree there is no cross-session awareness or file locking: stale reads, whole-file rewrites and `git add -A` can overwrite or accidentally commit another actor's edits. Anthropic recommends partitioning files precisely because teammates are not worktree-isolated. |
| **Cursor** | Parallel Agents (2.0, Oct 2025) run up to eight agents in worktrees or remote machines. Documented 2025–2026 bugs: incomplete applies, deleted files not applying, duplicated paths, follow-up turns unexpectedly editing `main`, worktree sessions jumping back to the main checkout. |
| **OpenAI Codex** | Cloud tasks are isolated containers, but patches conflict when applied to the same base. OpenAI's own documentation warns that parallel agents editing simultaneously increase conflict and coordination overhead. |
| **All of them** | **Git isolation does not isolate data.** Worktrees still share one `DATABASE_URL`, so one agent's migration, seed or truncate hits everyone. Copy-on-write database branching (e.g. Neon) is a separate mechanism that must be added deliberately. |

The recommended discipline that emerges: one agent → one worktree/branch → preferably
one VM/container → **one disposable database namespace.**

## Attribution: tool-level markers, and nothing more

Shipping conventions are commit trailers:

| Tool | Convention |
|---|---|
| Claude Code | `Co-Authored-By: Claude <noreply@anthropic.com>` (configurable, can be emptied) |
| Cursor | `Made-with: Cursor` / `Co-authored-by: Cursor` |
| OpenAI Codex | `Co-authored-by: Codex <noreply@openai.com>` — added February 2026 |
| Devin | `Generated with Devin` / `Co-Authored-By: Devin` |

**Every one of these identifies a vendor label and nothing else.** None records the
session, the prompt, the tool calls, which files were generated versus merely edited, or
how much a human revised afterwards. Session logs hold stronger evidence, but the
trailer does not durably link to them.

`Co-Authored-By` is also contested on its own terms: it was designed for human joint
authorship, and GitHub renders the named identity as a co-author.

The gap is the opportunity — provenance recorded as a signed fact rather than a
convention someone can forget or disable.

## Reversibility: partial today

Claude Code's `/rewind` checkpoints around user prompts and can restore conversation,
code or both. But it tracks only direct `Write`, `Edit` and `NotebookEdit` operations —
**not shell-created changes, most subagent edits, directory operations, remote files, or
unrelated concurrent changes.** Those fall back to git or external snapshots.

"The agent made 40 edits and 3 are wrong" is the normal case, and the tooling covers
some of the edits.

## CRDTs under machine-rate writers

The distinctive failure modes when the writer is not a human:

| Effect | Consequence |
|---|---|
| **Fine-grained streamed insertion** | Token- or character-level operations at the same position can interleave passage by passage. 2025 research established that **avoiding every form of interleaving is impossible**; Fugue/FugueMax provide *maximal* non-interleaving, not perfection. |
| **Thousands of ops/second** | Parsing, CRDT integration, rendering, persistence, awareness and fan-out become the bottleneck — not bandwidth. UI threads can spend all their time applying updates. |
| **Rewrite/delete/regenerate loops** | Every deleted character can leave causal metadata. History-retaining systems grow with all operations ever performed, not with visible document size. |
| **Many replicas, offline clients** | Safe GC requires knowing no replica can refer to old identifiers. A permanently offline client forces long retention or an epoch reset. |
| **Agent reads while emitting** | The agent reasons against an obsolete snapshot while human operations arrive. **Deterministic merge cannot repair decisions made from stale semantics.** |
| **Whole-file rewrites** | A formatter or generator replacing an entire file creates enormous churn and tombstones versus an AST- or range-level edit, and erases authorship granularity. |

**Measured cost.** In one Automerge 2 benchmark, ~260,000 operations used roughly **44 MB
and 1.8 s**, versus about **10 MB and 1.1 s** for Yjs. Yjs compresses adjacent structures
and garbage-collects some deleted content; **Automerge deliberately retains richer
history**, which is what supports version-control-like features — at a cost.

## The backpressure playbook

The most directly actionable part of the research:

| Control | Behaviour |
|---|---|
| **Private speculative replica** | Give the agent a snapshot or private replica; let it generate and revise there; publish **one bounded update** for the accepted result. Avoids exposing half-written syntax and collapses token streaming into a coherent transaction. Electric's 2026 Yjs demonstration uses exactly this. |
| **Batch / coalesce** | Group adjacent inserts and deletes into transactions; merge updates, removing duplicate information. |
| **Flow control** | Bound unacknowledged operations or bytes per peer; pause generation when persistence or rendering lag crosses a watermark. |
| **Rate limits** | Token buckets per agent, document and tenant, plus maximum update size and history quotas. **Must pause or reject explicitly** — silently dropping operations breaks causal dependencies. |
| **Semantic leasing** | File, symbol, AST-node, cell or range claims *even though the CRDT can merge overlaps.* **A lease protects intent, not convergence.** |
| **Epoch compaction** | Periodically materialise a canonical snapshot and begin a new history epoch once relevant peers acknowledge it. Clients older than the epoch resync rather than replay indefinitely. |
| **Agent-aware rendering** | Throttle highlighting, diagnostics and tests independently from ingestion. Show *"agent changing lines 80–140"* rather than animating 10,000 keystrokes. |

## Implications for liquid

1. **Claim novelty and admit it is unproven.** No mainstream tool does this. That is the
   differentiation and the risk, and the feature doc should say both.

2. **Reframe the semantic-merge risk correctly.** Cleanly-merged-but-wrong already
   happens with worktrees and git merge. liquid does not create the problem; it makes the
   overlap visible early enough to detect. That is a defensible position, and materially
   stronger than "our CRDT merges everything."

3. **Adopt the private speculative replica as the default agent interface.** Do not let a
   model's token stream touch the shared document. This is the single most actionable
   recommendation available and it resolves interleaving, churn and half-written-syntax
   exposure together.

4. **Implement semantic leasing.** Convergence is not the goal; protecting intent is.
   Two agents dispatched at one symbol should be refused before the merge, not
   reconciled after.

5. **Record provenance as a fact, not a trailer.** Every shipping convention is a vendor
   label. Which agent, which model, which principal, which authority — signed, using the
   attestation machinery liquid already has.

6. **Capture at the filesystem layer is a concrete advantage.** `/rewind` misses
   shell-created changes and subagent edits; drips do not, because they are below the
   tool API rather than inside it.

7. **Specify epoch compaction.** It is the answer to tombstone growth, and Automerge's
   history-retention choice — right for liquid — makes it necessary rather than optional.

8. **Extend isolation to data.** One agent → one worktree → **one disposable database
   namespace.** Git isolation stops at files, and that gap causes real incidents.

9. **Avoid whole-file rewrites.** Push agents toward range- or AST-level edits, which
   liquid's structural layer can express and which cost dramatically less in history.
