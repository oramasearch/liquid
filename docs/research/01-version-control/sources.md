<!-- Raw research source. Generated via Perplexity `perplexity_research` (web-grounded,
     multi-source) on 2026-09-01 for the liquid project. Lightly reformatted only.
     Treat as evidence, not as liquid design decisions - see README.md in this
     directory for the curated analysis and the implications drawn for liquid. -->

# State of version-control-system design, 2024–2026

## Executive assessment

The state of the art is **not one universal post-Git system**, but three distinct design fronts:

| Frontier | 2024–2026 leader(s) | What has materially improved | Remaining gap |
|---|---|---|---|
| Human-facing change management | **Jujutsu (`jj`)**, with related ideas in Sapling | Working copy represented as a commit, no exposed staging index, first-class conflicted commits, automatic descendant rebasing, repository-wide operation log and reliable undo. [web:22][web:28] | Git remains the interchange/storage layer; hosting, IDE and CI workflows still assume Git branches and commits. |
| Hyperscale repositories | **Sapling + EdenFS**, Google **Piper/CitC**, Perforce/P4VFS; upstream Git partial clone, sparse index, FSMonitor and Scalar | Fetch or materialize files lazily; avoid walking millions of filesystem entries; central or virtual workspaces expose a huge repository without cloning all contents. [web:2][web:8][web:40][web:227] | Requires server support, caches, filesystem daemons and operational infrastructure; offline behavior and tool compatibility become harder. |
| Change/merge theory | **Pijul**, structured merge tools such as **Mergiraf**, and AST-native experiments | Changes commute when independent; conflicts can remain represented in repository state; parsers distinguish edits, moves and reordered declarations that line merging confuses. [web:152][web:156][web:121] | None can generally prove that a syntactically clean merge preserves program behavior. Language coverage, malformed intermediate code and build-system files remain practical boundaries. |

Git therefore remains dominant—one 2025 survey synthesis puts its usage near 94%, versus about 5% for SVN—but increasingly acts as a **storage and interchange protocol underneath better clients**, especially `jj`. [web:376]

---

# 1. Git’s architecture and concrete limitations

## 1.1 Object model, DAG and packfiles

| Component | Concrete representation | Consequences |
|---|---|---|
| Blob | Content-addressed object containing file bytes but **no filename**. Each distinct content produces a distinct object ID. [web:48] | Deduplicates byte-identical files automatically, but an edited large file initially becomes another complete blob. Git has no inherent semantic idea that two blobs are revisions of “the same file.” |
| Tree | Snapshot of a directory: entries contain mode, filename and the object ID of a blob, subtree or submodule gitlink. [web:48][web:332] | Paths belong to snapshots, not blobs. Consequently, Git does not record rename operations as first-class history. |
| Commit | Points to one root tree, zero or more parent commits, and author/committer/message metadata. [web:48] | Parent links form a directed acyclic graph. A commit identifies the complete snapshot and ancestry, not merely a patch. Changing a parent or tree creates a new commit ID. |
| Ref | Mutable name such as `refs/heads/main` pointing to an otherwise immutable object. | Branching is cheap: creating a branch creates a ref, not a copy of project data. |
| Index | Binary staging snapshot between `HEAD` and the working tree; it is also a filesystem-stat cache and carries multiple stages during conflicts. [web:280][web:282] | Enables precise partial commits, but exposes a three-state model—`HEAD`, index and worktree—through overloaded commands such as `reset`, `restore`, `checkout`, `add` and `commit`. |
| Packfile | Many objects in one binary file, with a separate offset index; similar objects may be represented as deltas against other objects. Packing occurs during GC, repacking and network transfer. [web:51] | Excellent compression for ordinary source history, but pack generation can consume substantial CPU and memory in very large repositories. [web:361] |
| Commit graph and auxiliary indexes | Derived structures accelerate reachability, ancestry and changed-path queries; sparse index and FSMonitor reduce index/worktree traversal. [web:371][web:372] | These are important optimizations, but do not change Git’s fundamental snapshot/DAG model. |

### Why snapshots are both Git’s strength and weakness

The snapshot model is deliberately simple:

\[
C = H(\text{tree},\ \text{parents},\ \text{metadata})
\]

It gives strong integrity, cheap branching, offline history and straightforward replication. But identity is attached to **content and ancestry**, not to logical entities or changes. Consequently:

* moving a method, renaming a file and cherry-picking a logical change are reconstructed after the fact;
* rebasing necessarily changes commit identities;
* splitting a repository by path requires rebuilding snapshots and ancestry;
* a file-level or AST-level history is not an original repository object.

## 1.2 Git limitations

| Limitation | Architectural cause | Practical effect and mitigation |
|---|---|---|
| Large files and binaries | Every new content version is a new blob. Packfiles may delta similar blobs, but similarity selection is heuristic and expensive; incompressible binaries often remain effectively full versions. [web:369] | Clones, repacks and server-side pack generation become expensive. Git LFS replaces content with a small text pointer containing an object ID and size while storing bytes on another service. [web:331][web:336] That introduces a second object store, authentication, retention and backup path; GitHub also imposes plan-dependent per-file limits and does not support LFS in every product feature. [web:93] |
| Many files and monorepos | The ordinary index describes every tracked path, and status traditionally compares filesystem metadata for all those paths. [web:366] | Slow status/checkout, huge worktrees and watcher load. Sparse index, sparse checkout, FSMonitor, commit-graph files and background maintenance reduce—but do not eliminate—the work. |
| Initial clone size | Traditional distributed operation assumes a locally complete reachable object database. | `--depth` gives a **shallow clone** with truncated ancestry; `--filter=blob:none` gives a **partial clone** whose missing objects are fetched from a promisor remote; sparse checkout controls worktree paths, not by itself the object database. [web:91][web:92] Some history-sensitive operations trigger later network fetches or are restricted in shallow clones. |
| Index/staging complexity | The index is simultaneously proposed commit, worktree cache, merge-stage table and plumbing interface. | Users like `git add -p` for constructing clean commits, but frequently confuse “staged,” “modified,” `HEAD`, reset modes and conflict staging. Jujutsu’s critique is that the index behaves like an intermediate commit and can instead be modeled as an actual commit. [web:202] |
| Rename detection | Trees record deletion at one path and addition at another; Git later compares deleted and added blobs using a similarity threshold. A common default is 50%, and candidate limits may disable inexact detection. [web:57][web:59] | Results depend on thresholds and the surrounding diff. Rename-plus-heavy-edit may appear as delete/add; copies are not used by `ort`; directory rename handling is aggregated from file renames rather than recorded identity. [web:49][web:50] |
| Textual merge | Default `ort` is a three-way, mostly line-oriented merge of two heads against one merge-base tree. | It cannot understand that a method moved, a declaration was reordered or a call’s meaning changed. It may produce both false conflicts and syntactically valid but behaviorally wrong merges. |
| Reverted changes | Three-way merge mainly observes base, left and right snapshots, not the intent of every intervening commit. | A change made on both sides but reverted on one can reappear, because relative to the base the reverted side appears unchanged. Git explicitly documents this consequence. [web:50] |
| Criss-cross ancestry | There may be several “best” common ancestors. | Old `recursive`, and now `ort`, recursively merge those bases into a virtual merge base. This is pragmatic rather than a semantic account of the changes. `ort` became the default in Git 2.34; by Git 2.50, `recursive` is a synonym for `ort`. [web:50] |
| More than two complex heads | `ort` resolves two heads. | `octopus` can combine several heads only when no complex manual resolution is needed. [web:50] |
| Submodules | A tree holds a gitlink to one exact commit in an entirely separate repository, plus `.gitmodules` configuration. [web:332][web:345] | The superproject does not atomically contain the child’s objects or branch state. Users must fetch, commit and push child and parent separately; a cross-repository logical change needs multiple commits/PRs. HN reports repeatedly mention stale commits, forgotten updates, confusing status and non-transparent recursive operations. [web:61][web:63] |
| Subtrees | Child content is physically present in the parent tree; `git subtree split` synthesizes a path-filtered history for export. [web:338] | Easier checkout than submodules and allows atomic parent commits, but bidirectional synchronization repeatedly filters history; squash mode sacrifices detailed integration history. |
| History editing | Commit IDs include parent IDs and trees. Rebase or amend creates replacement commits rather than moving existing ones. | Clean linear histories are attractive, but published rebases require force-push coordination and can replay conflicts commit by commit. GitLab explicitly warns that rebase rewrites history and can create problems on shared branches. [web:224] |
| Recovery model | Branch refs are mutable; reflogs record prior local ref values, while unreachable objects survive until expiration and pruning. | Powerful but indirect recovery. Reflogs are local—not a distributed audit log—and default expiration is 90 days for ordinary entries and 30 days for entries unreachable from the current tip. [web:271] |

## 1.3 `recursive` versus `ort`

| Property | Old `recursive` | `ort` |
|---|---|---|
| Basic model | Three-way merge of two heads; recursively merged multiple bases. | Same external model and virtual-base approach. |
| Implementation | Updated index/worktree more tightly during processing; accumulated performance and correctness complexity. | Performs the merge primarily in memory and writes index/worktree as post-processing; designed for repeated merges such as rebase/cherry-pick. |
| Renames | Supported heuristic file and directory rename handling. | Reworked rename handling and caching; still detects rather than records renames, and does not use detected copies. [web:50] |
| Status in 2026 | Historical implementation; default through Git 2.33. | Default since Git 2.34; `recursive` redirects to `ort` as of Git 2.50. [web:50] |

`ort` is a major engineering improvement, not a new theory of version control: it remains a snapshot-based, textual three-way merge.

## 1.4 Why two unrelated repositories do not merge cleanly

Strictly speaking, Git **can** join them:

```bash
git fetch ../repo-b main:repo-b-main
git merge --allow-unrelated-histories repo-b-main
```

But an ordinary three-way merge requires a best common ancestor. [web:283] For unrelated DAGs there is none, so the allowed merge effectively uses the **empty tree** as the base. Both repositories therefore look as if they independently added every path. If both contain `README.md`, `src/`, `.github/` or similarly named files, Git sees add/add collisions rather than two projects to mount under separate directories. [web:279]

More fundamentally, Git does not know the intended mapping:

* Should repository B become `vendor/b/`, `services/b/`, or overlay the root?
* Do similarly named paths denote the same logical file?
* Are author identities, tags and branch names shared namespaces?
* Should two root commits remain visible, or should one be grafted beneath the other?
* How should commits that touched both retained and discarded paths be represented after a split?

A clean combination therefore normally rewrites each input first:

```bash
# In fresh clone of repo B
git filter-repo --to-subdirectory-filter project-b/

# In destination
git remote add project-b ../rewritten-b
git fetch project-b
git merge --allow-unrelated-histories project-b/main
```

This preserves a readable two-root DAG but changes every rewritten commit ID. `git filter-repo` notes that changing an ancestor changes all descendant IDs, can turn merge commits into non-merges after path pruning, does not follow renames automatically during path filtering, and may remove signatures. [web:112]

## 1.5 Merge versus rebase

| Operation | Graph result | What developers love | What developers hate |
|---|---|---|---|
| Merge | Adds a commit with both heads as parents; existing commits retain IDs. | Honest record of concurrency; safe for published history; usually resolve the integration once. | Nonlinear logs; “bubble” merges; merge commits may contain resolution changes that ordinary diffs historically displayed poorly. An HN/Reddit complaint describes merge commits as opaque. [web:75] |
| Rebase | Replays changes onto a new base, producing new commits with new parents and IDs. | Linear reviewable history; permits reorder, squash, fixup and editing before publication. | Rewrites identity, requires force pushes if published, and may surface related conflicts once per replayed commit. GitHub’s workflow requires resolve, stage, then repeatedly `rebase --continue`. [web:215] |
| `jj`-style mutable commits | Rewrites commits and automatically rebases descendants, while retaining repository-wide operation history. | Makes history editing routine and undoable rather than a dangerous exceptional mode. | Requires learning change IDs/bookmarks and translating the result back to Git’s branch/commit model. |

---

# 2. Alternatives: theory, architecture, and developer sentiment

The “love/hate” evidence below is qualitative. HN, Reddit and personal blogs reveal concrete failure modes, but are not representative surveys.

| System | Theory and architecture | What developers concretely love | What developers concretely hate |
|---|---|---|---|
| **Git** | Distributed content-addressed snapshot DAG; immutable blobs, trees and commits; mutable refs; packs and heuristic textual merges. | Ubiquity, hosting/tool support, cheap local branches, offline work, integrity and strong performance for normal source repositories. Git’s network effect is reflected in roughly 94% reported usage. [web:376] | Staging/reset mental model, conflict interruption, rebasing/force-pushing, submodules, LFS’s second storage plane and poor out-of-box huge-monorepo behavior. HN users describe submodules as requiring every parent operation to be repeated in the child and logical changes to be coordinated manually. [web:61][web:68] |
| **Darcs** | A repository is conceptually a partially ordered collection of patches. Patches can **commute** when their order does not matter; merge derives transformed patches satisfying a commuting relationship. [web:166][web:170] | Interactive patch selection and dependency-aware cherry-picking make logical changes first class rather than inferred from snapshots. Former users describe this as ahead of Git conceptually. [web:79][web:248] | Darcs 1 conflictors could expand exponentially with nested conflicts; Darcs 2 reduced but did not theoretically eliminate all conflict-fight cases. [web:169][web:172][web:178] Developers report merges taking hours or routine commands taking many minutes, damaging trust in the repository. [web:249] |
| **Pijul** | Repository state is an unordered set of changes represented in an append-oriented graph of byte/line chunks and labelled edges. Independent changes commute; conflicts are represented in the graph; dependency labels distinguish concurrent deletion, reversion and missing context. It can be viewed as a CRDT that deterministically represents conflicts. [web:152][web:157] Its category-theoretic motivation is the **pushout**: a common result receiving both concurrent edits; Pijul expands the state space to represent cases where an ordinary file has no clean pushout. [web:156] | Patch identity survives application order, so rebase ceremony and repeat conflicts can disappear; conflict resolutions are changes, and further changes may be applied before resolution. [web:151][web:159] Cherry-picking and undoing an applied patch are closer to set operations. | Small ecosystem and network effect; unfamiliar graph/conflict representation; limited hosting and IDE integration. A 2026 HN user called it a brilliant experiment but questioned history-size performance, while another said adoption is difficult without compatibility backends. [web:251][web:253] |
| **Jujutsu (`jj`)** | Commits remain the unit of storage, commonly through a Git backend, but the working copy is an automatically snapshotted commit. There is no user-visible index; conflicts are stored in commits; descendant commits are automatically rebased after edits. [web:22][web:28] A repository-wide operation log stores atomic views of refs, heads and workspaces and supports undo/restore. [web:27][web:30] | Users love editing any commit, `jj split`/`squash`, not stashing before switching work, nonblocking conflicts, revsets and undoing an entire mistaken rebase. One HN user emphasized that “everything is already committed,” making movement between changes easy. [web:196] | Automatic snapshots occur when a `jj` command runs, not continuously by default: two large edit phases without a command may become one snapshot. A watcher improves granularity but can create many snapshots. [web:204] Bookmarks, change IDs and colocated Git semantics remain learning and interoperability edges. |
| **Sapling** | Mercurial-derived client with Git support, designed around stacked changes and Meta-scale repositories; integrates with EdenFS. | More coherent history-editing UX than raw Git and proven large-monorepo workflows. | Compatibility is incomplete: an HN commenter noted that an `sl` checkout and ordinary Git were not simply interchangeable. [web:77] EdenFS support for Git mode remained marked experimental in the 2026 documentation. [web:1] |
| **EdenFS** | Virtual filesystem for repositories with millions of files. It lazily fetches file/tree data as paths are accessed and accelerates status, checkout and file-change notification; uses FUSE or NFS on Unix-like systems and ProjFS on Windows. [web:2] | Users see the whole namespace without manually maintaining sparse profiles; Meta reports this as a better experience than sparse checkout because any file can be accessed transparently. [web:8] | A daemon, mount layer, cache and server backing store add failure modes. Cold path access incurs network latency, offline access is limited to cached objects, and arbitrary tools can generate huge accidental traversals. |
| **VFS for Git / Scalar** | VFS for Git virtualizes the worktree and object access; Scalar moved the generally useful ideas upstream by configuring partial clone, sparse checkout, maintenance and performance settings. [web:98][web:227] | Makes enormous Git repositories usable without changing hosting/review identity; avoids downloading most blobs initially. | VFS for Git’s own FAQ says it is appropriate for only a very small number of extremely large repositories and users can enter bad states by over-populating the worktree or misconfiguring ProjFS. [web:232] Legacy GVFS required special server support, had limited offline operation and did not support Git filters. [web:229] Git LFS maintainers say the legacy VFS protocol is unsupported and sparse-checkout/LFS interaction remains insufficiently tested. [web:226] |
| **Fossil** | Distributed immutable artifacts stored in a single SQLite repository, with version control, wiki, tickets, forum and web UI integrated. Sync usually exchanges nearly all public project artifacts; autosync is the default social model. [web:31][web:37] | “Project in one file,” easy self-hosting, integrated issue/wiki history, atomic SQLite storage and automatic synchronization. An HN user described it as essentially Gitea in one SQLite file and praised offline editing and autosync across machines. [web:252] | Unfamiliar commands, smaller ecosystem and a workflow biased toward immediately synced commits. Critics question how autosync scales to privilege-separated, review-heavy organizations. [web:242] Individual branches are not normally pushed or pulled independently. [web:37] |
| **Perforce Helix Core** | Centralized client/server depots; workspaces map a chosen path subset; changelists are atomic submissions; streams prescribe branch flow. Binary revisions, exclusive locking, proxies, edge servers and replication are first-class. [web:304][web:309] | Game and media teams love large-binary handling, locking, granular permissions and not cloning all history. In a game-development survey, 28% used Perforce and usage exceeded 38% among organizations over 100 employees. [web:380] | Central server/admin dependence, workspace mappings, explicit checkout and complicated integrations. The same survey identified usability for non-developers as a challenge; Reddit users also complain about weak tool integrations and historical branch/merge pain. [web:302][web:312] |
| **Google Piper + CitC** | Centralized monorepo replacing a Perforce installation that no longer scaled. Piper stores the canonical repository across Google infrastructure; CitC gives a small workspace overlay while exposing the whole tree. Google largely uses trunk-based development. [web:40][web:42] | Atomic cross-project changes, one dependency version, global search/refactoring and no full clone. The repository provides a common source of truth to tens of thousands of developers. | Not a generally deployable product: the benefits depend on Google-scale storage, code search, build, ownership, review and presubmit systems. Central trunk also requires strong automated testing and coordination; project-level repository autonomy is sacrificed. |
| **Unity Version Control / Plastic SCM** | Client/server system that can also use distributed workflows; lightweight task branches, visual Branch Explorer, large-file support, locking and merge tools. Smart Locks follow a file across branches to reduce binary conflicts. [web:262][web:269] | Artists get GUIs and locking while programmers retain branching; native large binaries are preferable to bolting LFS onto Git. Unity directly integrates it with the editor. [web:260] | Smaller ecosystem and proprietary hosting/client assumptions. One HN complaint called its merge-based workflow tools “half-baked,” illustrating that advertised merge sophistication does not guarantee workflow satisfaction. [web:258] |
| **Subversion (SVN)** | Centralized repository with global revision numbers, atomic commits, versioned directories and copy-on-write “cheap copies” for branches/tags. Working copies contain local modifications but not a complete repository history. [web:320][web:321] | Simple authority model, path-level checkout and permissions, easy large central trees, and no distributed-history rewriting. Cheap-copy branches are constant-time repository operations. [web:321] | Network/server dependence for history and commit; remote rather than private-local branches; mixed-revision working copies and historically painful merge tracking. Reddit complaints cite slow commits, confusing updates and globally visible branch operations, while defenders emphasize cheap copies and post-1.5 merge tracking. [web:322][web:324] |

### Darcs versus Pijul: the important theoretical distinction

Darcs often determines whether a new patch can coexist with existing patches by **commuting and transforming patches**. Nested conflictors may recursively contain transformed conflict structure, which produced the notorious exponential behavior: each additional nested dependency can roughly double work. [web:168][web:178]

Pijul instead retains a graph of the materialized causal structure:

* vertices identify content by introducing change and position;
* labelled edges encode order, liveness/deletion and provenance;
* explicit dependencies say which context a change observed;
* applying independent changes is graph union/update and therefore order-independent;
* conflicting states remain representable rather than requiring recursively nested transformed patches. [web:152]

That moves complexity from symbolic patch transformation into a persistent graph and algorithms for outputting its live state. It is theoretically cleaner for repeated merging, but operational maturity and ecosystem—not only asymptotic theory—determine whether it can displace Git.

---

# 3. Recurring developer pain points

| Pain point | What developers want | Why existing approaches disappoint |
|---|---|---|
| Merge conflicts | Small conflicts that correspond to logical program entities, which can be postponed, shared and resolved once. | Git/SVN block the current operation and expose line hunks. Rebase may replay a related conflict several times. `jj` and Pijul improve conflict state management; Mergiraf improves conflict detection/granularity. |
| History rewriting | Freely clean up local work, but retain stable review identity and reliable undo. | Git identifies commits by ancestry, so cleanup changes hashes. Reflogs are per-ref, local and expiring. `jj` separates stable change identity and operation history from exported Git commit IDs. |
| Submodules | Atomic changes spanning components, reproducible dependency pinning and ordinary recursive commands. | A gitlink pins a separate repository commit but cannot atomically publish both repositories. Users report double commits/PRs, stale child states and difficult moves across module boundaries. [web:63][web:70] |
| LFS | Version huge assets without cloning every historical version, while preserving ordinary authentication, backup and branching behavior. | LFS solves transfer size by introducing a second content service. Pointer/file mismatch, missing uploads, hosting quotas, clean/smudge filters and migration complicate the system. HN users have described it as sufficiently clunky that they replaced it with a separate repository. [web:65] |
| Monorepo | Atomic refactors, one dependency graph, easy discovery and a single source of truth. | Git status/index/worktree and pack production scale with enormous path/object sets. Google, Meta and Microsoft therefore built virtual workspaces, filesystems and specialized services. |
| Polyrepo | Independent permissions, release cadence, ownership and small clones. | Cross-repository changes cannot be one atomic VCS transaction; dependency updates require coordinated PRs; refactors and code search cross boundaries poorly. |
| Shallow clone | Fast CI clone with recent source only. | Truncated parent history changes the semantics or availability of merge-base, blame, bisect and some fetch operations. |
| Partial clone | Preserve the commit graph while lazily obtaining omitted blobs or trees. | Better semantics than shallow clone, but requires promisor-remote support and can cause surprising network access during diff, blame, checkout or merge. |
| Sparse checkout | Materialize only selected paths. | It traditionally reduces the worktree but not history/object transfer; profiles must be curated and tools may assume absent paths are real deletions. Pairing it with partial clone and sparse index is materially better. [web:91] |
| Virtual filesystem | Make the entire repository appear present while fetching bytes on open. | Best monorepo UX when healthy, but moves complexity into a filesystem daemon, cache consistency, network availability and interception of unusual filesystem behavior. |

The strongest monorepo argument is not merely convenience: it is the ability to make an **atomic commit across arbitrary components**. HN practitioners contrast this with subrepositories, where one logical change cannot be a single pull/commit and moving a file between submodules is substantially harder than `mv`. [web:68]

---

# 4. Combining and splitting repositories

## 4.1 Git techniques

| Technique | Suitable use | History result | Main hazards |
|---|---|---|---|
| `git merge --allow-unrelated-histories` | One-time combination where roots already occupy noncolliding directories. | Preserves both original DAGs and creates a two-parent joining commit. | Empty-tree merge base causes add/add conflicts at common paths; leaves multiple roots and colliding tag/branch namespaces. [web:279] |
| `git filter-repo --to-subdirectory-filter X/` followed by unrelated merge | Preferred controlled monorepo import. | Rewrites every source snapshot under `X/`, then preserves the rewritten DAG in the destination. | All rewritten commit IDs and signatures change; consumers must reclone or carefully translate IDs. [web:106][web:112] |
| `git filter-repo --path component/ --subdirectory-filter component/` | Split one directory into a repository. | Synthesizes a history containing commits that affected the selected path. | Path filtering does not automatically follow historical renames, so old and new paths must be supplied. Mixed commits lose unrelated portions; merge topology may collapse. [web:112] |
| `git subtree add/pull/push` | Ongoing vendor or embedded-project synchronization without separate checkout state. | Full-history mode joins histories; `split` creates deterministic synthetic commits affecting the prefix; `--squash` records only aggregate imports. [web:338] | Repeated filtering can be slow; commit IDs differ between parent and extracted history; provenance depends on subtree metadata and disciplined commands. |
| Submodule | Keep ownership and histories genuinely separate. | Parent stores only the child commit ID. | No atomic cross-repository commit; recursive fetch/checkout/push and branch coordination remain explicit. |
| Grafts / replace refs | Temporarily tell Git that an existing root has another parent. | Alters history interpretation without initially rewriting objects. | Local/nonportable unless replace refs are distributed; making the graft permanent rewrites descendants. Major forges and libraries do not uniformly honor replace refs. `filter-repo` notes that GitHub, GitLab, Gerrit, JGit and libgit2 do not generally understand them. [web:112] |
| Fast-export / fast-import | Bulk conversion or custom history surgery. | Can recreate branches and commits in another repository or VCS. | Extended metadata and signatures may be lost; merges are represented relative to first parents, making sophisticated topology edits difficult. |

## 4.2 Why history grafting is intrinsically hard

Repository history has at least four coupled dimensions:

1. **Content topology:** where each file lives in every snapshot.
2. **Causal topology:** commit-parent relationships.
3. **Identity:** commit hashes, signatures and external references.
4. **Namespace:** refs, tags, issue links, CI IDs and code-review records.

Changing a historical path changes a tree ID; that changes its commit ID; because descendants include parent IDs, the change propagates through the DAG. A split also creates a semantic question for mixed commits:

```text
Commit K:
  service-a/API change
  service-b/caller update
```

After splitting, each repository receives only half of `K`. Neither new commit is the original atomic change, even if author, message and timestamp are copied. Merge commits are worse: filtering one side may remove an entire parent line, turning a merge into a normal commit. `git filter-repo` explicitly documents this topology degeneration. [web:112]

## 4.3 Which systems handle combination/splitting better?

| System or model | Advantage | Qualification |
|---|---|---|
| Mercurial `convert` with filemaps and splicemaps | Can filter paths and explicitly assign one or two replacement parents during conversion. [web:182][web:192] | Still a history rewrite with changed changeset IDs; semantic decisions remain manual. |
| Darcs / Pijul | First-class patches can be selected and applied to another repository if their dependencies and contexts are present; independent Pijul changes retain identity across application order. | Better for moving *changes*, not automatically for converting decades of snapshot history or resolving namespace/path collisions. |
| Perforce/SVN centralized trees | Moving or copying repository paths can retain server-side ancestry without every developer recloning an object DAG. SVN versions directories and records cheap copies. [web:320] | Moving content between distinct servers still needs conversion; cross-server atomicity is absent. |
| Piper/CitC | Avoids most splitting pressure by making one global repository cheap to view selectively. | This solves access and scale, not export of an independent, history-preserving repository. |
| Jujutsu | Its virtual root is a common ancestor of all commits, eliminating Git’s special unborn/unrelated-root state. [web:22] | With a Git backend, content placement and exported commit topology still obey Git constraints; it does not infer how two project roots should be mounted. |
| Fossil | Git fast-export import and artifact reconstruction make one-shot ingestion possible. | Its normal synchronization model exchanges one project’s artifact universe; it is not a transparent cross-repository composition system. |

No mainstream system makes arbitrary split/combine operations lossless because the requested operation itself can destroy atomicity and change causal meaning. Patch-based systems make **logical change transfer** better; virtual-monorepo systems make splitting less necessary.

---

# 5. Semantic and structured version control

## 5.1 Structural diff and merge systems

| System | Representation and algorithm | What it solves | Limitations |
|---|---|---|---|
| **Difftastic** | Parses both revisions with tree-sitter, simplifies the concrete syntax trees, and treats structural alignment as a graph/shortest-path problem using Dijkstra’s algorithm. [web:122][web:133] | Cleaner review of expression edits, delimiter changes and formatting-insensitive modifications. | Two-way diff only, not a merge engine. Parse errors and unsupported grammars degrade results; syntax is not program semantics. |
| **Diffsitter** | Tree-sitter parsing followed by longest-common-subsequence matching over syntax-tree leaves. [web:126][web:134] | Removes some whitespace noise while retaining a relatively simple diff model. | Leaf LCS captures less move and parent/child structure than full tree matching. |
| **GumTree** | Greedy top-down matching of large unique isomorphic subtrees, bottom-up matching of containers with many common descendants, followed by recovery mappings and insert/delete/update/move operations. [web:293] | Fine-grained AST edit scripts and explicit move detection; useful as a building block for refactoring-aware tools. | Matching is heuristic; ambiguity among repeated structures can create incorrect correspondences. Usually language/parser dependent. |
| **SemanticMerge** | Language-dependent three-way merge: parse base and contributors into code trees, combine parser knowledge with Plastic’s refactoring-aware merge. [web:286][web:289] | Can merge edits to methods that were moved or reorganized and avoid conflicts caused only by location changes. | Proprietary/product-dependent and limited by supported language front ends; “semantic” primarily means syntax/refactoring aware, not behavioral equivalence. |
| **Mergiraf** | Parses base/left/right with tree-sitter; runs GumTree matchings for all three pairs; creates cross-revision node equivalence classes; converts trees to parent-child-successor triples; merges triple sets; detects delete/modify and duplicate-signature conflicts; falls back locally to line-based `diff3`. [web:121] | Generic syntax-aware Git merge driver. Handles independent insertions, reordered declarations and many move/edit cases while preserving ordinary conflict markers where needed. | Parser failure falls back to text. Matching remains heuristic; cross-file moves and semantic dependencies are difficult; structured merge costs more, so it first tries an ordinary line merge. |
| **Spork and related research** | Move-enabled structured three-way merge, often using GumTree-derived AST mappings and structured tree amalgamation. [web:300] | Demonstrates that move-aware structured merge can reduce conflicts in language-specific settings. | Fully resolved ASTs may reject temporarily invalid code; language-specific implementations are expensive to maintain. |

### Mergiraf’s core merge operation

For each syntax tree it emits triples:

\[
(p,c,s)
\]

meaning that under parent \(p\), node \(s\) immediately follows child \(c\). Base triples inconsistent with either edited side are removed, and the remaining relations are reconstructed into a tree. This can merge two insertions into the same class or declaration list even when a textual merge sees overlapping lines. Delete/modify checks are essential: a naïve union could silently discard an edit made to an element deleted on the other side. [web:121]

## 5.2 OT and CRDTs for source code

| Model | Mechanics | Relevance to source control | Technical limits |
|---|---|---|---|
| Operational Transformation | Concurrent operations are transformed against one another so each replica applies context-adjusted operations in a compatible order. | Proven for real-time text editors; can support live pair programming and centralized collaborative IDEs. | Correct transformation functions become difficult as operation types and participants grow; ordinary Git commits do not retain the operation stream needed after the fact. |
| Sequence CRDT | Characters or text spans receive stable identifiers; insert/delete operations are designed to commute and replicas converge under eventual delivery. | Decentralized/offline collaborative editing and mergeable operation logs. | Convergent text may still be syntactically invalid or semantically contradictory. Tombstones/identifiers add metadata and ordinary compiler tooling still consumes files. |
| Tree/AST CRDT | Replicate nodes and structural operations rather than characters. CoAST, for example, models the program AST as an operation-based CRDT with logical timestamps and a shared change log. [web:136] | Preserves more syntax, permits entity-aware concurrent edits, and can combine real-time collaboration with branch/merge. Microsoft research demonstrated conflict-free AST merge integrated with an eventual-consistency model. [web:138][web:141] | Tree moves, delete-versus-edit, name binding, type dependencies and temporarily malformed programs need explicit policies. Convergence does not imply that the merged program is correct. |
| Pijul’s graph CRDT interpretation | Changes add graph vertices/edges or relabel known edges; dependencies preserve causal context; replicas converge to the same graph, potentially containing represented conflicts. [web:152] | Bridges distributed version history and CRDT ideas without requiring simultaneous editing. | Primarily structural/textual causality; it does not automatically resolve semantic conflicts such as incompatible API changes. |

OT/CRDT systems answer **“Will replicas converge?”** Traditional merge additionally asks **“Which edits conflict?”** Program analysis asks the harder question **“Does the merged program preserve intended behavior?”** These are separate properties.

## 5.3 Unison’s content-addressed codebase

Unison is the most radical deployed example because the canonical unit is not a text file:

| Property | Unison model | Consequence |
|---|---|---|
| Definition identity | A term or type is identified by the hash of its normalized syntax tree; dependencies are replaced by their hashes before hashing. [web:346][web:349] | Identity follows implementation rather than file path or human name. |
| Names | Names are separately stored metadata mapping to definition hashes. [web:346] | Renaming does not rewrite callers or change the definition’s hash. |
| Dependencies | References point directly to immutable definition hashes. | Multiple versions can coexist, and the usual filename/package-name diamond dependency conflict largely disappears. [web:346] |
| Storage | The codebase is a database of terms and types indexed by hashes and names; textual scratch files are an editing projection rather than canonical storage. [web:357] | Builds, caching, search, deployment and versioning can operate at definition granularity. |
| Update semantics | Editing creates a new definition with a new hash; namespace metadata can be updated to use it. | Old code remains addressable and dependents do not silently change. |
| Scope limitation | Requires adopting the Unison language and codebase manager. | It is not a drop-in VCS for arbitrary Java/C++/JavaScript repositories, text assets or build configurations. |

This model removes several problems that Git can only heuristically reconstruct—rename identity, definition-level dependencies and same-file edit interference—but does so by integrating the language, compiler, package manager and versioned store. That is the essential trade-off: **the more semantics version control knows, the less language-agnostic it becomes**.

---

# Overall conclusions

| Question | 2026 answer |
|---|---|
| Is Git’s object model obsolete? | No. It remains an exceptionally effective immutable snapshot and interchange format. Its weaknesses appear mainly in user-visible history editing, very large worktrees, large binaries and language-blind merging. |
| Is `ort` a semantic merger? | No. It is a faster and generally more accurate implementation of Git’s two-head, three-way snapshot merge, with sophisticated heuristic rename handling. |
| Why are submodules persistently painful? | They compose repositories only by pinning a child commit. They do not compose transactions, publication, branches or working-copy state. |
| Does LFS solve large files completely? | No. It moves large bytes out of Git, solving core clone/storage pressure while introducing a separate object service and lifecycle. |
| What is the most practical post-Git improvement? | **Jujutsu over a Git backend**: it preserves ecosystem compatibility while replacing staging, conflict blocking, fragile history editing and per-ref reflog recovery with a cleaner change/operation model. |
| What is best at hyperscale? | Purpose-built virtual workspaces: **EdenFS/Sapling**, **Piper/CitC**, or Perforce/P4VFS. Upstream Git’s partial clone, sparse index, FSMonitor and Scalar are the portable compromise. |
| What is the deepest alternative theory? | **Pijul’s commutative change graph**, informed by categorical pushouts and CRDT reasoning, improves on Darcs’s dynamically transformed conflictors. |
| What currently reduces merge pain most? | First-class conflict state (`jj`, Pijul) plus syntax-aware merge drivers such as **Mergiraf**. Neither removes semantic/API conflicts. |
| Which system combines/splits arbitrary repositories cleanly? | None. Git `filter-repo` plus subdirectory rewriting is the most practical migration technique; Mercurial splicemaps offer more explicit parent surgery; patch systems transfer logical changes better. No system can preserve atomic meaning after an inherently lossy split. |
| Likely direction | Git increasingly becomes a backend/protocol; clients maintain richer change and operation identity, monorepos use lazy virtual workspaces, and parser-aware diff/merge is applied selectively with line-based fallback. |

Citations:
[46] https://git-scm.com/docs/git-merge
[47] https://git-scm.com/book/en/v2
[48] https://git-scm.com/book/en/v2/Git-Internals-Git-Objects
[49] https://github.com/git/git/blob/4d34dffbdd2226034df9d7612f1e221b7143fc6a/Documentation/technical/directory-rename-detection.txt
[50] https://github.com/gitster/git-htmldocs/blob/master/merge-strategies.adoc
[51] https://git-scm.com/book/id/v2/Git-Internals-Packfiles
[52] https://git-scm.tw/book/en/v2/Git-Internals-Maintenance-and-Data-Recovery
[53] https://git-scm.com/book/ms/v2/Getting-Started-About-Version-Control
[54] https://git-scm.com/docs/git
[55] https://chromium.googlesource.com/external/github.com/git/git/+/refs/tags/v2.32.3/merge-ort.c
[56] https://www.yuanbian-lab.com/article/gitbook/git-internals/Maintenance-and-Data-Recovery.html
[57] https://libgit2.org/docs/reference/main/merge/git_merge_options.html
[58] https://stackoverflow.com/questions/6013261/disable-git-rename-detection/35672618
[59] https://libgit2.org/docs/reference/v1.9.1/merge/git_merge_options.html
[60] https://chromium.googlesource.com/external/github.com/git/git/+/HEAD/Documentation
[91] https://www.mslinn.com/git/600-partial-clone.html
[92] https://git-scm.com/docs/git-clone
[93] https://docs.github.com/en/repositories/working-with-files/managing-large-files/about-git-large-file-storage
[94] https://github.com/git-lfs/git-lfs/blob/main/docs/man/git-lfs-faq.adoc
[95] https://docs.github.com/en/enterprise-server@3.21/repositories/working-with-files/managing-large-files/about-git-large-file-storage
[96] https://docs.github.com/en/enterprise-server@3.18/repositories/working-with-files/managing-large-files/about-git-large-file-storage
[97] https://docs.github.com/en/enterprise-server@3.14/repositories/working-with-files/managing-large-files/about-git-large-file-storage
[98] https://github.com/microsoft/VFSForGit
[99] https://github.com/Reedbeta/git-partial-submodule
[100] http://www.kouhan.cn/en/article/00000979
[101] https://github.com/git-lfs/git-lfs/issues/3811
[102] https://www.reedbeta.com/made/git-partial-submodule/
[103] https://docs.github.com/en/repositories/working-with-files/managing-large-files
[104] https://stackoverflow.com/questions/6238590/set-git-submodule-to-shallow-clone-sparse-checkout
[105] https://docs.github.com/en/repositories/working-with-files/managing-large-files/about-large-files-on-github
[166] https://darcs.net/Theory/MergersDocumentation
[167] https://en.wikibooks.org/wiki/Understanding_Darcs/Patch_theory_and_conflicts
[168] https://lists.osuosl.org/pipermail/darcs-users/2007-October/011291.html
[169] https://erickow.com/koweycode/2011/04/why-darcs-users-care-about-consistency.html
[170] http://urchin.earth.li/~ian/theory.pdf
[171] https://en.wikipedia.org/wiki/Darcs
[172] https://darcs.net/FAQ/Performance
[173] https://www.cs.tufts.edu/comp/150FP/archive/david-roundy/darcs_print.pdf
[174] https://urchin.earth.li/~ian/conflictors/paper-2006-10-30.pdf
[175] https://www.cs.tufts.edu/~nr/cs257/archive/jason-dagit/tmr-darcs.pdf
[176] https://darcs.net/Theory/PekkaPatchTheory
[177] https://en.wikibooks.org/wiki/Understanding_Darcs/Print_Version
[178] https://darcs.net/FAQ/ConflictsDarcs1
[179] https://en.wikibooks.org/wiki/Understanding_Darcs/Patch_theory
[180] https://www.cs.tufts.edu/comp/150GIT/archive/iago-abal/mfes_darcs.pdf
[151] https://pijul.org/manual/conflicts.html
[152] https://pijul.org/manual/theory.html
[153] https://pijul.org/
[154] https://playbooks.com/skills/plurigrid/asi/pijul
[155] https://news.ycombinator.com/item?id=22021309
[156] https://pijul.org/model/
[157] https://pijul.org/manual/why_pijul.html
[158] https://pijul.org/posts/2020-12-19-partials
[159] https://news.ycombinator.com/item?id=19083748
[160] https://www.everand.com/book/888053756/Pijul-Theory-and-Practice-of-Patch-Based-Version-Control-The-Complete-Guide-for-Developers-and-Engineers
[161] https://av.tib.eu/media/52238
[162] https://github.com/bitemyapp/Pijul/blob/master/theory.md
[163] https://lobste.rs/s/9ga9cw/theory_pijul_version_control_system
[164] https://news.ycombinator.com/item?id=47672941
[165] https://pijul.org/posts/2020-11-07-towards-1.0/
[16] https://docs.jj-vcs.dev/latest/working-copy/
[17] https://v5.chriskrycho.com/essays/jj-init/
[18] https://github.com/jj-vcs/jj
[19] https://docs.jj-vcs.dev/latest/conflicts/
[20] https://docs.jj-vcs.dev/latest/technical/conflicts/
[21] https://docs.jj-vcs.dev/v0.13.0/conflicts/
[22] https://docs.jj-vcs.dev/latest/git-comparison/
[23] https://www.git-tower.com/blog/jujutsu
[24] https://kubamartin.com/posts/introduction-to-the-jujutsu-vcs/
[25] https://gist.github.com/ruvnet/60e5749c934077c7040ab32b542539d0
[26] https://deepwiki.com/jj-vcs/jj/3-git-integration
[27] https://docs.jj-vcs.dev/v0.15.1/operation-log/
[28] https://docs.jj-vcs.dev/latest/technical/architecture/
[29] https://man.archlinux.org/man/extra/jujutsu/jj-operation-log.1.en
[30] https://docs.jj-vcs.dev/latest/operation-log/
[1] https://sapling-scm.com/docs/git/git_support_modes/
[2] https://github.com/facebook/sapling/blob/main/eden/fs/docs/Overview.md
[3] https://github.com/DurhamG/eden/blob/main/eden/fs/docs/Overview.md
[4] https://sapling-scm.com/docs/scale/overview/
[5] https://github.com/kmancini/eden/blob/main/eden/fs/docs/Overview.md
[6] https://github.com/facebook/sapling/blob/main/eden/fs/docs/Data_Model.md
[7] https://github.com/facebook/sapling
[8] https://developers.facebook.com/blog/post/2022/11/15/meta-developers-workflow-exploring-tools-used-to-code/
[9] https://github.com/facebook/sapling/blob/main/eden/fs/docs/Process_State.md
[10] https://dev.to/moozzyk/what-it-is-like-to-work-in-metas-facebooks-monorepo-15pm/comments
[11] https://sapling-scm.com/docs/commands/clone/
[12] https://engineering.fb.com/2022/11/15/open-source/sapling-source-control-scalable/
[13] https://www.linkedin.com/in/andrew-milas-53577a203
[14] https://dev.to/daniel15/comment/2i1ob
[15] https://github.com/facebook/sapling/blob/main/eden/fs/docs/InodeStorage.md
[31] https://fossil-scm.org/home/help/sync
[32] https://fossil-scm.org/home/doc/tip/www/branching.wiki
[33] https://fossil-scm.org/home/doc/d650eea3/www/branching.wiki
[34] https://fossil-scm.org/home/doc/8c6aca20/www/branching.wiki
[35] https://fossil-scm.org/home/doc/169a10dd/www/branching.wiki
[36] https://fossil-scm.org/home/doc/8ddeb17d/www/branching.wiki
[37] https://fossil-scm.org/home/doc/1d276f7b/www/gitusers.md
[38] https://docs.unity.com/ugs/en-us/manual/devops/manual/release-notes/9
[39] https://fossil-scm.org/home/doc/tip/www/private.wiki
[40] https://alastairreid.github.io/RelatedWork/papers/potvin:cacm:2016/
[41] https://fossil-scm.org/home/doc/tip/www/fossil-v-git.wiki
[42] https://github.com/florian/reading-notes/blob/master/papers/003_Why_Google_Stores_Billions_of_Lines_of_Code_in_a_Single_Repository.md
[43] https://fossil-scm.org/home/doc/tip/www/sync.wiki
[44] https://www.uncommonengineer.com/docs/books/papers/Google3/
[45] https://docs.unity3d.com/2022.1/Documentation//Manual/PlasticSCMPlugin.html
[61] https://news.ycombinator.com/item?id=31792396
[62] https://news.ycombinator.com/item?id=31792303
[63] https://news.ycombinator.com/item?id=25536257
[64] https://news.ycombinator.com/item?id=33614617
[65] https://news.ycombinator.com/item?id=48574148
[66] https://news.ycombinator.com/item?id=42291833
[67] https://news.ycombinator.com/item?id=29188863
[68] https://news.ycombinator.com/item?id=37291765
[69] https://news.ycombinator.com/item?id=34359736
[70] https://news.ycombinator.com/item?id=20569348
[71] https://news.ycombinator.com/item?id=26164790
[72] https://news.ycombinator.com/item?id=23377012
[73] https://news.ycombinator.com/item?id=23483436
[74] https://news.ycombinator.com/item?id=37292494
[75] https://www.reddit.com/r/programming/comments/1q3t6j/10_things_i_hate_about_git/
[76] https://news.ycombinator.com/item?id=47672941
[77] https://news.ycombinator.com/item?id=33612410
[78] https://news.ycombinator.com/item?id=39452543
[79] https://news.ycombinator.com/item?id=37094599
[80] https://news.ycombinator.com/item?id=38590080
[81] https://news.ycombinator.com/item?id=42488112
[82] https://news.ycombinator.com/item?id=43022059
[83] https://news.ycombinator.com/item?id=19070172
[84] https://news.ycombinator.com/item?id=10470281
[85] https://news.ycombinator.com/item?id=18235798
[86] https://news.ycombinator.com/item?id=42351436
[87] https://news.ycombinator.com/item?id=47734950
[88] https://news.ycombinator.com/item?id=19801884
[89] https://news.ycombinator.com/item?id=43021515
[90] https://news.ycombinator.com/item?id=31634560
[106] https://alex-v.medium.com/how-to-merge-git-repositories-and-keep-history-587f973f06a0
[107] https://kokkonisd.github.io/2021/06/23/import-git-history
[108] https://gist.github.com/gitcos/2219596b4445b9b26dc8406bcbd0fd3c
[109] https://codimd.web.cern.ch/s/DpXow5c0r
[110] https://medium.com/allbound-fireside/how-to-merge-git-branches-and-preserve-history-672ce8b72a1c
[111] https://dev.to/itminds/how-to-split-and-merge-multiple-git-repositories-and-keep-the-history-2938
[112] https://github.com/newren/git-filter-repo/blob/main/Documentation/git-filter-repo.txt
[113] https://stackoverflow.com/questions/5760331/git-subtree-is-not-retaining-history-so-i-cannot-push-subtree-changes-how-can-i
[114] https://stackoverflow.com/questions/72898186/merge-forked-git-repos-and-keep-both-file-systems
[115] https://gist.github.com/achauve/deb076b2e2eaacd83b7d28de941fe91b
[116] https://gfscott.com/blog/merge-git-repos-and-keep-commit-history/
[117] https://stackoverflow.com/questions/26928299/why-does-git-subtree-always-process-every-commit/27155706
[118] https://github.com/newren/git-filter-repo
[119] https://gist.github.com/JalilArfaoui/99eece7ab1ce0aaffb32929744e7fd8f
[120] https://www.commandlinux.com/man-page/man1/git-subtree.1.html
[121] https://mergiraf.org/architecture.html
[122] https://github.com/wilfred/difftastic
[123] https://www.emergentmind.com/topics/mergiraf
[124] https://sparkles-docs.pages.dev/research/diff-review/mergiraf
[125] https://difftastic.wilfred.me.uk/
[126] https://github.com/Wilfred/difftastic/wiki/Structural-Diffs
[127] https://arxiv.org/html/2507.19687v1
[128] https://arxiv.org/pdf/2608.11345v2
[129] https://sourceforge.net/projects/difftastic.mirror/
[130] https://jpk.io/dev-tools/difftastic-structural-diff-review/
[131] https://lwn.net/Articles/1042355/
[132] https://www.reddit.com/r/rust/comments/1tg0kg0/weave_structural_merging_what_i_learned_shifting/
[133] https://difftastic.wilfred.me.uk/parsing.html
[134] https://difftastic.wilfred.me.uk/tree_diffing.html
[135] https://www.x-cmd.com/install/difftastic/
[136] http://soft.vub.ac.be/Publications/2022/vub-tr-soft-22-17.pdf
[137] https://dl.acm.org/doi/abs/10.1145/3555644
[138] https://www.microsoft.com/en-us/research/wp-content/uploads/2015/02/paper.pdf
[139] https://arxiv.org/html/2212.02618v2
[140] https://www.sciencedirect.com/science/article/abs/pii/S1474034616301811
[141] https://www.microsoft.com/en-us/research/publication/towards-ast-based-collaborative-editing/
[142] https://dl.acm.org/doi/10.1145/3578358.3591330
[143] https://www.ijraset.com/research-paper/real-time-web-ide-collaborative-code-editors
[144] https://www.theregister.com/software/2019/09/26/haskell-erlang-and-frank-walk-into-a-bar-and-begin-new-project-to-work-in-unison/557744
[145] https://www.ijraset.com/best-journal/a-comprehensive-study-on-realtime-web-ide-collaborative-code-editors
[146] https://dl.acm.org/doi/10.1145/2957276.2957310
[147] https://dl.acm.org/doi/10.1145/3392825
[148] https://dl.acm.org/doi/10.1145/2034691.2034717
[149] https://arxiv.org/html/2605.19373
[150] https://byteiota.com/unison-1-0-content-addressed-code-hits-production/
[316] https://www.reddit.com/r/programming/comments/amuck/my_company_is_looking_to_start_using_version/
[317] https://www.reddit.com/r/programming/comments/e4k8yi/the_evolution_of_version_control_system_vcs/
[318] https://ftp.sjtu.edu.cn/sites/ftp.openpkg.org/sources/CPY/subversion/svn-book-20050525.pdf
[319] https://www.reddit.com/r/programming/comments/8iob5/obsolete_scm_killing_interest_in_tinycc/
[320] https://en.wikipedia.org/wiki/Apache_Subversion
[321] https://www.reddit.com/r/programming/comments/3x9c5e/is_subversion_as_bad_as_comparisons_with_git_tell/
[322] https://www.reddit.com/r/programming/comments/79h2k/subversion_sucks_get_over_it/
[323] https://www.reddit.com/r/programming/comments/7iupi/why_subversion_does_not_suck/
[324] https://www.reddit.com/r/programming/comments/1l48ny/svn_branching_as_easy_as_git/
[325] https://news.ycombinator.com/item?id=18215588
[326] http://www.r-5.org/files/books/computers/dev-teams/toolchains/Mike_Mason-Pragmatic_Version_Control_Using_Subversion-EN.pdf
[327] https://www.reddit.com/r/programming/comments/cj4vv/distributed_is_not_a_superset_of_centralized/
[328] https://www.reddit.com/r/programming/comments/1ju1f1g/20_years_of_git/
[329] https://news.ycombinator.com/item?id=9144748
[330] https://www.reddit.com/r/programming/comments/45988n/the_curse_of_the_gifted_programmer_esrs_email_to/
[301] https://ftp.perforce.com/perforce/r19.1/doc/manuals/overview.pdf
[302] https://www.reddit.com/r/programming/comments/2n4qo/dear_perforce_fuck_you/
[303] https://www.perforce.com/sites/default/files/pdfs/whats-new-helix-core-2024-1.pdf
[304] https://ftp.perforce.com/perforce/r21.1/doc/manuals/overview.pdf
[305] https://www.perforce.com/products/helix-core
[306] https://www.reddit.com/r/programming/comments/e6jyh/a_history_of_version_control_from_rcs_to_the_dvcs/
[307] https://www.reddit.com/r/programming/comments/qvxzbz/p4_to_git_converter_written_in_c_that_runs_100x/
[308] https://www.reddit.com/r/programming/comments/e4k8yi/the_evolution_of_version_control_system_vcs/
[309] https://help.perforce.com/helix-core/server-apps/p4sag/2024.1/Content/P4SAG/intro.architecture.html
[310] https://ftp.perforce.com/perforce/r19.2/doc/manuals/overview.pdf
[311] https://www.perforce.com/products/helix-core/free-version-control
[312] https://www.reddit.com/r/programming/comments/71btyi/perforce_vs_svn_vs_git_vs_hg_for_gamedev/
[313] https://www.perforce.com/sites/default/files/pdfs/whats-new-helix-core.pdf
[314] https://www.reddit.com/r/programming/comments/7l2jp/perl_5_now_uses_git_for_version_control/
[315] https://help.perforce.com/helix-core/server-apps/p4sag/2023.1/Content/P4SAG/intro.architecture.html
[256] https://learn.unity.com/course/getting-started-with-plastic-scm/tutorial/branch-and-merge-workstreams-with-plastic-scm?version=2022.1
[257] https://learn.unity.com/course/getting-started-with-plastic-scm/tutorial/branch-and-merge-workstreams-with-plastic-scm-1?version=2022.1
[258] https://news.ycombinator.com/item?id=36778634
[259] https://docs.unity3d.com/2021.1/Documentation/Manual/PlasticSCMPlugin.html
[260] https://docs.unity3d.com/2022.1/Documentation//Manual/PlasticSCMPlugin.html
[261] https://docs.unity.com/en-us/unity-version-control/release-notes/11
[262] https://unity.com/features/version-control
[263] https://docs.unity.com/en-us/unity-version-control/merge-changes-landing
[264] https://docs.unity3d.com/2021.2/Documentation/Manual/PlasticSCMAccessRemoteProjects.html
[265] https://docs.unity3d.com/cn/2021.3/Manual/PlasticSCMAccessRemoteProjects.html
[266] https://news.ycombinator.com/item?id=31762245
[267] https://www.reddit.com/r/programming/comments/6xcut/
[268] https://en.wikipedia.org/wiki/Unity_Version_Control
[269] https://docs.unity.com/ugs/en-us/manual/devops/manual/unity-version-control?m=0
[270] https://docs.unity.com/en-us/unity-version-control/release-notes/9
[196] https://news.ycombinator.com/item?id=47713460
[197] https://man.archlinux.org/man/extra/jujutsu/jj-workspace-update-stale.1.en
[198] https://man.archlinux.org/man/jj-commit.1.en
[199] https://man.archlinux.org/man/extra/jujutsu/jj-commit.1.en
[200] https://man.archlinux.org/man/jj-git-colocation-disable.1.en
[201] https://github.com/jj-vcs/jj/issues/5224
[202] https://docs.jj-vcs.dev/latest/git-comparison/
[203] https://news.ycombinator.com/item?id=43021961
[204] https://news.ycombinator.com/item?id=43021515
[205] https://jkellz.dev/blog/20250321-how-i-use-jujutsu/
[206] https://news.ycombinator.com/item?id=43022323
[207] https://www.reddit.com/r/programming/comments/1nwa2m0/should_i_switch_from_git_to_jujutsu/
[208] https://docs.jj-vcs.dev/latest/tutorial/
[209] https://www.reddit.com/r/programming/comments/1tnmtm5/jujutsu/
[210] https://www.reddit.com/r/programming/comments/1hentx8/tech_notes_the_jujutsu_version_control_system/
[241] https://news.ycombinator.com/item?id=10737131
[242] https://news.ycombinator.com/item?id=27433751
[243] https://news.ycombinator.com/item?id=19070172
[244] https://news.ycombinator.com/item?id=12680311
[245] https://news.ycombinator.com/item?id=16807818
[246] https://news.ycombinator.com/item?id=809493
[247] https://news.ycombinator.com/item?id=39452543
[248] https://news.ycombinator.com/item?id=18235798
[249] https://news.ycombinator.com/item?id=43022059
[250] https://news.ycombinator.com/item?id=47672941
[251] https://news.ycombinator.com/item?id=47734747
[252] https://news.ycombinator.com/item?id=30815693
[253] https://news.ycombinator.com/item?id=47734718
[254] https://news.ycombinator.com/item?id=25538145
[255] https://news.ycombinator.com/item?id=37094599
[211] https://survey.stackoverflow.co/
[212] https://github.com/StackExchange/Survey
[213] https://repositorio.ufpe.br/bitstream/123456789/54306/1/DISSERTA%C3%87%C3%83O%20Marcela%20Bandeira%20Cunha.pdf
[214] https://seg.nju.edu.cn/uploadPublication/copyright/123401376954.pdf
[215] https://docs.github.com/en/get-started/using-git/resolving-merge-conflicts-after-a-git-rebase
[216] https://stackoverflow.com/questions/64625361/the-most-painless-way-of-resolving-conflicts-when-merging-feature-branches
[217] https://survey.stackoverflow.co/2025/
[218] https://docs.github.com/en/enterprise-cloud@latest/get-started/using-git/resolving-merge-conflicts-after-a-git-rebase
[219] https://link.springer.com/article/10.1007/s10515-017-0227-0
[220] https://github.com/orgs/community/discussions/190710
[221] https://stackoverflow.com/questions/161813/how-do-i-resolve-merge-conflicts-in-a-git-repository?rq=1
[222] https://docs.github.com/en/pull-requests/reference/merge-conflicts
[223] https://survey.stackoverflow.co/2024/
[224] https://microfluidics.utoronto.ca/gitlab/help/topics/git/git_rebase.md
[225] https://coderefinery.github.io/git-intro/conflicts/
[271] https://git-scm.com/docs/git-reflog
[272] https://www.kernel.org/pub/software/scm/git/docs/git-gc.html
[273] https://linux.die.net/man/1/git-reflog
[274] https://www.atlassian.com/git/tutorials/rewriting-history/git-reflog
[275] https://www.atlassian.com/git/tutorials/git-gc
[276] https://gist.github.com/zaqmor/254b48c9dda7ce2478f50fd74ae8d347
[277] https://gist.github.com/koseki/b67f960c842313f8330c
[278] https://initialcommit.com/blog/git-gc
[279] https://stackoverflow.com/questions/61734449/what-happens-in-git-when-you-merge-unrelated-histories/61735274
[280] https://www.augmentedmind.de/2024/04/07/ultimate-git-guide-for-developers/
[281] https://stackoverflow.com/questions/78536297/what-is-the-difference-between-gc-pruneexpire-gc-reflogexpire-and-gc-reflogexpi
[282] https://itplaybook.org/courses/git-github-actions/chapter001/the-three-trees.html
[283] https://www.kernel.org/pub/software/scm/git/docs/git-merge-base.html
[284] https://www.atlassian.com/git/tutorials/git-prune
[285] https://babulal.com.np/blogs/git-github-course-in-depth.html
[286] https://www.semanticmerge.com/users-sub/documentation/intro-guide/semanticmerge-intro-guide
[287] https://www.semanticmerge.com/documentation
[288] https://sel.ist.osaka-u.ac.jp/lab-db/Mthesis/archive/175/175_2.pdf
[289] https://www.semanticmerge.com/semanticmerge-intro-guide
[290] https://sdl.ist.osaka-u.ac.jp/pman/pman3.cgi?DOWNLOAD=506
[291] https://sdl.ist.osaka-u.ac.jp/pman/pman3.cgi?DOWNLOAD=496
[292] https://stackoverflow.com/tags/semantic-merge/info
[293] https://courses.cs.vt.edu/cs5704/spring20/lecture_notes/5704-12-GumTree.pdf
[294] https://courses.cs.vt.edu/cs6704/spring17/slides_by_students/CS6704_gumtree_Kijin_AN_Feb15.pdf
[295] https://www.c-sharpcorner.com/UploadFile/761867/semanticmerge-and-C-Sharp/
[296] https://www.semanticmerge.com/files/documents/semanticmerge-intro-guide.pdf
[297] https://rionscode.wordpress.com/2013/10/15/semantic-merge-emerges-from-beta/
[298] https://www.semanticmerge.com/blog-sub/index
[299] https://cir.nii.ac.jp/crid/1390850475731067264
[300] https://kth.diva-portal.org/smash/get/diva2:1471148/FULLTEXT01.pdf
[226] https://github.com/git-lfs/git-lfs/discussions/5643
[227] https://github.blog/open-source/git/the-story-of-scalar/
[228] https://devblogs.microsoft.com/devops/introducing-scalar/
[229] https://devblogs.microsoft.com/bharry/more-on-gvfs/
[230] https://github.com/microsoft/git/blob/f90e3eda8744efad2aba71b4a8feaf9a3d9a546d/contrib/scalar/docs/getting-started.md
[231] https://github.com/desktop/dugite-native/issues/45
[232] https://github.com/microsoft/VFSForGit/blob/master/docs/faq.md
[233] https://www.reddit.com/r/programming/comments/5rtlk0/git_virtual_file_system_from_microsoft/
[234] https://github.com/microsoft/git/blob/HEAD/contrib/scalar/docs/index.md
[235] https://courses.axx.no/rdgitcourse/tier-1-basics/47-scalar.html
[236] https://gitlab.com/gitlab-org/gitaly/-/issues/1116
[237] https://github.com/microsoft/scalar/blob/main/Readme.md
[238] https://www.tdcommons.org/cgi/viewcontent.cgi?article=8242&context=dpubs_series
[239] https://runbook.fyi/Git/monorepos-and-scaling/
[240] https://dev.to/elmuerte/comment/3j7a
[181] https://stackoverflow.com/questions/20266534/restore-history-to-split-repository
[182] https://wiki.mercurial-scm.org/ConvertExtensionImplementation
[183] https://stackoverflow.com/questions/257767/can-i-split-a-mercurial-repository
[184] https://stackoverflow.com/questions/6889750/how-to-adjust-history-after-merge-of-two-unrelated-mercurial-repositories
[185] https://wiki.mercurial-scm.org/MergingUnrelatedRepositories
[186] https://changelog.complete.org/archives/528-whose-distributed-vcs-is-the-most-distributed
[187] https://heavymetaldev.com/hg-repo-conversion
[188] https://homes.cs.washington.edu/~mernst/advice/version-control.html
[189] https://stackoverflow.com/questions/29938367/merge-two-mercurial-repositories-without-breaking-file-history
[190] https://hgbook.red-bean.com/read/migrating-to-mercurial.html
[191] https://blog.janestreet.com/dropping-history-with-mercurial/
[192] https://stackoverflow.com/questions/16628372/in-mercurial-how-to-combine-unrelated-repositories-leaving-only-one-root
[193] https://stackoverflow.com/questions/54924374/create-new-mercurial-repository-cloning-since-revision
[194] https://stackoverflow.com/questions/111031/comparison-between-centralized-and-distributed-version-control-systems
[195] https://stackoverflow.com/questions/4602452/how-to-convert-an-existing-mercurial-repository-to-use-subrepositories-and-keep
[346] https://www.unison-lang.org/docs/the-big-idea/
[347] https://www.unison-lang.org/docs/tour/_big-technical-idea/
[348] https://www.unison-lang.org/docs/language-reference/hashes/
[349] https://www.unison-lang.org/docs/tour/
[350] https://composure.systems/catalog/related-work/unison
[351] https://forum.holochain.org/t/holochain-unison-language-content-addressed-code/7955
[352] https://news.ycombinator.com/item?id=22156370
[353] https://softwaremill.com/trying-out-unison-part-1-code-as-hashes/
[354] https://www.unison-lang.org/blog/heres-whats-been-happening-with-unison/
[355] https://lwn.net/Articles/978955/
[356] https://renato.athaydes.com/posts/unison-revolution.html
[357] https://www.unison-lang.org/docs/language-reference/name-resolution-and-the-environment/
[358] https://byteiota.com/unison-1-0-content-addressed-code-hits-production/
[359] https://rosettalens.com/s/ko/docs-the-big-idea-simplifying-distributed-programming
[360] https://github.com/unisonweb/unison/blob/trunk/docs/repoformats/v2.markdown
[376] https://rhodecode.com/blog/156/version-control-systems-popularity-in-2025
[377] https://newsletter.pragmaticengineer.com/p/the-pragmatic-engineer-2025-survey
[378] https://moldstud.com/articles/p-version-control-systems-a-comprehensive-comparison-of-top-tools-for-effective-software-development
[379] https://commandlinux.com/statistics/git-usage-statistics-on-linux-vs-other-platforms/
[380] https://www.diversion.dev/blog/version-control-for-game-devs-survey-results
[381] https://survey.stackoverflow.co/
[382] https://www.jetbrains.com.cn/en-us/help/idea/version-control-integration.html
[383] https://www.jetbrains.com/help/idea/enabling-version-control.html
[384] https://intellij-support.jetbrains.com/hc/en-us/sections/201620065-Version-Control
[385] https://www.jetbrains.com/help/idea/settings-version-control-git.html
[386] https://www.jetbrains.com/guide/tags/vcs/
[387] https://www.jetbrains.com/ja-jp/help/go/settings-version-control-git.html
[388] https://tms-outsource.com/blog/posts/what-is-github-desktop/
[389] https://www.jetbrains.com/idea/whatsnew/2024-1/
[390] https://www.jetbrains.com/help/idea/version-control-integration.html
[331] https://github.com/git-lfs/git-lfs/blob/main/docs/spec.md
[332] https://git.github.io/htmldocs/gitsubmodules.html
[333] https://git-scm.com/docs/git-submodule/2.12.5
[334] https://stackoverflow.com/questions/66955714/where-is-the-gitlink-entry-located-for-git-submodules
[335] https://deepwiki.com/metmuseum/openaccess/3.2-git-lfs-architecture
[336] https://docs.github.com/en/enterprise-server@3.18/repositories/working-with-files/managing-large-files/about-git-large-file-storage
[337] https://docs.github.com/en/enterprise-server@3.21/repositories/working-with-files/managing-large-files/about-git-large-file-storage
[338] https://man.archlinux.org/man/git-subtree.1
[339] https://stackoverflow.com/questions/29530200/what-is-the-storage-mechanism-behind-git-large-file-storage/29531702
[340] https://docs.gitlab.com/development/lfs/
[341] https://docs.gitlab.com/topics/git/lfs/
[342] https://git-scm.com/docs/git-submodule
[343] https://www.oreilly.com/library/view/version-control-with/9780596158187/ch15s04.html
[344] https://deepwiki.com/git-lfs/git-lfs/1.1-key-concepts
[345] https://git-scm.com/book/en/v2/Git-Tools-Submodules
[361] https://repository.prace-ri.eu/git/help/user/project/repository/monorepos/index.md
[362] https://kindatechnical.com/git-version-control/performance-troubleshooting-large-repos.html
[363] https://www.kenmuse.com/blog/tips-for-large-monorepos-on-github/
[364] https://www.querystack.tech/post/monorepo-tooling-and-git-sparse-checkouts-scaling-developer-workflows-without-the-pain-80b30b
[365] https://stackoverflow.com/questions/17888604/git-with-large-files
[366] https://www.atlassian.com/git/tutorials/monorepos
[367] https://www.grizzlypeaksoftware.com/library/git-performance-optimization-for-large-repositories-nxxd5xrq
[368] https://qiita.com/taquaki-satwo/items/bf8393dc9707639bea4f
[369] https://lwn.net/Articles/774125/
[370] https://www.atlassian.com/git/tutorials/big-repositories
[371] https://blog.gitbutler.com/git-tips-3-really-large-repositories
[372] https://github.blog/engineering/infrastructure/improve-git-monorepo-performance-with-a-file-system-monitor/
[373] https://www.gitflow.dev/blog/monorepo-git-techniques
[374] https://stackoverflow.com/questions/984707/what-are-the-file-limits-in-git-number-and-size
[375] https://stackoverflow.com/questions/14064605/does-github-handle-large-repositories-well/14066705
