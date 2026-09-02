<!-- Raw research source. Generated via Perplexity `perplexity_research` (web-grounded,
     multi-source) on 2026-09-02 for the liquid project. Lightly reformatted only.
     Treat as evidence, not as liquid design decisions - see README.md in this
     directory for the curated analysis and the implications drawn for liquid. -->

# Research report: concurrent human/agent editing of code and data, 2025–2026

**State as of September 2, 2026:** mainstream coding agents support parallelism primarily by giving each task a **separate Git worktree, branch, VM, container, or cloud sandbox**. They generally do **not** provide Google-Docs-style simultaneous editing of the same source file. Isolation prevents silent filesystem overwrites, but overlapping work reappears later as Git merge conflicts or, more dangerously, as cleanly merged but semantically inconsistent code. Real-time human/agent CRDT co-editing exists in research prototypes, demonstrations, and a few niche platforms, but not as the normal editing model of Claude Code, Cursor, Codex, Devin, or GitHub Copilot.

---

## 1. Concurrent agentic coding tools

### Shipping isolation and collision behavior

| Tool | 2025–2026 status | Isolation used for concurrent work | If multiple agents or a human and agent edit the same files |
|---|---|---|---|
| **Claude Code** | Shipping; parallel sessions, worktrees, subagents, agent teams, and a research-preview Agent View are documented. | A top-level session can run in a separate Git worktree; Agent View places dispatched sessions in worktrees automatically. Custom subagents can request `isolation: worktree`. However, ordinary subagents and **agent-team teammates can share the parent working tree** unless deliberately isolated or partitioned by files. [web:32][web:35] | Separate worktrees prevent mid-flight overwrites but defer overlap to Git merge/cherry-pick time. In one shared tree, sessions have no inherent cross-session awareness or file locking: stale reads, whole-file rewrites, and `git add -A` can overwrite or accidentally commit another actor’s edits. Anthropic specifically recommends partitioning files for agent teams because teammates are not worktree-isolated. [web:23][web:415] |
| **Cursor** | Parallel Agents shipped in Cursor 2.0 in October 2025; Cursor 3 and the 2026 Agents Window expanded explicit `/worktree`, `/best-of-n`, local, cloud, and SSH execution. | Up to eight agents may run on one prompt in separate Git worktrees or remote machines. Cloud agents run on separate Ubuntu VMs; `/worktree` keeps the main checkout untouched until `/apply-worktree`. [web:78][web:76][web:89] | In a local non-worktree session, edits save directly to the shared workspace, so normal last-writer/stale-read hazards apply. Worktrees move collisions to Apply/merge time. Documented 2025–2026 bugs included incomplete applies, deleted files not applying, duplicated paths, follow-up turns unexpectedly editing `main`, and worktree sessions “jumping” back to the main checkout. [web:82][web:437][web:439][web:443] |
| **OpenAI Codex** | Codex cloud launched in May 2025; the IDE/CLI and Codex app subsequently added local/cloud handoff, multi-agent execution, and built-in worktrees. | Every Codex cloud task runs independently in its own isolated cloud container with a repository checkout. The Codex app uses separate threads and isolated worktrees. Local CLI execution is sandboxed but remains attached to the selected local working directory unless a worktree is used. [web:15][web:66][web:185] | Cloud tasks cannot clobber each other’s live files, but patches can conflict when applied to the same base. Local parallel write-heavy subagents can conflict; OpenAI’s documentation explicitly warns that parallel agents editing code simultaneously increase conflict and coordination overhead. [web:62] |
| **Devin** | Shipping cloud sessions, managed parallel Devins, local CLI, workflows, and self-hosted Outposts. | The usual cloud model is one isolated VM per Devin session. Managed Devins each receive their own VM. Outposts can run sessions on customer VMs, containers, or Kubernetes workers. Some dynamic-workflow subagents deliberately run on the coordinator’s VM and share its working tree. [web:46][web:47][web:57] | Independently scoped sessions normally produce separate branches/PRs, so overlap becomes a merge conflict. Devin’s own batch guidance says shared-helper conflicts must be manually resolved or delegated back to Devin. Shared-VM workflow agents can collide in the live tree and also compete for CPU, memory, and disk. [web:52][web:57] |
| **GitHub Copilot coding/cloud agent** | Public preview began May 2025 and GA followed September 25, 2025. | Runs in a restricted GitHub Actions-backed sandbox. A task normally creates a dedicated `copilot/…` branch and PR; the agent can push only to that branch. When invoked on an existing PR it is granted access to that PR branch, or may create a separate PR layered on top. [web:109][web:112][web:423][web:434] | Parallel tasks are branch-isolated. Humans may check out and push to a Copilot branch, but that makes it an ordinary shared Git branch: commits may coexist, or later pushes/rebases may conflict. Divergence from the base produces normal merge conflicts. Since March–April 2026, users can ask Copilot to propose conflict resolutions, which still require review. [web:431][web:425][web:432] |
| **GitHub Copilot Workspace** | **Not shipping.** Its technical preview was sunset on May 30, 2025; its issue-to-plan-to-implementation ideas were absorbed into newer Copilot agent workflows. [web:106] | During preview it created a proposed plan and implementation in a GitHub-hosted workspace, eventually yielding a branch/PR rather than a true shared filesystem. | It should not be evaluated as a current concurrency solution. It did not establish simultaneous human/agent operational editing of one source buffer. |

### What isolation does—and does not—solve

| Situation | Actual behavior |
|---|---|
| **Two writers in one checkout** | Filesystem isolation is absent. Targeted patch tools may reject an edit if the expected old text no longer matches, but full-file writes, shell redirection, formatters, generators, or stale fuzzy replacements can silently replace recent work. A documented Claude failure involved one session committing a stale CI file and silently overwriting another session’s work. [web:408][web:415] |
| **Separate worktrees/branches** | Uncommitted state is safe because writers touch different directories. Overlap is detected only when the branches are applied, rebased, cherry-picked, or merged. Git catches textual line conflicts, not semantic conflicts such as two agents independently changing an API contract and all its callers. |
| **Agent and human on the same remote branch** | Git commits provide some serialization, but the second actor must fetch/rebase before pushing. Force-pushes, generated files, or an agent that assumes an old branch head remain hazardous. |
| **Agent edits a PR while the human works** | The safer pattern is a stacked/secondary PR: GitHub’s “ask Copilot on any PR” leaves the original branch untouched until the human merges Copilot’s proposed PR. [web:434] |
| **Code branch but shared database** | Git isolation does not isolate schema or data. All worktrees can still point at the same `DATABASE_URL`, so one agent’s migration, seed, truncate, or test cleanup affects everyone. A database branch or disposable database is required separately. Neon, for example, documents copy-on-write database branches whose writes do not affect the parent. [web:451][web:463] |

The database distinction is not theoretical. In July 2025 a Replit agent deleted production data during development; Replit’s CEO called it unacceptable and announced automatic development/production separation, stronger rollback, and planning-only mode. [web:466] The architectural lesson is that **a code sandbox is incomplete if credentials still reach shared production data**.

### Practical concurrency rule

The reliable 2026 pattern is:

1. one task → one agent identity;
2. one agent → one worktree/branch and preferably one VM/container;
3. one worktree → one disposable database/data namespace;
4. explicit file or subsystem ownership;
5. frequent, narrow commits;
6. integrate through reviewed PRs, not by copying entire files;
7. serialize agents that must change the same API, schema, lockfile, generated artifact, or configuration root.

---

## 2. Attribution and provenance of AI-authored changes

### What tools actually record

| Tool or convention | Shipping attribution behavior | Provenance quality |
|---|---|---|
| **Claude Code** | Agent-created commits normally include a `Co-Authored-By: Claude … <noreply@anthropic.com>` trailer. Newer settings permit commit and PR attribution strings to be changed or emptied. [web:91][web:100] | Identifies a vendor/model label at commit level, but generally not the exact session, prompt, tool calls, files generated versus merely edited, or amount of human revision. |
| **Cursor** | Depending on surface and settings, commits/PRs use `Made-with: Cursor` or `Co-authored-by: Cursor`. IDE/CLI attribution can be disabled; cloud-agent attribution historically used a separate server-side mechanism and was not always controlled by the desktop setting. [web:99][web:101] | Tool-level marker only; not line-level authorship or reproducible generation provenance. |
| **OpenAI Codex** | Early 2025 Codex had no universal attribution convention. In February 2026 the CLI introduced managed commit co-author attribution with a default/custom/disabled setting, normally producing `Co-authored-by: Codex <noreply@openai.com>`. [web:481][web:482] | Model/version and prompt are not necessarily retained in Git. Cloud task history may contain richer evidence, but the trailer alone does not link it durably. |
| **Devin** | Attribution is on by default in the CLI configuration: commit/PR bodies can include `Generated with Devin` and `Co-Authored-By: Devin`; it can be disabled. [web:123] | Declares tool involvement but does not prove which lines or commands came from Devin. Session logs provide the stronger record. |
| **Copilot coding agent** | Every cloud-agent commit is authored by Copilot, with the dispatching human listed as co-author. Since March 2026, commit messages link to agent session logs, allowing a reviewer to trace the task and actions. [web:97][web:229] | The strongest mainstream implementation surveyed: distinct bot identity plus a durable session-log link. It still depends on GitHub retaining and authorizing access to the logs. |
| **Apache `Generated-by:`** | Apache’s legal guidance recommends a source-control token such as `Generated-by:` for contributions made using generative tooling. [web:163] | Better semantics than “co-author,” but no universal field schema. |
| **Linux kernel `Assisted-by:`** | The 2026 kernel policy requires human responsibility and prohibits agents from adding `Signed-off-by`; significant AI assistance can be recorded through an `Assisted-by` trailer identifying the model/agent/tools. [web:172][web:175] | Separates assistance from legal authorship and DCO certification; currently the clearest emerging commit-message pattern. |

### Why `Co-Authored-By` is contested

`Co-Authored-By` was designed for human joint authorship and GitHub renders the named identity as a co-author. Objections are therefore both semantic and operational:

- a model is not a legal person or copyright author;
- “co-author” does not distinguish autocomplete, review, a small rewrite, or an autonomous 40-file change;
- vendor-added trailers act as branding and can alter contribution graphs;
- model names may disclose internal tooling choices;
- trailers can be removed by squash, rebase, patch transfer, or a user setting;
- they do not show the prompt, retrieved context, tool calls, human review, or which lines survived;
- they can be confused with legal certification. Anthropic users pushed back specifically against default, non-optional co-author branding. [web:93][web:100]

A better vocabulary is:

| Meaning | Preferred record |
|---|---|
| Human accepts legal responsibility | Human `Author` plus, where required, human `Signed-off-by` |
| AI materially helped | `Assisted-by: <agent>, <model>, <tools>` |
| Output was largely generated | `Generated-by: <agent/model>` |
| Autonomous agent made the commit | Distinct bot/agent Git identity |
| Exact execution evidence | Signed session or run attestation linked by immutable identifier |
| Human validation | `Reviewed-by`, approval record, CI results, and policy decision |

### DCO and sign-off

A `Signed-off-by` trailer is not an authorship decoration: it certifies the Developer Certificate of Origin. An agent cannot attest that it wrote the contribution, has the right to submit it, or agrees to license terms. Current Linux kernel policy therefore says AI agents **must not** add sign-offs; the human must review the output, verify licensing, sign personally, and take responsibility. [web:172] Open Policy Agent likewise requires every AI-assisted commit to be signed off by a human and warns that unverifiable-origin patches may be rejected. [web:176]

An agent may mechanically run `git commit -s` using the human’s configured identity, but that is dangerous because it creates the appearance of a human certification without proving review. A protected workflow should require the human to add or cryptographically approve the sign-off after inspecting the final diff.

### Copyright and licensing

Under the U.S. Copyright Office’s January 2025 analysis:

- prompts alone ordinarily do not supply enough expressive control for copyright authorship;
- purely machine-generated output is not copyrightable;
- human selection, arrangement, or creative modification can be protected, but only to the extent of the human contribution;
- using AI as an assistive tool does not make the surrounding human-authored work unprotectable. [web:152][web:161]

That does **not** mean generated code is automatically safe to license. A noncopyrightable output may still contain a reproduction or adaptation of third-party copyrighted code. Provenance review, dependency/license scanning, and source-reference checking remain necessary. Copilot coding agent now highlights matches with public GitHub code in session logs and links to the source and possible license; unlike ordinary completion “Block” mode, the coding agent logs rather than necessarily suppressing such output. [web:504]

### Emerging provenance standards: actual versus proposed

There was **no universally adopted Git standard for AI-change provenance by September 2026**. What is real is a set of partially overlapping conventions:

| Mechanism | Status | Records |
|---|---|---|
| `Co-Authored-By`, `Generated-by`, `Assisted-by`, `Made-with` | Shipping conventions | Human-readable tool/model participation |
| Bot commit identity plus session-log URL | Shipping in GitHub Copilot agent | Agent identity and trace back to the hosted run |
| PR conversation and tool transcript | Shipping in several cloud agents | Prompts, commands, test results, and follow-ups, subject to retention/access |
| AgentPack dataset | Research artifact | 1.3 million public commits associated with Claude Code, Codex, and Cursor from part of 2025, illustrating that trailers can support ecosystem analysis. [web:124] |
| CycloneDX/SPDX AI or agent BOM extensions | Research and standards work, not yet a code-authorship norm | Model lineage, datasets, agent/tool descriptors, execution context |
| Prompt hashes, model IDs, signed per-run attestations, Git notes or sidecars | Emerging proposals/prototypes | Tamper-evident, privacy-preserving provenance beyond commit trailers |

A practical organization-level schema should minimally retain: **agent/tool and version, model identifier, session/run ID, prompt hash or protected transcript reference, starting commit and environment digest, files and external systems touched, approvals, test evidence, resulting commit hashes, and human reviewer**. Sensitive prompts should generally be access-controlled and hash-linked rather than copied into public commits.

---

## 3. Agent permissions and scoped authority

### Why the agent should have less authority than its dispatcher

Delegation is not identity cloning. The human may have broad authority because they can exercise judgment across many tasks; the agent is executing one bounded workload while processing untrusted repository text, issue content, web results, package metadata, and tool output. Prompt injection, model mistakes, stale context, and runaway loops can therefore turn inherited human credentials into a confused-deputy vulnerability.

OWASP’s agent guidance recommends minimum tool sets, read/write resource scoping, explicit authorization for sensitive tools, action previews, human approval for irreversible operations, audit trails, interruptibility, and rollback. It also recommends separating model decision-making from an independent execution/policy service. [web:224]

### Recommended authority model

| Layer | Required control |
|---|---|
| **Identity** | Give each run a separate non-human workload identity, not the human’s SSH key, browser session, cloud token, or personal PAT. Record both `delegated-by=<human>` and `executed-by=<agent-run>`. Google’s workload-identity model similarly uses service accounts, federation, and dedicated agent identities rather than human credentials. [web:214][web:215] |
| **Capabilities** | Issue narrow, unforgeable capabilities: read repository X at commit Y; write only branch Z; read a particular issue; query but not mutate production; use one named deployment target. Possessing “shell access” should not imply arbitrary network, secrets, Git, database, or cloud access. |
| **Lifetime** | Mint short-lived, task-bound credentials after dispatch and revoke them on stop, timeout, or merge. Avoid long-lived secrets in `.env`, shell history, prompts, or worktree setup scripts. |
| **Filesystem** | Default read-only outside the task checkout. Restrict writes to one worktree and explicitly approved generated/output directories. Treat symlinks, mounted sockets, parent directories, `/tmp`, Git hooks, and credential/config files as escape paths. |
| **Network** | Deny by default or allowlist required registries and APIs. Apply independent controls to MCP servers and browser automation. |
| **Git** | Permit pushing only the agent-created branch; deny default-branch push, tag/release creation, force-push, approval, and merge. |
| **Data** | Supply a disposable database branch or masked snapshot. Production should be read-only by default; mutations require an exact transaction preview, human approval, and externally controlled backup/restore. |
| **High-impact actions** | Require gates for package publication, infrastructure changes, external messages, secrets access, deleting data, migrations, deployments, and merging. Approval must bind the exact tool, target, normalized parameters, expiry, and content—not “allow Bash forever.” |
| **Audit** | Log prompts, model/tool versions, reads where sensitive, every command/tool call, changed resources, network destinations, approvals, stdout/stderr, commits, and stop/rollback events to storage the agent cannot rewrite. |

### What current products enforce

| Product | Shipping controls |
|---|---|
| **Claude Code** | Layered permissions; writes are normally constrained to the starting project and subdirectories, with explicit permission required for broader actions. Newer versions provide filesystem/network sandbox configuration and hooks. [web:186][web:181] |
| **Cursor** | Workspace edits ordinarily happen without per-edit approval, while terminal commands, MCP connections, configuration changes, and sensitive operations require approval unless allowlisted. `.cursorignore`, command allow/deny lists, shell sandboxing, and cloud network allowlists are available; Cursor warns that auto-review classifiers are best-effort rather than a hard security boundary. [web:184][web:188] |
| **Codex** | Local commands run under sandbox and approval policies restricting writable roots, commands, and network. On Windows, AppContainer-style capabilities are used, though world-writable paths are a documented limitation. [web:185] |
| **Devin** | Reusable security profiles can restrict network, MCP, Git, and GitHub CLI access and can be pinned to sessions and automations. Profiles include read-only versus full Git access. [web:207][web:54] |
| **GitHub Copilot coding agent** | Only repository writers may dispatch it; it can push only its assigned branch, cannot approve or merge its PR, and draft PRs require human review. Agent-triggered Actions workflows require “Approve and run” by default, and agent PRs may require an additional approval. [web:197] GitHub enterprise audit logs can be queried with `actor:Copilot`. [web:200] |

The best-shipping example of “less authority than the dispatcher” is GitHub Copilot’s inability to approve or merge its own work even when the dispatching human could do so.

---

## 4. Reversibility and the “40 edits, 3 wrong” problem

### Current product behavior

| Tool | Unit rollback | Selective correction |
|---|---|---|
| **Claude Code** | A checkpoint is created around user prompts; `/rewind` can restore conversation, code, or both. | Checkpointing tracks direct `Write`, `Edit`, and `NotebookEdit` operations, but not shell-created changes, most subagent edits, directory operations, remote files, or unrelated concurrent changes. Those require Git or external snapshots. [web:242][web:243] |
| **Cursor** | Chat checkpoints restore all workspace files to the state at a selected point. Worktree results can be discarded without touching the main checkout. [web:241] | Review UI can expose individual edits, but checkpoint restore is state-oriented rather than “remove exactly these three semantic changes.” Apply/Undo Apply had documented completeness and review-card bugs during the period. [web:445][web:449] |
| **Codex** | CLI `/undo` rolls back an unwanted edit/command; `/diff` exposes the current Git diff. Cloud/worktree tasks can be discarded or reverted as branch changes. [web:250][web:382] | Usually Git/hunk-oriented review or a corrective follow-up prompt; no universal semantic edit ledger spanning all shell and external side effects. |
| **Devin** | CLI `/revert <step>` returns files and conversation to before a selected step. Cloud branches/PRs can be abandoned or reverted. [web:240] | The session timeline records shell commands, code edits, and browser actions, but external actions may still need compensating operations. |
| **Copilot coding agent** | The branch or whole PR is the natural rollback unit; stop terminates the Actions run but preserves commits already pushed. A merged PR can be reverted through Git. [web:229] | Humans can edit the branch, request follow-up commits, or selectively stage/revert hunks. There is no general “undo three intent-level edits” command across a completed PR. |
| **VS Code Copilot/agent editing** | Conversation checkpoints restore the state before a request. Editing an earlier request reverts that request and subsequent ones before rerunning. | Diff controls provide **Keep/Undo per individual edit**, which is the clearest shipping answer to “40 edits, 3 wrong,” provided the edits were captured as editor operations. [web:258][web:259] |

### The unresolved granularity problem

Existing mechanisms operate at different levels:

| Level | Strength | Limitation |
|---|---|---|
| **Editor hunk** | Keep 37 edits and reject 3 visibly bad hunks. | A logical change may span several hunks; generated files and shell edits may not be represented cleanly. |
| **Prompt/checkpoint** | Undo the whole agent turn as one unit. | Also removes correct work performed during that turn. |
| **Commit** | Durable, auditable and easily reverted. | Agents often combine many intentions in one commit; `git revert` may conflict after later work. |
| **Branch/PR** | Strong isolation and easy wholesale discard. | Selective adoption still requires hunk staging, cherry-picking, or manual editing. |
| **External-system transaction** | Database rollback or compensating action can restore state. | Git cannot reverse emails, deployments, package publication, secrets exposure, or destructive database actions. |

The best practical workflow is to have the agent make **small intent-labeled commits**, then review with a hunk-level UI. For 40 changes with 3 wrong: reject those hunks if independent; otherwise ask for a corrective commit rather than rewinding all work. Keep the original agent commits intact until review so attribution and audit remain visible.

For data, take a pre-run snapshot or database branch and wrap mutations in transactions where possible. “Undo” must be implemented as a tested inverse or compensating transaction; it cannot be inferred reliably after arbitrary side effects.

---

## 5. CRDTs under high-rate automated writers

### What changes when the writer is an agent

CRDT convergence does not imply sensible merged code. A text CRDT guarantees that replicas eventually agree on the same character sequence; it does not guarantee that the sequence compiles, preserves both writers’ intentions, or maintains an API invariant.

| High-rate effect | Consequence |
|---|---|
| **Fine-grained streamed insertion** | If two agents emit token- or character-level operations at the same position, some sequence CRDTs can interleave passages character by character or run by run. Research published in 2025 showed that common replicated-list approaches can produce unreadable interleaving and that avoiding every possible form of interleaving is impossible; Fugue/FugueMax instead provide maximal non-interleaving. [web:271][web:272] |
| **Thousands of operations per second** | Parsing, CRDT integration, rendering, persistence, awareness updates, WebSocket framing, and fan-out—not merely network bandwidth—become bottlenecks. UI threads may spend all their time applying updates and recomputing syntax/diagnostics. |
| **Repeated rewrite/delete/regenerate loops** | Every deleted character or object may leave causal metadata or tombstones. History-retaining systems can grow with all operations ever performed rather than visible document size. |
| **Many replicas and offline clients** | Safe garbage collection becomes harder because the system must know that no replica can later refer to old identifiers. Permanently offline clients can force long retention or require epoch/snapshot resets. |
| **Agent reads while emitting** | The agent may reason against an obsolete snapshot while human operations are arriving. Deterministic merge cannot repair decisions made from stale semantics. |
| **Formatter/generator behavior** | Replacing an entire file creates enormous churn and tombstones compared with an AST-level or range-level edit, and can erase useful authorship granularity. |

The cost is implementation-dependent. In an Automerge 2 benchmark, inserting about 260,000 operations used roughly 44 MB and 1.8 seconds, versus about 10 MB and 1.1 seconds for Yjs; optimized encodings improve the constants but do not eliminate operation-history cost. [web:302] Yjs compresses adjacent structures and can garbage-collect some deleted content, while Automerge deliberately retains richer history; a comparative study notes that Yjs trims or compresses stale metadata whereas Automerge’s full-history choice supports version-control-like features at a cost. [web:310][web:315]

### Backpressure design

A production human/agent CRDT system should not feed every model token directly into the live document.

| Control | Recommended behavior |
|---|---|
| **Private speculative replica** | Give the agent a snapshot/private CRDT replica, let it generate and revise there, then compute one bounded CRDT update for the accepted result. This avoids exposing half-written syntax and collapses token streaming into a coherent transaction. A 2026 Yjs agent demonstration uses this private-copy pattern before applying one resulting update. [web:330] |
| **Batch/coalesce** | Group adjacent inserts/deletes into transactions and merge binary updates. Yjs supports combining updates while removing duplicate information. [web:294] |
| **Flow control** | Bound unacknowledged operations/bytes per peer. Pause model generation or CRDT publication when server, persistence, or renderer lag crosses a watermark. |
| **Rate limits** | Apply token buckets per agent, document, and tenant, plus maximum update size and document/history quotas. Rate limiting must pause or reject the producer explicitly; silently dropping CRDT operations can break causal dependencies. |
| **Semantic leasing** | Use file, symbol, AST-node, cell, table-range, or section claims even though the CRDT can technically merge overlaps. A lease protects intent, not convergence. |
| **Epoch compaction** | Periodically materialize a canonical snapshot and start a new history epoch after all relevant peers acknowledge it. Offline clients older than the epoch must resync rather than replay indefinitely. |
| **Agent-aware rendering** | Throttle syntax highlighting, LSP, tests, and UI repaint independently from CRDT ingestion. Show a summarized “agent changing lines 80–140” preview rather than animating 10,000 keystrokes. |

### Is production human-agent CRDT editing shipping?

| System/category | Status by September 2026 |
|---|---|
| **Claude Code, Cursor, Codex, Devin, GitHub Copilot** | **No:** their mainstream concurrency model is isolated files/branches/worktrees and reviewable diffs, not shared CRDT source buffers. |
| **Google Docs with Gemini** | **Human collaboration ships, but Gemini edits are primarily staged suggestions**: users accept individual changes, accept all, or reject all. This is not the same as exposing Gemini as an autonomous CRDT peer with live presence. [web:351][web:353] |
| **Electric “AI agent as CRDT peer”** | Working public demonstration: a server-side agent joins a Yjs document, has awareness/presence, and streams edits through the same synchronization layer. It is presented as a demo/build pattern, not evidence of broad production adoption. [web:331][web:349] |
| **Multi-User Collaborative Editing with AI Agents** | Research prototype using Yjs, Quill, shared agent profiles and tasks, and collaborative comments. [web:336] |
| **CodeCRDT / AgentRoom** | Research systems for concurrent agent coding. AgentRoom combines a CRDT filesystem with file claims, status and broadcasts—implicitly acknowledging that convergence alone is insufficient. [web:317][web:318] |
| **ODocs and agent-focused collaboration infrastructure** | Early/niche products advertise humans and agents editing the same CRDT-backed document through browser/API/MCP. This is evidence that the pattern is becoming productized, but not yet a mainstream code-development norm. [web:337] |

Thus the accurate answer is: **yes, small products and demonstrations exist; no, there is not yet a widely deployed mainstream human-agent CRDT coding environment comparable to Git-based agents or Google Docs for humans.**

---

## 6. Real-time UX for mixed human/agent teams

### What mainstream products show today

| Product | Presence/progress | Steering and interruption |
|---|---|---|
| **Claude Code** | Agent panels list teammates and allow opening their transcripts; split-pane mode displays all outputs simultaneously. Background task views expose status and sessions needing input. | Select a teammate, send it a message, or press Escape while viewing its transcript to interrupt its current turn. [web:362] |
| **Cursor** | Agents Window shows parallel tasks, branches/worktrees and cloud/local execution; results can be compared and moved into the foreground. | Follow-up prompts and queued messages steer a run, but ordinary Cursor UX emphasizes task cards/diffs rather than a live non-human cursor in the source buffer. |
| **Codex** | CLI/IDE display commands and diffs as they appear; cloud tasks can be tracked from the IDE, web interface, or app. | The active CLI turn can be steered while running; approval modes can be changed and commands inspected. [web:376][web:377] |
| **Devin** | Progress panels show workflow phases, child agents, status, tool-call counts and session links. The unified timeline records shell, code and browser actions. [web:381][web:390] | Users can continue talking during a background workflow, request summaries, put child sessions to sleep, terminate them, or stop a run while preserving resumable state. |
| **GitHub Copilot coding agent** | The Agents page streams session progress and logs; commits link back to the session. | A follow-up prompt redirects the agent after its current tool call. Stop ends the Actions run while preserving already-pushed commits. [web:227][web:229] |

These are chiefly **control-room UIs**, not true multiplayer presence. They show agents as jobs, cards, transcripts, branches, or terminal panes rather than as collaborators occupying selections in the same source buffer.

### Recommended mixed-team UX

| UX element | Purpose |
|---|---|
| **Explicit non-human identity** | Use a bot badge, distinct color/pattern, model/tool label, authority level, and dispatching human. Do not mimic an anonymous human collaborator. |
| **Scope/claim visualization** | Show the files, symbols, cells, table ranges, or data records the agent currently reads, owns, or intends to modify. Warn before another actor overlaps. |
| **State machine** | Display `planning`, `reading`, `editing`, `running tests`, `waiting for approval`, `blocked`, `publishing`, `stopped`, and `rolled back` rather than an undifferentiated spinner. |
| **Prospective action preview** | Before high-impact changes, show “about to modify 8 files, run migration X, and call endpoint Y,” including permissions and rollback path. |
| **Live but rate-limited traces** | Stream current file/command/test and a summarized diff. Do not animate thousands of token-level edits merely to appear human. |
| **Steering queue** | Let a human append constraints immediately; define whether they take effect after the current tool call, transaction, or checkpoint. Show that boundary clearly. |
| **Hard stop and pause** | Stop must revoke credentials and terminate child work, not merely stop UI output. Pause should preserve resumable state without continuing hidden subagents. |
| **Localized intervention** | Allow “stop editing this file,” “keep these hunks,” “do not touch this table,” “switch to read-only,” and “finish tests but do not commit.” |
| **Conflict awareness** | Alert both parties that another actor changed a read dependency or entered the same edit region; automatically refresh context before further writes. |
| **After-action ledger** | Present touched files/resources, commits, commands, approvals, external effects, test results and a one-click rollback or compensating procedure. |

Research supports presence but warns against naïve anthropomorphism. In Codellaborator, an AI caret/cursor improved participants’ awareness of what the agent was attending to and changing. [web:392] A 2026 collaborative-writing study found that a visual AI cursor could improve understanding of intent but also provoke feelings of surveillance, while synchronous suggestions sometimes became contextually misaligned. [web:391] The implication is to show **attention and action truthfully**, not to simulate human typing for theatrical effect.

---

## Bottom line

The strongest deployable architecture in 2026 is **speculative isolation plus controlled integration**, not unconstrained simultaneous writing:

1. agents work in identity-bound worktrees/branches, VMs and disposable data branches;
2. the human remains the legal author/signatory and final approver;
3. agent authority is narrower, shorter-lived and independently audited;
4. every run produces a reviewable diff, small commits and a rollback unit;
5. simultaneous same-file work is prevented through claims or integrated through CRDT/AST operations only where the product truly supports them;
6. UX exposes agent identity, scope, progress, approvals and interruption boundaries;
7. provenance links commits to durable run evidence rather than relying solely on `Co-Authored-By`.

CRDT human-agent editing is technically feasible and is beginning to ship in specialized document systems, but for production code and structured data the difficult problems are no longer convergence alone: they are **intent preservation, semantic conflict detection, authority, attribution, backpressure, review, and reversible external side effects**.

Citations:
[31] https://www.developersdigest.tech/blog/git-worktrees-claude-code-parallel-agents-guide
[32] https://code.claude.com/docs/en/worktrees
[33] https://crystl.dev/blog/git-worktrees-claude-code/
[34] https://code.claude.com/docs/en/checkpointing
[35] https://code.claude.com/docs/en/agents
[36] https://code.claude.com/docs/en/agent-sdk/file-checkpointing
[37] https://www.aibuilderclub.com/blog/claude-code-worktree-parallel-agents
[38] https://docs.claude.com/zh-CN/docs/claude-code/checkpointing
[39] https://docs.claude.com/pt/docs/claude-code/checkpointing
[40] https://docs.claude.com/de/docs/claude-code/checkpointing
[41] https://github.com/onmyway133/blog/issues/1028
[42] https://github.com/dgalarza/claude-code-workflows/blob/main/tips/worktrees-for-parallel-agents.md
[43] https://www.reddit.com/r/ClaudeAI/comments/1s978m3/i_built_a_parallel_agent_orchestrator_for_claude/
[44] https://claudedirectory.org/blog/claude-code-worktrees-guide
[45] https://code.claude.com/docs/ru/worktrees
[1] https://github.blog/news-insights/product-news/github-copilot-meet-the-new-coding-agent/
[2] http://github.com/apps/copilot-swe-agent
[3] https://docs.github.com/en/enterprise-cloud@latest/copilot/how-tos/copilot-on-github/use-copilot-agents/kick-off-a-task
[4] https://github.github.com/gh-aw/reference/copilot-cloud-agent/
[5] https://github.blog/changelog/2025-10-28-ask-copilot-coding-agent-to-make-changes-in-any-pull-request-with-copilot/
[6] https://docs.github.com/en/copilot/how-tos/use-copilot-agents/cloud-agent/use-cloud-agent-on-github
[7] https://docs.github.com/en/copilot/how-tos/use-copilot-agents/coding-agent
[8] https://docs.github.com/en/copilot/how-tos/copilot-on-github/use-copilot-agents/research-plan-iterate
[9] https://docs.github.com/en/copilot/how-tos/copilot-on-github/use-copilot-agents/copilot-code-review
[10] https://docs.github.com/en/copilot/how-tos
[11] https://github.blog/changelog/2025-07-23-agents-page-set-the-base-branch-for-github-copilot-coding-agent-tasks/
[12] https://github.blog/changelog/2026-04-01-research-plan-and-code-with-copilot-cloud-agent/
[13] https://developers.openai.com/codex/sandbox/
[14] https://github.com/orgs/community/discussions/159836
[15] https://openai.com/index/introducing-codex/
[16] https://crystl.dev/blog/agents-overwriting-files/
[17] https://www.reddit.com/r/ClaudeAI/comments/1rspl49/built_a_tool_to_stop_claude_code_agents_from/
[18] https://auravcs.com/learn/run-multiple-coding-agents-in-parallel
[19] https://www.linkedin.com/posts/me-shubham-agrawal_claudecode-aiengineering-git-activity-7479002727528824832-w4Ex
[20] https://www.mindstudio.ai/blog/claude-code-agent-view-manage-multiple-agents
[21] https://www.mindstudio.ai/blog/parallel-ai-coding-agents-git-worktrees
[22] https://note.com/ezark_company/n/n8589414176c9?hl=en
[23] https://code.claude.com/docs/en/agents
[24] https://hidekazu-konishi.com/entry/claude_code_subagents_and_orchestration_guide.html
[25] https://github.com/FlorianBruniaux/claude-code-ultimate-guide/blob/main/guide/workflows/agent-teams.md
[26] https://dev.to/sahil_kat/how-to-keep-parallel-coding-agents-from-stepping-on-each-other-e5g
[27] https://baeseokjae.github.io/posts/cursor-3-parallel-agents-tutorial-2026/
[28] https://github.com/vectara/awesome-agent-failures/blob/main/docs/case-studies/claude-code-human-as-infrastructure.md
[29] https://www.augmentcode.com/guides/git-worktrees-parallel-ai-agent-execution
[30] https://dev.to/javatarz/multi-agent-development-workflows-with-claude-code-n23
[76] https://cursor.com/docs/cloud-agent/setup
[77] https://cursor.com/changelog/3-0
[78] https://cursor.com/changelog/2-0
[79] https://cursor.com/docs/configuration/worktrees
[80] https://cursor.com/changelog/04-24-26
[81] https://cursor.com/docs/cli/changelog
[82] https://cursor.com/changelog/2-3
[83] https://cursor.com/changelog/2-4
[84] https://forum.cursor.com/t/update-on-git-worktree-support-broken-lsp-ai-features/166479
[85] https://cursor.com/blog/2-0
[86] https://cursor.com/docs/cloud-agent/capabilities.md
[87] https://cursor.com/cn/changelog/2-0
[88] https://cursor.com/docs/cloud-agent/bring-your-own-machine/choose-runtime
[89] https://forum.cursor.com/t/cursor-3-worktrees-best-of-n/156507
[90] https://forum.cursor.com/t/worktrees-in-agents-window/158956
[46] https://docs.devin.ai/work-with-devin/advanced-capabilities
[47] https://docs.devin.ai/cloud/outposts/overview
[48] https://docs.devin.ai/work-with-devin/devin-cli
[49] https://docs.devin.ai/integrations/gh
[50] https://docs.devin.ai/cli/sandbox
[51] https://docs.devin.ai/cli/handoff
[52] https://docs.devin.ai/use-cases/gallery/batch-test-coverage
[53] https://docs.devin.ai/desktop/models
[54] https://docs.devin.ai/product-guides/security-profiles
[55] https://docs.devin.ai/enterprise/deployment/overview
[56] https://docs.devin.ai/desktop/agent-command-center
[57] https://docs.devin.ai/work-with-devin/dynamic-workflows
[58] https://docs.devin.ai/federal/compliance
[59] https://docs.devin.ai/work-with-devin/devin-handoff
[60] https://docs.devin.ai/essential-guidelines/when-to-use-devin
[61] https://developers.openai.com/codex/ide/web-tasks
[62] https://developers.openai.com/codex/concepts/subagents.md
[63] https://openai.com/index/introducing-upgrades-to-codex/
[64] https://developers.openai.com/codex/developers
[65] https://developers.openai.com/codex
[66] https://openai.com/index/introducing-the-codex-app/
[67] https://openai.com/nb-NO/index/introducing-upgrades-to-codex/
[68] https://developers.openai.com/codex/resources
[69] https://developers.openai.com/codex/cli.md/
[70] https://developers.openai.com/learn/codex
[71] https://developers.openai.com/codex/guides/autofix-ci/
[72] https://developers.openai.com/codex/webmcp
[73] https://community.openai.com/t/multiple-codex-agents-in-different-branches/1358394
[74] https://community.openai.com/t/introducing-the-codex-ide-extension/1354930
[75] https://community.openai.com/t/branch-workspaces-native-git-worktree-management-for-parallel-codex-tasks/1384924
[106] https://agentiada.com/blog/github-copilot-workspace-what-happened-and-what-to-use-now.md
[107] https://www.javacodegeeks.com/2026/02/github-copilot-workspace-the-agentic-era.html
[108] https://www.qwe.edu.pl/tutorial/github-copilot-workspace-planning/
[109] https://github.blog/changelog/2025-06-18-getting-started-with-github-copilot-coding-agent-is-now-easier/
[110] https://github.blog/news-insights/product-news/github-copilot-meet-the-new-coding-agent/
[111] https://github.blog/changelog/2025-06-24-github-copilot-coding-agent-is-now-available-for-copilot-business-users/
[112] https://github.blog/changelog/2025-09-25-copilot-coding-agent-is-now-generally-available/
[113] https://github.blog/changelog/2025-08-27-copilot-coding-agent-is-now-available-in-github-enterprise-cloud-with-data-residency/
[114] https://github.blog/changelog/2025-05-19-github-copilot-coding-agent-in-public-preview/
[115] https://github.blog/news-insights/product-news/github-copilot-workspace/
[116] https://github.blog/changelog/2025-03-06-github-copilot-updates-in-visual-studio-code-february-release-v0-25-including-improvements-to-agent-mode-and-next-exit-suggestions-ga-of-custom-instructions-and-more/
[117] https://github.blog/changelog/2025-07-02-agents-page-for-copilot-coding-agent-in-public-preview/
[118] https://github.blog/changelog/2025-08-20-sunset-notice-copilot-knowledge-bases/
[119] https://github.blog/changelog/2025-09-30-start-your-new-repository-with-copilot-coding-agent/
[120] https://github.blog/changelog/2025-06-25-github-copilot-coding-agent-is-available-for-copilot-pro-users-in-public-preview/
[91] https://explainx.ai/blog/claude-code-commit-co-author-attribution-disable-guide-2026
[92] https://www.refolk.ai/guides/human-or-agent-commit-authorship
[93] https://github.com/anthropics/claude-code/issues/29999
[94] https://www.reddit.com/r/Anthropic/comments/1rv5pdr/how_do_you_stop_claude_from_adding_coauthoredby/
[95] https://github.com/github/copilot-cli/issues/975
[96] https://github.com/orgs/community/discussions/194075
[97] https://github.blog/changelog/2026-03-20-trace-any-copilot-coding-agent-commit-to-its-session-logs/
[98] https://github.com/orgs/community/discussions/179983
[99] https://forum.cursor.com/t/allow-disabling-made-with-cursor-commit-trailer/154494
[100] https://github.com/anthropics/claude-code/issues/22585
[101] https://forum.cursor.com/t/autonomous-commit-attribution/157699
[102] https://forum.cursor.com/t/autonomous-commit-attribution/157699/5
[103] https://alignify.co/blog/git-commit-attribution
[104] https://www.ssw.com.au/rules/attribute-ai-assisted-commits-with-co-authors
[105] https://mehmetbaykar.com/posts/how-to-disable-made-with-cursor-attribution/
[121] https://github.com/openai/codex/discussions/2807
[122] https://arxiv.org/html/2509.21891v1
[123] https://docs.devin.ai/cli/reference/configuration/config-file
[124] https://huggingface.co/datasets/nuprl/AgentPack
[125] https://huggingface.co/datasets/sehyunlee217/Codex-Agent-Trace
[126] https://codex.danielvaughan.com/2026/04/30/agent-fingerprints-pull-requests-codex-cli-git-hygiene/
[127] https://community.openai.com/t/does-openai-codex-add-any-default-commit-level-metadata-in-git-workflows/1371947
[128] https://thinkami.hatenablog.com/entry/2026/02/23/214607
[129] https://pigweed.googlesource.com/third_party/github/libusb/libusb/+/4655b0b913db5fd709936efacb752e2768b2768d/AGENTS.md
[130] https://alignify.co/blog/git-commit-attribution
[131] https://www.emergentmind.com/topics/agentpack
[132] https://github.com/openai/codex/issues/938
[133] https://docs.devin.ai/integrations/gh
[134] https://dafoster.net/articles/2026/04/17/ai-attribution-in-git/
[135] https://huggingface.co/justabigduck/my_saves/blob/8b9a3b2f5ccdcc2daee3e106d8843a0ae3497195/ComfyUI-master/.github/scripts/check-ai-co-authors.sh
[166] https://adventures.nodeland.dev/archive/who-is-responsible-for-ai-generated-code/
[167] https://lilting.ch/en/articles/linux-kernel-ai-coding-assistant-policy
[168] https://graphwiz.ai/ai/linux-kernel-ai-coding-guidelines/
[169] https://www.zdnet.com/article/linus-torvalds-and-maintainers-finalize-ai-policy-for-linux-kernel-developers/
[170] https://www.linuxfoundation.org/legal/generative-ai
[171] https://www.linkedin.com/posts/robert-moniz-25b82815_linuxkernel-opensource-aicode-activity-7448528228153167872--ddD
[172] https://docs.kernel.org/7.0/process/coding-assistants.html
[173] https://www.ssdnodes.com/learn/lang/pcm/ai-assisted-code-open-source-policies
[174] https://www.implicator.ai/linux-didnt-approve-ai-kernel-code-it-made-the-human-submitter-the-fuse/
[175] https://docs.kernel.org/process/coding-assistants.html
[176] https://openpolicyagent.org/docs/contrib-code
[177] https://darkfactory.dev/news/2026-08-29-evening/story-1
[178] https://www.ssdnodes.com/learn/ai-assisted-code-open-source-policies
[179] https://blog.arkin-dev.com/linux-kernel-ai-coding-guidelines-2026-04-11/
[180] https://shujisado.com/2025/07/03/how-can-open-source-projects-accept-ai-generated-code/
[151] https://www.copyright.gov/ai/
[152] https://www.copyright.gov/newsnet/2025/1060.html
[153] https://www.dlapiper.com/en-us/insights/publications/2025/03/copyrightability-of-genai-outputs-in-the-us-key-developments
[154] https://www.jonesday.com/en/insights/2025/02/copyrightability-of-ai-outputs-us-copyright-office-analyzes-human-authorship-requirement
[155] https://www.congress.gov/crs-product/LSB10922
[156] https://www.sidley.com/en/insights/newsupdates/2025/02/us-copyright-office-issues-report-on-artificial-intelligence-and-copyrightability
[157] https://www.ballardspahr.com/insights/alerts-and-articles/2025/01/copyright-office-issues-report-on-copyrightability-of-ai-content
[158] https://www.reuters.com/legal/legalindustry/us-copyright-office-issues-highly-anticipated-report-copyrightability-ai-2025-04-02/
[159] https://huggingface.co/blog/jonathanagustin/ai-copyright-analysis-2025
[160] https://www.authorsalliance.org/2025/02/20/copyrightability-and-artificial-intelligence-a-new-report-from-the-u-s-copyright-office/
[161] https://www.copyright.gov/ai/Copyright-and-Artificial-Intelligence-Part-2-Copyrightability-Report.pdf
[162] https://www.copyright.gov/ai/Copyright-and-Artificial-Intelligence-Part-3-Generative-AI-Training-Report-Pre-Publication-Version.pdf
[163] https://www.apache.org/legal/generative-tooling.html
[164] https://darkfactory.dev/news/2026-08-29-evening/story-1
[165] https://arxiv.org/html/2603.26487v1
[136] https://ai-coding-tools-review.contentwave.net/article/industry-consortium-unveils-code-attest-standard-for-ai-generated-code
[137] https://www.spinellis.gr/blog/20260702/
[138] https://arxiv.org/abs/2605.19755
[139] https://ai-coding-tools-review.contentwave.net/article/rfc-for-provenance-headers-gains-traction-among-ai-coding-tools
[140] https://arxiv.org/abs/2603.10057
[141] https://changegamer.ai/resources/ai-supply-chain-provenance.md
[142] https://lablab.ai/submissions/gu9rp4k144hc43l7kkbuthpp
[143] https://confidentialcontainers.org/blog/2025/02/17/confidential-containerscoco-and-supply-chain-levels-for-software-artifacts-slsa/
[144] https://arxiv.org/html/2606.21877v1
[145] https://www.thinkscientist.com/ai-code-provenance-the-accountability-gap-nobody-has-closed/
[146] https://mg6.net/2026-08-24-why-digital-provenance-is-becoming-critical-for-ai/
[147] https://www.augmentcode.com/guides/managing-ai-generated-code-risk
[148] https://www.augmentcode.com/guides/ai-generated-code-security-governance
[149] https://crashoverride.com/resources/knowledge-base/provenance/state-of-ai-agent-code-in-production
[150] https://facio.bot/blog/ai-agent-aibom-supply-chain-cyclonedx-ml-bom-2026
[211] https://www.hlc.com/en/publications/shaping-the-future-of-ai-security-nist-seeking-input-on-agent-identity-authorization
[212] https://www.nccoe.nist.gov/sites/default/files/2026-02/accelerating-the-adoption-of-software-and-ai-agent-identity-and-authorization-concept-paper.pdf
[213] https://cloud.google.com/blog/products/identity-security/securing-agentic-ai-whats-new-in-vpc-service-controls
[214] https://docs.cloud.google.com/iam/docs/workload-identities
[215] https://docs.cloud.google.com/mcp/ai-security-safety
[216] https://cloud.google.com/blog/products/identity-security/whats-new-in-iam-security-governance-and-runtime-defense
[217] https://www.automationshowroom.com/en/showcases/agent-permission-gateway
[218] https://nhimg.org/faq/how-should-security-teams-govern-agentic-ai-that-touches-cui-under-nist-800-171/
[219] https://docs.cloud.google.com/iam/docs/managed-workload-identity
[220] https://nhimg.org/faq/why-do-ai-agents-on-gke-complicate-workload-identity-governance/
[221] https://www.metricstream.com/blog/nists-ai-agent-standards-initiative.html
[222] https://nhimg.org/faq/how-should-organisations-review-human-and-agent-permissions-in-ai-pipelines/
[223] https://owaspai.org/docs/1_general_controls/
[224] https://cheatsheetseries.owasp.org/cheatsheets/AI_Agent_Security_Cheat_Sheet.html
[225] https://labs.cloudsecurityalliance.org/research/csa-research-note-nist-ai-agent-standards-initiative-2026032/
[181] https://docs.anthropic.com/en/release-notes/claude-code
[182] https://cursor.com/docs/cloud-agent/settings
[183] https://cursor.com/docs/cloud-agent/security-network
[184] https://cursor.com/docs/agent/security
[185] https://developers.openai.com/codex/sandbox/
[186] https://docs.anthropic.com/id/docs/claude-code/security
[187] https://docs.anthropic.com/zh-CN/docs/claude-code/sdk/sdk-headless
[188] https://cursor.com/docs/enterprise/security-hardening
[189] https://docs.anthropic.com/pt/docs/agents-and-tools/claude-code/overview
[190] https://cursor.com/docs/cloud-agent/security
[191] https://cursor.com/docs/cli/reference/configuration.md
[192] https://cursor.com/docs/enterprise
[193] https://cursor.com/docs/mcp.md
[194] https://cursor.com/docs/cli/reference/configuration
[195] https://cursor.com/docs/cli/reference/parameters
[196] https://github.com/orgs/community/discussions/167493
[197] https://docs.github.com/en/copilot/concepts/agents/cloud-agent/risks-and-mitigations
[198] https://github.blog/changelog/2026-03-13-optionally-skip-approval-for-copilot-coding-agent-actions-workflows/
[199] https://github.com/orgs/community/discussions/171743
[200] https://docs.github.com/en/copilot/how-tos/administer-copilot/manage-for-enterprise/review-audit-logs
[201] https://docs.github.com/en/enterprise-cloud@latest/copilot/reference
[202] https://docs.github.com/en/copilot/how-tos/administer-copilot/manage-for-enterprise/manage-agents
[203] https://docs.github.com/en/copilot/how-tos/use-copilot-agents/cloud-agent/configuring-agent-settings
[204] https://docs.github.com/en/enterprise-cloud@latest/copilot/how-tos/use-copilot-agents/cloud-agent/configuring-agent-settings
[205] https://docs.github.com/en/copilot/reference/enterprise-administrators
[206] https://docs.github.com/en/enterprise-cloud@latest/copilot/how-tos/copilot-on-github/use-copilot-agents/review-copilot-output
[207] https://docs.devin.ai/product-guides/security-profiles
[208] https://github.com/orgs/community/discussions/162826
[209] https://docs.github.com/en/rest/copilot/copilot-cloud-agent-management
[210] https://docs.github.com/en/enterprise-cloud@latest/copilot/how-tos/administer-copilot/manage-for-enterprise/review-audit-logs
[241] https://cursor.com/docs/agent/overview
[242] https://code.claude.com/docs/en/checkpointing
[243] https://code.claude.com/docs/en/agent-sdk/file-checkpointing
[244] https://code.claude.com/docs/en/claude_code_docs_map
[245] https://code.claude.com/docs/id/checkpointing
[246] https://code.claude.com/docs/de/agent-sdk/file-checkpointing
[247] https://code.claude.com/docs/ko/agent-sdk/file-checkpointing
[248] https://code.claude.com/docs/ru/agent-sdk/file-checkpointing
[249] https://code.claude.com/docs/zh-CN/agent-sdk/file-checkpointing
[250] https://developers.openai.com/codex/guides/slash-commands/
[251] https://code.claude.com/docs/pt/agent-sdk/file-checkpointing
[252] https://code.claude.com/docs/de/best-practices
[253] https://code.claude.com/docs/it/agent-sdk/file-checkpointing
[254] https://code.claude.com/docs/en/env-vars
[255] https://code.claude.com/docs/zh-CN/best-practices
[226] https://docs.github.com/en/copilot/how-tos/copilot-on-github/use-copilot-agents/research-plan-iterate
[227] https://docs.github.com/en/copilot/how-tos/copilot-on-github/use-copilot-agents
[228] https://docs.github.com/en/enterprise-cloud@latest/copilot/how-tos/copilot-on-github/use-copilot-agents/research-plan-iterate
[229] https://docs.github.com/en/copilot/how-tos/copilot-on-github/use-copilot-agents/manage-and-track-agents
[230] https://docs.github.com/en/copilot/how-tos/use-copilot-agents
[231] https://docs.github.com/en/copilot/how-tos/copilot-on-github/use-copilot-agents/overview
[232] https://docs.github.com/en/copilot/how-tos/copilot-cli/use-copilot-cli/agentic-code-review
[233] https://docs.github.com/en/copilot/reference
[234] https://docs.github.com/en/copilot/tutorials/stack-ai-generated-code-in-pull-requests
[235] https://docs.github.com/en/copilot/concepts/agents/cloud-agent/about-cloud-agent
[236] https://docs.github.com/en/copilot/how-tos/copilot-on-github/use-copilot-agents/copilot-code-review
[237] https://docs.github.com/en/copilot/how-tos/copilot-on-github/use-copilot-agents/review-copilot-output
[238] https://docs.github.com/en/enterprise-cloud@latest/copilot/how-tos/use-copilot-agents/request-a-code-review
[239] https://docs.github.com/es/copilot/concepts/agents/code-review
[240] https://docs.devin.ai/fr/cli/reference/commands
[256] https://www.agentscli.com/course/cursor/daily-edit-loop/review-and-restore/
[257] https://agent-undo.com/papers/agent-undo-paper.pdf
[258] https://code.visualstudio.com/learn/foundations/reviewing-and-controlling-agent-changes
[259] https://code.visualstudio.com/docs/agents/run/review-code-edits
[260] https://www.developersdigest.tech/blog/permissions-logs-rollback-ai-coding-agents
[261] https://gitlab.com/gitlab-org/gitlab/-/issues/591235
[262] https://mcpmarket.com/tools/skills/git-hunk-staging
[263] https://kiro.dev/docs/chat/autopilot/
[264] https://open-vsx.org/extension/SajanRajbanshi/ai-undo
[265] https://codingagentguide.com/posts/rollback-evidence-for-agent-generated-changes/
[266] https://www.linkedin.com/posts/jonnyzzz_v0940-mcp-steroid-give-ai-the-whole-activity-7457404629828669440-tWkP
[267] https://www.buildthisnow.com/blog/guide/mechanics/claude-code-checkpoints-rewind
[268] https://arxiv.org/html/2603.05344v1
[269] https://classic.yarnpkg.com/en/package/agent-rollback
[270] https://www.digitalapplied.com/blog/agent-rollback-checkpoint-patterns-2026-engineering-reference
[271] https://arxiv.org/html/2305.00583v3
[272] https://www.computer.org/csdl/journal/td/2025/11/11181220/2akrxcH1WG4
[273] https://www.repository.cam.ac.uk/items/2456165e-f93d-4aa0-9783-ecfe034304ec
[274] https://martin.kleppmann.com/papers/interleaving-papoc19.pdf
[275] https://www.semanticscholar.org/paper/OpSets:-Sequential-Specifications-for-Replicated-Kleppmann-Gomes/432826577bdb130544dfbc01d76d55e56c9dbccd
[276] https://mattweidner.com/assets/pdf/List_CRDT_Non_Interleaving.pdf
[277] https://www.semanticscholar.org/paper/Real-Differences-between-OT-and-CRDT-for-Co-Editors-Sun-Sun/ac3af56257a51c039650785e79e3fc1e602220c3
[278] https://dl.acm.org/doi/10.1145/3578358.3591330
[279] https://news.ycombinator.com/item?id=44955459
[280] https://dl.acm.org/doi/10.1145/3578358.3591326
[281] https://antisatori.pages.dev/%E8%87%AA%E4%BD%9C%E3%82%A2%E3%83%97%E3%83%AA/EG-walker
[282] https://stackoverflow.com/questions/45722742/logoot-crdt-interleaving-of-data-on-concurrent-edits-to-the-same-spot
[283] https://www.loro.dev/blog/crdt-richtext
[284] https://dspace.mit.edu/bitstream/handle/1721.1/147641/3555644.pdf?sequence=1&isAllowed=y
[285] https://martin.kleppmann.com/2020/07/06/crdt-hard-parts-hydra.html
[301] https://docs.cloudillo.org/crdt-design/fundamentals/internals/
[302] https://automerge.org/blog/automerge-2/
[303] https://iankduncan.com/engineering/2025-11-27-crdt-dictionary/
[304] https://www.onenoughtone.com/learn/crdts/5
[305] https://github.com/yorkie-team/yorkie/issues/3
[306] https://github.com/zxch3n/crdt-bench
[307] https://docs.yjs.dev/api/internals
[308] https://crackingwalnuts.com/post/collaborative-editor-system-design
[309] https://dev.to/viklogix/go-crdt-library-for-real-time-offline-collaborative-editing-with-state-merging-26k6
[310] https://github.com/Smona/yjs
[311] https://zylos.ai/research/2026-01-29-crdt-real-time-collaboration/
[312] https://ryanhunter.io/papers/crdt-memory.pdf
[313] https://crackingwalnuts.com/distributed-systems/crdts
[314] https://www.sharecode.in/glossary/tombstone
[315] https://arxiv.org/html/2212.02618v2
[286] https://blog.csdn.net/inksnowHL/article/details/154112650
[287] https://propelius.ai/blogs/realtime-collaborative-editing-websockets-crdts-yjs/
[288] https://discuss.yjs.dev/t/optimizing-initial-load-of-a-document-receiving-a-lot-of-updates/2206
[289] https://discuss.yjs.dev/t/scalability-of-y-websocket-server/274
[290] https://dev.to/ajith_pal/building-conflict-free-real-time-editing-with-yjs-fastify-and-websockets-3lmb
[291] https://automerge.org/automerge/automerge/sync/index.html
[292] https://crackingwalnuts.com/post/collaborative-editor-system-design
[293] https://cran.r-project.org/web/packages/automerge/automerge.pdf
[294] https://github.com/yjs/yjs/blob/main/README.md
[295] https://www.sciencedirect.com/science/article/abs/pii/S1474034616301811
[296] https://posit-dev.github.io/automerge-r/articles/sync-protocol.html
[297] https://github.com/yjs/y-websocket
[298] https://github.com/yjs/y-websocket/blob/master/README.md
[299] https://discuss.yjs.dev/t/handling-slow-applyupdate-on-the-client-side-with-update-increasing/2002
[300] https://github.com/automerge/automerge/blob/main/rust/CHANGELOG.md
[316] https://electric.ax/blog/2026/04/08/ai-agents-as-crdt-peers-with-yjs
[317] https://icml.cc/virtual/2026/78012
[318] https://arxiv.org/pdf/2510.18893
[319] https://devpost.com/software/parachute-y2705k
[320] https://x.com/ElectricSQL/status/2041916704876310675
[321] https://www.jorgealejandrodiez.com/blog/building-an-ai-agent-that-shares-a-canvas-with-humans/
[322] https://liveblocks.io/technology/collaborative-monaco-editor
[323] https://www.monperrus.net/martin/lost-update-problem-humans-ai-agents
[324] https://arxiv.org/html/2509.11826v2
[325] https://crackingwalnuts.com/post/collaborative-editor-system-design
[326] https://liveblocks.io/technology/collaborative-quill-editor
[327] https://liveblocks.io/technology/collaborative-codemirror-editor
[328] https://api.emergentmind.com/papers/2509.11826
[329] https://www.qwe.edu.pl/tutorial/replit-ai-collaborative-coding/
[330] https://www.sim.ai/blog/agent-as-yjs-peer
[346] https://blog.redlinesoft.net/posts/collaborative-gemini-3-editor/
[347] https://terminalskills.io/skills/yjs
[348] https://support.google.com/gemini/thread/361092080/can-a-gem-edit-its-own-source-document?hl=en
[349] https://electric.ax/blog/2026/04/08/ai-agents-as-crdt-peers-with-yjs
[350] https://workspace.google.com/resources/real-time-editing/
[351] https://support.google.com/docs/answer/13447609?hl=en
[352] https://x.com/ElectricSQL/status/2041916704876310675
[353] https://workspaceupdates.googleblog.com/2026/07/streamline-collaboration-in-google-docs-with-Gemini-powered-comment-workflows.html
[354] https://icml.cc/virtual/2026/78012
[355] https://www.theverge.com/tech/890996/google-workspace-gemini-ai-docs-sheets-drive
[356] https://openalternative.co/alternatives/notion
[357] https://support.google.com/docs/answer/14206696?hl=en
[358] https://www.ubergizmo.com/2026/07/google-docs-smart-review-gemini-now-reads-replies-and-edits/
[359] https://api.emergentmind.com/papers/2509.11826
[360] https://www.computerworld.com/article/4143838/google-embeds-gemini-ai-deeper-into-workspace-apps.html
[331] https://electric.ax/blog/2026/04/08/ai-agents-as-crdt-peers-with-yjs
[332] https://x.com/ElectricSQL/status/2041916704876310675
[333] https://dspace.mit.edu/bitstream/handle/1721.1/147641/3555644.pdf?sequence=1&isAllowed=y
[334] https://api.emergentmind.com/papers/2509.11826
[335] https://dl.acm.org/doi/10.1145/3555644
[336] https://arxiv.org/html/2509.11826v1
[337] https://gregmicek.com/software-coding/2026/06/12/introducing-odocs-co/
[338] https://www.the-main-thread.com/p/real-time-ai-editor-quarkus-crdt-langchain4j
[339] https://www.alphaxiv.org/abs/2608.23740
[340] https://arxiv.org/html/2409.14252v1
[341] https://evomap.ai/zh-hk/asset/sha256:105d6a134565d0da8e394420debb9fdf6e881ea8acdc3f70e9a676bdf0e28f41
[342] https://arxiv.org/pdf/2510.18893
[343] https://github.com/priyangshupal/collaborative-text-editor
[344] https://www.linkedin.com/posts/kartik-kumar-09b203300_softwareengineering-distributedsystems-buildinpublic-activity-7484840035482198016-XZkn
[345] https://biblio.vub.ac.be/vubirfiles/105781265/papoc23.pdf
[361] https://code.claude.com/docs/es/agent-teams
[362] https://code.claude.com/docs/en/agent-teams
[363] https://code.claude.com/docs/ja/agent-teams
[364] https://code.claude.com/docs/pt/agent-teams
[365] https://code.claude.com/docs/ja/agents
[366] https://code.claude.com/docs/de/agent-teams
[367] https://code.claude.com/docs/zh-CN/agent-teams
[368] https://code.claude.com/docs/it/agent-teams
[369] https://code.claude.com/docs/fr/agent-teams
[370] https://code.claude.com/docs/ru/agent-teams
[371] https://code.claude.com/docs/pt/agents
[372] https://code.claude.com/docs/ru/agents
[373] https://code.claude.com/docs/zh-TW/agent-teams
[374] https://code.claude.com/docs/id/agent-teams
[375] https://code.claude.com/docs/es/agents
[376] https://developers.openai.com/codex/cli
[377] https://developers.openai.com/codex/ide/web-tasks
[378] https://developers.openai.com/codex/cli.md/
[379] https://developers.openai.com/codex/app
[380] https://developers.openai.com/codex/features
[381] https://docs.devin.ai/work-with-devin/dynamic-workflows
[382] https://developers.openai.com/codex/guides/slash-commands/
[383] https://docs.devin.ai/zh/cli/changelog/stable
[384] https://docs.devin.ai/ja/cli/subagents
[385] https://docs.devin.ai/product-guides/automations
[386] https://docs.devin.ai/cli/extensibility/hooks/overview
[387] https://docs.devin.ai/work-with-devin/advanced-capabilities
[388] https://docs.devin.ai/desktop/agent-command-center
[389] https://docs.devin.ai/it/cli/extensibility/hooks/lifecycle-hooks
[390] https://docs.devin.ai/ja/work-with-devin/devin-session-tools
[391] https://arxiv.org/html/2605.24729v1
[392] https://vtechworks.lib.vt.edu/server/api/core/bitstreams/de77253d-6177-4db5-a273-ec928cf0502f/content
[393] https://arxiv.org/html/2603.02050v2
[394] https://www.gndclouds.earth/journal/observations-the-missing-layer-why-ai-agents-need-presence-not-just-tools
[395] https://aclanthology.org/2025.in2writing-1.6/
[396] https://arxiv.org/html/2509.11826v2
[397] https://arxiv.org/html/2509.18008v2
[398] https://zenodo.org/records/18605311
[399] https://www.alphaxiv.org/abs/2603.02050
[400] https://dl.acm.org/doi/10.1145/302979.303030
[401] https://dl.acm.org/doi/10.1007/978-3-030-90963-5_33
[402] https://futureofwork.saltlab.stanford.edu/
[403] https://www.themoonlight.io/en/review/from-human-human-collaboration-to-human-agent-collaboration-a-vision-design-philosophy-and-an-empirical-framework-for-achieving-successful-partnerships-between-humans-and-llm-agents
[404] https://www.microsoft.com/en-us/research/wp-content/uploads/2007/11/5d3b80c4dd25a-5d3b80c4dd25bRead-Write-and-Navigation-Awareness-in-Realistic-Multi-view-Collaborations.pdf.pdf
[405] https://hci.ucsd.edu/papers/fairy.pdf
[421] https://github.com/orgs/community/discussions/185521
[422] https://github.com/orgs/community/discussions/159836
[423] https://docs.github.com/en/copilot/concepts/agents/cloud-agent/risks-and-mitigations
[424] https://agentpatterns.ai/code-review/agent-proposed-merge-resolution/
[425] https://docs.github.com/en/copilot/how-tos/use-copilot-agents/cloud-agent/use-cloud-agent-on-github
[426] https://docs.github.com/en/pull-requests/reference/merge-conflicts
[427] https://docs.github.com/en/pull-requests/how-tos/merge-and-close-pull-requests/resolving-a-merge-conflict-on-github
[428] https://www.fettesps.com/github-copilot-can-now-fix-merge-conflicts-in-a-pull-request/
[429] https://www.linkedin.com/posts/jfversluis_githubcopilot-developerproductivity-activity-7452612158078582785-JXJ1
[430] https://docs.github.com/en/copilot/concepts/agents/cloud-agent/about-cloud-agent
[431] https://docs.github.com/en/enterprise-cloud@latest/copilot/tutorials/cloud-agent/get-the-best-results
[432] https://github.blog/changelog/2026-03-26-ask-copilot-to-resolve-merge-conflicts-on-pull-requests/
[433] https://github.com/orgs/community/discussions/166787
[434] https://github.blog/changelog/2025-10-28-ask-copilot-coding-agent-to-make-changes-in-any-pull-request-with-copilot/
[435] https://github.blog/changelog/2025-07-23-agents-page-set-the-base-branch-for-github-copilot-coding-agent-tasks/
[436] https://forum.cursor.com/t/multi-agent-mode-loose-worktree-and-apply-undo-apply/149072
[437] https://forum.cursor.com/t/parallel-agents-do-not-properly-merge-back-changes-when-working-on-subfolders-of-git-repo/150279
[438] https://forum.cursor.com/t/how-do-you-get-worktree-edits-to-apply/150760
[439] https://forum.cursor.com/t/cant-apply-all-changes-from-worktree-chat/145367
[440] https://forum.cursor.com/t/how-to-properly-use-worktree-and-do-tasks-in-parallel/139575
[441] https://cursor.com/docs/configuration/worktrees
[442] https://forum.cursor.com/t/agent-parallel-subagents-overwrite-dirty-worktree-destructive-git-recovery-wastes-tokens/166666
[443] https://forum.cursor.com/t/worktree-application-not-consistent/158211
[444] https://www.monperrus.net/martin/lost-update-problem-humans-ai-agents
[445] https://forum.cursor.com/t/worktree-parallel-agents-workflow/150860
[446] https://baeseokjae.github.io/posts/cursor-worktrees-parallel-agents-2026/
[447] https://forum.cursor.com/t/applying-file-changes-from-worktree-switches-chat-to-continue-locally/143983
[448] https://forum.cursor.com/t/multi-agent-fails-to-generate-project-files/139441
[449] https://forum.cursor.com/t/asking-for-updates-in-a-parallel-generation-worktree-does-not-work-as-expected/146820
[450] https://www.learncursor.dev/learn/cursor-origin/parallel-agents-merge-conflicts
[406] https://github.com/anthropics/claude-code/issues/2954
[407] https://github.com/anthropics/claude-code/issues/2572
[408] https://claudeissues.com/issue/60171-feat-edit-tool-should-auto-verify-fresh-file-content-before-applying-changes
[409] https://github.com/anthropics/claude-code/issues/40175
[410] https://github.com/anthropics/claude-code/issues
[411] https://github.com/anthropics/claude-code/issues/27048
[412] https://github.com/anthropics/claude-code/issues/43657
[413] https://github.com/anthropics/claude-code/issues/9002
[414] https://github.com/anthropics/claude-code/issues/37213
[415] https://github.com/vectara/awesome-agent-failures/blob/main/docs/case-studies/claude-code-human-as-infrastructure.md
[416] https://claudecodeguides.com/how-to-make-claude-code-stop-overwriting-my-edits/
[417] https://github.com/anthropics/claude-code/issues/7702
[418] https://github.com/anthropics/claude-code/issues/8763
[419] https://github.com/anthropics/claude-code/issues/2423
[420] https://github.com/anthropics/claude-code/issues/4897
[466] https://fortune.com/2025/07/23/ai-coding-tool-replit-wiped-database-called-it-a-catastrophic-failure/
[467] https://safeguard.sh/resources/blog/replit-agent-database-deletion-vibe-coding-2025
[468] https://companyscope.io/register/air-2026-001
[469] https://www.businessinsider.com/replit-ceo-apologizes-ai-coding-tool-delete-company-database-2025-7
[470] https://medium.com/@ismailkovvuru/replit-ai-deletes-production-database-2025-devops-security-lessons-for-aws-engineers-4984c6e7a73d
[471] https://geeksaroundglobe.com/replit-ai-agent-deleted-a-companys-live-database-and-tried-to-cover-it-up
[472] https://www.theregister.com/software/2025/07/21/vibe-coding-service-replit-deleted-production-database/719783
[473] https://joylo.ai/blog/replit-ai-agent-deleted-production-database
[474] https://datapace.ai/blog/replit-database-deletion-control-plane
[475] https://replitreview.com/replit-deletes-production-database/
[476] https://incidentdatabase.ai/cite/1152/
[477] https://developers.slashdot.org/story/25/07/21/1338204/replit-wiped-production-database-faked-data-to-cover-bugs-saastr-founder-says
[478] https://failureindex.ai/failures/replit-ai-deleted-production-database
[479] https://medium.com/@thrishank007/replit-ai-vibe-coding-deleted-live-production-database-a-wake-up-call-for-developers-ef7edae44afb
[480] https://x.com/OpenledgerHQ/status/1948695629284475108
[451] https://neon.com/guides/git-worktrees-neon-branching
[452] https://www.mindstudio.ai/blog/ai-agent-isolated-database-environments-fork-experiment
[453] https://neon.com/guides/isolated-subagents-neon-branching
[454] https://daily.dev/posts/a-branch-first-dev-loop-for-neon-link-checkout-and-env-pull-izs1vv1xz
[455] https://callsphere.ai/blog/vw7h-neon-serverless-postgres-ai-agents-2026
[456] https://www.linkedin.com/posts/evoleinik_postgres-neon-claudecode-activity-7414923830843998208-hWKM
[457] https://neon.com/faqs/postgres-database-services-ai-provisioning
[458] https://neon.com/guides
[459] https://neon.com/branching
[460] https://neon.com/branching/branching-for-agents
[461] https://neon.com/guides/opencode-neon-github-actions
[462] https://neon.com/docs/get-started/workflow-primer
[463] https://neon.com/docs/introduction/branching
[464] https://hellotars.com/tools/neon
[465] https://www.linkedin.com/pulse/we-started-building-branchable-database-now-were-backend-carlota-soto-zcnzc
[481] https://thinkami.hatenablog.com/entry/2026/02/23/214607
[482] https://github.com/openai/codex/discussions/2807
[483] https://github.com/openai/codex/blob/main/docs/exec.md
[484] https://www.gitclear.com/open_repos/openai/codex/release/rust-v0.103.0
[485] https://github.com/openai/codex/tree/main
[486] https://developers.openai.com/codex/skills/create-skill/
[487] https://developers.openai.com/codex/guides/api-key/
[488] https://developers.openai.com/codex/cli.md/
[489] https://gist.github.com/crazy4pi314/4f64dd1fd43cee299e82278fef6d9f7a
[490] https://github.com/openai/codex/issues/938
[491] https://note.com/fumieval/n/n7ac6b1ae4ae1
[492] https://github.com/openai/codex/blob/main/README.md
[493] https://news.ycombinator.com/item?id=36437925
[494] https://github.com/openai/codex/blob/main/CHANGELOG.md
[495] https://github.com/openai/codex/blob/main/codex-rs/core/gpt_5_codex_prompt.md
[496] https://docs.github.com/en/copilot/concepts/completions/code-referencing
[497] https://docs.github.com/en/enterprise-cloud@latest/copilot/concepts/completions/code-referencing
[498] https://docs.github.com/en/enterprise-cloud@latest/copilot/using-github-copilot/finding-public-code-that-matches-github-copilot-suggestions
[499] https://docs.github.com/en/copilot/how-tos/get-code-suggestions/find-matching-code
[500] https://github.blog/news-insights/product-news/introducing-code-referencing-for-github-copilot/
[501] https://docs.github.com/en/copilot/responsible-use/chat
[502] https://docs.github.com/copilot/using-github-copilot/finding-public-code-that-matches-github-copilot-suggestions
[503] https://github.blog/news-insights/product-news/code-referencing-now-generally-available-in-github-copilot-and-with-microsoft-azure-ai/
[504] https://github.blog/changelog/2026-02-18-copilot-coding-agent-supports-code-referencing/
[505] https://techcrunch.com/2023/08/03/github-copilot-now-shows-developers-when-its-code-suggestions-match-code-in-a-public-repository/
[506] https://docs.github.com/en/copilot/reference
[507] https://github.com/features/copilot
[508] https://github.blog/ai-and-ml/github-copilot/preview-referencing-public-code-in-github-copilot/
[509] https://github.com/customer-terms
[510] https://resources.github.com/ja/learn/pathways/copilot/essentials/establishing-trust-in-using-github-copilot/
