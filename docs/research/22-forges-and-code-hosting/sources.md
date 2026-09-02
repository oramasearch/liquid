<!-- Raw research source. Generated via Perplexity `perplexity_research` (web-grounded,
     multi-source) on 2026-09-01 for the liquid project. Lightly reformatted only.
     Treat as evidence, not as liquid design decisions - see README.md in this
     directory for the curated analysis and the implications drawn for liquid. -->

# Research brief: code hosting platforms and software forges, 2024–2026

**Bottom line:** GitHub is the dominant hosted developer platform, not merely a Git server. Its advantage is the integrated network—repositories, review, automation, packages, cloud environments, security, AI, identity and third-party apps. Its recurring weaknesses follow from the same design: repository- and branch-centric abstractions, metered cloud services, centralized availability, proprietary collaboration metadata and growing Copilot/Microsoft coupling. GitLab is the closest integrated alternative; Forgejo/Gitea are the practical self-hosted alternatives; Gerrit and Phabricator/Phorge offer fundamentally stronger change-based review; and Graphite, Sapling/ReviewStack and similar tools retrofit that workflow onto GitHub.

## 1. What GitHub actually consists of

GitHub has at least four layers: Git storage and browsing; collaboration metadata; execution and artifact services; and an identity/integration/commercial ecosystem.

| Component | What it actually does | Technical/product significance |
|---|---|---|
| Repositories | Hosts Git objects, refs, branches, tags, files, commit history, forks, releases and optional wikis, accessed through HTTPS/SSH, web, CLI and APIs. A repository contains the files and their revision history, but GitHub adds substantial non-Git metadata around it. [web:186] | The Git data is comparatively portable; the surrounding database records are not. |
| Pull requests | A proposal to merge the difference between a head branch and base branch. It combines discussion, commits, changed files, checks, mergeability and merge controls. | GitHub’s central collaboration abstraction is **branch-based**, not a persistent logical “change.” |
| Code review | Inline and file comments, review summaries, approve/request-changes states, suggested edits, CODEOWNERS and required reviewers. Reviewers mark files “Viewed” and track progress in the Files changed view. [web:77] | Excellent for ordinary PRs; increasingly awkward when a branch is repeatedly rebased or contains a large/stacked change. |
| Issues and Discussions | Issues track bugs, requests and work; labels, assignees, milestones, templates, sub-issues and relationships add structure. Discussions provide forum-style community communication. | Close to code and convenient for open source, but not a complete enterprise planning system. |
| Projects | Table, board and roadmap views over issues, pull requests and draft items, with custom fields and automation. [web:31] | A lightweight work-management layer, distinct from each repository’s issue list. |
| Actions | Event-driven automation described in YAML under `.github/workflows`; jobs run on GitHub-hosted or self-hosted runners and use reusable “actions.” | CI/CD, release automation, bots, scheduled jobs and policy checks; also a major metered cost and lock-in surface. |
| Packages | Package and container registries associated with users and organizations. | Keeps source, CI and built artifacts under one permission/billing system. |
| Codespaces | Cloud development VMs/containers configured by the repository, commonly through `devcontainer.json`, and accessed through VS Code or a browser. | Converts the repository into a reproducible development environment, but introduces compute and persistent-storage charges. |
| Code search / “Blackbird” | GitHub’s purpose-built Rust search engine, supporting literal, regex, path, language, symbol and repository qualifiers. [web:188] | Far faster and more expressive than classic GitHub search, but it is an index rather than an exhaustive scan. |
| Copilot | IDE completion and chat plus GitHub-integrated review, coding agents, repository assistance and agent-created branches/PRs. The cloud agent can plan and make changes on a branch before opening a PR. [web:184] | Increasingly part of the platform rather than a stand-alone editor plug-in; separately licensed and partly usage-metered. |
| Organizations and teams | Identity grouping, team nesting, repository assignment and roles: Read, Triage, Write, Maintain and Admin, plus enterprise/custom roles. [web:182] | Provides the main authorization boundary above repositories. Enterprise adds SAML/SCIM, managed users, audit and policy controls. |
| Branch protection | Older per-branch controls such as required reviews, checks, linear history, signed commits, restricted pushes and no force-push/deletion. [web:187] | Protects important refs but becomes cumbersome across many repositories. |
| Rulesets | Named, inspectable policies targeting branches, tags, pushes or multiple repositories, with bypass actors and organization-level application. [web:185] | The more scalable successor/complement to individual branch-protection rules. |
| Apps, OAuth apps and webhooks | GitHub Apps receive granular permissions and short-lived installation tokens; webhooks deliver signed event payloads; OAuth apps act more directly for users. | Foundation for bots, CI providers, security scanners and external workflow systems. |
| Marketplace | Distribution and billing/discovery channel for GitHub Apps and reusable Actions. GitHub Apps may be private or published through Marketplace. [web:31] | Produces strong ecosystem/network effects—and switching costs. |
| Security layer | Dependabot, dependency graph, secret scanning/push protection, code scanning/CodeQL, advisories, attestations and security campaigns. | Some capabilities are free for public repositories; advanced private-repository capabilities are paid add-ons. |
| APIs and command line | REST, GraphQL, GitHub CLI and audit/event APIs. | Make most metadata programmable, but an API-exportable object is not necessarily losslessly importable elsewhere. |

### Scale

GitHub’s 2025 Octoverse—covering September 2024 through August 2025—reported **more than 180 million developers, 630 million repositories, 395 million public repositories, 518.7 million merged pull requests and 11.5 billion free Actions minutes consumed by public projects**. GitHub also said 81.5% of contributions occurred in private repositories, showing that it is simultaneously the main public open-source network and a very large private enterprise platform. [web:451]

### Pricing model as of September 2, 2026

| Plan/product | Current list model | Important included usage or overage |
|---|---|---|
| GitHub Free | $0; unlimited public and private repositories | Personal/organization Free includes 2,000 Actions minutes; 500 MB Actions/artifact allowance; 500 MB Packages storage; personal Free includes 120 Codespaces core-hours and 15 GB-month. Public repositories receive standard hosted Actions without minute charges. [web:442] |
| GitHub Pro | **$4/month** for an individual | 3,000 Actions minutes, 1 GB Actions storage, 2 GB Packages, 180 Codespaces core-hours and 20 GB-month. [web:440][web:442] |
| GitHub Team | **$4/user/month** | 3,000 Actions minutes and 2 GB each for Actions artifacts and Packages, plus private-repository rules, code owners, required/multiple reviewers and team controls. [web:31][web:442] |
| GitHub Enterprise Cloud | **From $21/user/month** | 50,000 Actions minutes, 50 GB Actions/artifact storage, 50 GB Packages, enterprise identity, audit, compliance and organization-wide policy. Add-ons are separate. [web:31][web:449] |
| Actions overage | Metered per runner-minute and storage | Representative September 2026 rates include Linux x64 2-core at $0.006/minute, ARM 2-core at $0.005, Windows 2-core at $0.010 and macOS at $0.062; larger runners cost more. [web:123] |
| Codespaces | Metered compute plus storage | Starts at **$0.18/hour** for a two-core machine and **$0.07/GB-month**; larger machines scale to $2.88/hour for 32 cores. [web:35] |
| Packages | Included quota, then storage and egress | Allowances vary by plan; for example, Pro has 2 GB storage and 10 GB monthly transfer, while Enterprise has 50 GB and 100 GB. [web:442][web:446] |
| Git LFS | Included storage/download bandwidth, then metered | Free/Pro include 10 GiB each; Team and Enterprise include 250 GiB each. Overage is $0.07/GiB-month stored and $0.0875/GiB downloaded. [web:61][web:68] |
| Copilot | Separate seat subscription plus AI-credit/usage model | Free is limited; Pro $10/month, Pro+ $39 and Max $100. Business is $19/user/month and Enterprise $39/user/month. [web:232] |
| Security products | Per-seat enterprise add-ons or negotiated contracts | Secret Protection, Code Security/Advanced Security and premium support are not simply part of the $4 Team price. |

The headline seat price is therefore a poor estimate of total enterprise cost. CI minutes, storage, LFS downloads, Codespaces, Copilot, security products and premium support can exceed the repository-hosting charge.

---

## 2. Concrete complaints about GitHub

These are not claims that GitHub is unusable; they are recurrent, technically identifiable failure modes.

| Complaint | Concrete basis | Assessment in 2026 |
|---|---|---|
| Large-diff review | GitHub imposes diff-generation/display limits, and users report Files changed views omitting or failing to load files and comments. GitHub’s 2026 redesign specifically advertised reduced memory usage and up to 67% better interaction performance on large PRs—confirmation that large-review performance was a real product problem. [web:76][web:78][web:79] | Improved, not eliminated. Generated files, renames, broad refactors and thousands of changed files are still better reviewed locally or split beforehand. |
| Stacked PRs | Historically every layer required its own branch and PR, with child PRs targeting parent branches. Fixing a lower layer means rebasing and force-pushing all higher branches. GitHub’s own current workflow describes exactly that cascading operation. [web:83] | GitHub added native `gh stack` support by 2026, materially improving mechanics, but the server model remains multiple dependent branch PRs rather than first-class commit/change dependencies. |
| Review after rebase | Rebase changes commit IDs and often line ancestry. Conversations may become “outdated,” commit-linked comments can disappear from the normal review context, and “changes since last review” can become unreliable. Developers specifically report inability to reconstruct the originally reviewed context after stacked rebases. [web:88][web:90] | Fundamental consequence of attaching review presentation to moving branch/commit comparisons rather than to stable change IDs and explicit patch-set versions. |
| Files changed loses context | GitHub collapses unchanged regions, virtualizes large file lists and associates comments with diff positions. Rewrites, line movement or base-branch changes can detach comments or make the displayed “current” diff differ from what the reviewer approved. | The UI is optimized for the latest aggregate branch delta, not for comparing patch set 4 with patch set 5 of one logical change. |
| Actions expense | Private CI is billed by time, OS and machine size; parallel matrices multiply minutes. macOS is especially expensive, while a continually occupied standard Linux runner costs hundreds of dollars per month at retail minute rates. [web:123] | Convenient for bursty CI; often poor economics for sustained utilization, where self-hosted or specialized CI can be cheaper. |
| Actions cold starts and queues | Hosted jobs require a runner to be allocated and an image/environment prepared; dependency downloads recur unless caching succeeds. Start time and queueing are outside the repository owner’s control. | Most painful for many short jobs: provisioning and checkout/setup can dominate useful execution. Persistent self-hosted runners trade latency for operational work and security risk. |
| Actions caching | Cache keys are immutable and branch-scoped in important ways; misses make a nominally cached build cold. The default cache allowance is 10 GB per repository, with least-recently-used eviction; caches unused for a week were historically evicted, and stuck segment downloads required a timeout mechanism. [web:348][web:353][web:402] | Better controls and purchasable capacity arrived by 2026, but monorepos and large build matrices can churn the shared cache and pay both time and storage costs. |
| Notifications | Watching repositories, team mentions, review requests, issue participation and automated bots create a single high-volume inbox. Users report disabling nearly everything except direct mentions and still finding the inbox hard to triage by urgency or relationship. [web:201][web:202] | Filters and saved views help, but GitHub notifications remain event-oriented rather than a full personal work queue. |
| Issues versus Jira/Linear | GitHub Issues/Projects are excellent for code-adjacent work but have shallower workflow enforcement, portfolio management and analytics. Reported gaps include native velocity, cumulative-flow, cycle-time and burndown reporting, and cross-project portfolio rollups. [web:196][web:200] | GitHub added sub-issues, dependencies, custom fields and roadmaps, so “only labels and open/closed” is outdated. Jira remains stronger for deeply governed workflows/reporting; Linear is generally more opinionated and interaction-focused. |
| Search incompleteness | Blackbird is indexed, not exhaustive: vendored/generated code, binaries, non-UTF-8 files, files over 350 KiB and very large repositories may be excluded; long lines are truncated and only default branches are searched. [web:167][web:179] | Extremely good interactive code search, but it cannot be treated as proof that text does not exist anywhere in all refs or artifacts. |
| Monorepo pain | GitHub recommends keeping repositories below 1 GB ideally and below 5 GB strongly; files over 100 MiB are blocked without LFS. Very large repositories may not be search-indexed. [web:173][web:167] | Git itself and GitHub can host substantial monorepos, but large ones need sparse/partial checkout, path ownership, selective CI, external build caches and careful automation. A single 10 GB default Actions cache and repository-wide event model are weak boundaries for hundreds of components. |
| LFS quotas and download cost | Large binaries are stored outside normal Git objects, but every download consumes bandwidth; Free/Pro receive only 10 GiB monthly storage and bandwidth, with metered overage. [web:61][web:68] | Fine for modest assets; costly for popular binary releases, game/media repositories or repeated CI downloads. Object storage or a dedicated artifact registry may be preferable. |
| Vendor lock-in | Git history clones cleanly, but PR review state, issue event history, Projects, Actions secrets/runs, Apps, Packages, policies and identity mappings do not. Even GitHub-to-GitHub migrations omit numerous categories. [web:413] | The lock-in is primarily **workflow and metadata**, not source code. |
| Outages and centralization | Git operations may continue from local clones, but PRs, Actions, merge queues, releases and APIs depend on GitHub. An August 2026 incident delayed PR-triggered workflows and affected mergeability and merge-button availability. [web:241] | Local Git protects source history, not the operational control plane. Organizations should retain mirrors, release artifacts and a deployment path that does not require GitHub to be online. |
| Microsoft/Copilot pressure | After GitHub’s CEO departure, leadership began reporting into Microsoft’s CoreAI organization rather than through a replacement GitHub CEO. Community complaints included inability to block Copilot-created issues/PRs or disable Copilot review, and GitHub withdrew misplaced Copilot “tips” from PR comments after backlash. [web:242][web:246][web:252] | The concern is loss of product neutrality: the forge, editor ecosystem, cloud, identity and AI assistant are increasingly one Microsoft commercial surface. Organizations also raise privacy, training, model-choice, licensing and procurement concerns. |

---

## 3. Competitors and what each does better

| Platform | Model | What it does better than GitHub | Main trade-off |
|---|---|---|---|
| **GitLab** | GitLab.com SaaS, Dedicated, or Self-Managed; repositories, merge requests, CI/CD, registries, planning, security and deployment in one application. | The strongest “single application” story: CI/CD runners, container/package registries, environments, security and release/deployment management have historically been more coherent and self-hosting is first-class. GitLab explicitly positions itself as one application for the DevSecOps lifecycle. [web:17][web:29] | Large and operationally demanding when self-hosted; UI and licensing tiers are complex. In 2026, SaaS Premium is $29/user/month, Ultimate is custom-priced, extra compute is $10/1,000 minutes and storage $5/month per 10 GiB. [web:124][web:135] |
| **Gitea** | MIT-licensed, lightweight, self-hosted all-in-one forge, generally deployed as a Go binary plus database and optional runner. | Much lower resource and administration footprint; unlimited local users/repos; Git hosting, PRs, issues, projects, packages and Actions-compatible CI without per-seat hosting fees. [web:211][web:214] | Less enterprise governance, search, analytics and hosted-service ecosystem; operators own availability, upgrades, backups and abuse controls. |
| **Forgejo** | Community-governed fork of Gitea, now independently developed and copyleft. | Similar lightweight deployment, with a stronger community-governance and software-freedom commitment. It began after Gitea assets/governance moved toward a company/open-core direction; Forgejo became a hard fork in 2024 so it could reduce technical debt and evolve independently. [web:20][web:26] | Smaller ecosystem and enterprise support market than GitHub/GitLab; some post-fork compatibility divergence. |
| **Codeberg** | Public nonprofit forge operated by Codeberg e.V., based on Forgejo. | Noncommercial, privacy-oriented hosting for free/open-source projects, with Pages, CI, packages, LFS and Weblate; funded by donations and membership rather than ads or seat extraction. [web:323][web:325] | Not a free private SaaS for arbitrary commercial workloads; fair-use limits and nonprofit resources constrain scale and support. |
| **sourcehut / sr.ht** | Hosted and self-hostable suite of small services: Git/Mercurial, mailing lists, builds, tickets, pages and documentation. | Email-native patch review, low-bandwidth/minimal UX, composability, no tracking or advertising, no AI, and every feature works without JavaScript. Hosted plans are €4/€8/€12 monthly. [web:271][web:273][web:282] | Deliberately unfamiliar to PR-centric teams; fewer rich visual planning and enterprise-governance features. |
| **Bitbucket Cloud/Data Center** | Atlassian-hosted or self-managed Git forge with Pipelines and tight Jira integration. | Best fit where Jira is already the planning authority: commits, branches, builds and deployments can be attached directly to Jira work. Pipelines is built into Bitbucket Cloud. [web:216][web:313] | Smaller public developer network and marketplace than GitHub; Cloud pricing/limits and Data Center administration add complexity. Cloud is free for five users, with paid tiers and separate Pipeline/LFS allowances. [web:304] |
| **Azure DevOps** | Modular Microsoft suite: Repos, Boards, Pipelines, Artifacts and Test Plans, cloud or Azure DevOps Server. | More mature enterprise work-item tracking, manual testing, release pipelines and Azure/Microsoft integration. Pipelines can build code from GitHub, Bitbucket or other Git hosts, so the services are independently adoptable. [web:301][web:221] | Older, denser UX and a less compelling public/open-source social network. Pricing combines user access with parallel-job capacity; one hosted job with 1,800 monthly minutes and one self-hosted job are included. [web:302] |
| **Gerrit** | Self-hosted Git server and review system organized around changes and patch sets. | First-class pre-submit, one-change/one-review workflow; explicit patch-set history; strong ACLs, labels/votes, submit requirements and dependency handling. A stable `Change-Id` associates amended commits with the same review. [web:46][web:49] | Steeper Git workflow and UI learning curve; weaker product-management/social ecosystem. |
| **Phabricator / Phorge** | Suite including Differential review, Diffusion repository browsing, Maniphest tasks and Arcanist CLI. Phabricator is unmaintained; Phorge is its active community fork. [web:151] | Differential treats a review as a revision that can be repeatedly updated and landed, rather than merely a branch comparison. It supports pre-commit review, rich policy and stacked revisions; Diffusion separates repository browsing from Differential review. [web:57][web:387] | Phabricator itself should not be newly deployed; Phorge has a much smaller community and modern integration ecosystem. |
| **Graphite** | Commercial review UI, CLI and merge queue layered over GitHub. | Best polished GitHub-compatible stacked-PR workflow: creates/manages dependent branches, submits whole stacks, navigates them and restacks after lower changes. [web:1][web:9] | Does not remove GitHub dependence or branch/PR representation; adds another vendor, UI, permissions and bill. |
| **Sapling + ReviewStack** | Meta-originated scalable SCM plus a separate GitHub review UI. | Sapling makes commit stacks editable—amend, split, fold, absorb and automatically restack descendants. ReviewStack displays only the intended commit for each overlapping GitHub PR and provides stack navigation. [web:376][web:377][web:388] | Smaller external ecosystem; GitHub remains the server of record, and normal GitHub views may show overlapping/awkward diffs. |
| **Meta’s internal model** | Sapling/Mononoke-style source control plus Phabricator-derived review and internal CI; Differential is review, while Diffusion is repository browsing. | Small dependent diffs are normal, each diff has its own review lifecycle, and engineers navigate and submit stacks rather than manually maintaining one feature branch per review. Meta describes Phabricator as displaying the test plan and enabling navigation through a stack of diffs. [web:386] | Highly customized for Meta’s infrastructure and not available externally as a turnkey hosted forge. |

### Why Google and Android use Gerrit

Gerrit originated to serve the Android Open Source Project and added access-control features that did not fit its Rietveld predecessor. It remains integral to public Git-based projects such as Android and Chromium. [web:256][web:258]

The reason is workflow, not simply historical preference:

1. Every proposed commit enters a staging namespace such as `refs/for/main`.
2. A stable `Change-Id` identifies the logical change.
3. Amending and uploading the commit creates patch set 2, 3, and so forth without creating a new review.
4. Review votes and automated verification apply according to explicit copy/outdating rules.
5. Only the approved latest patch set is submitted to the target branch. [web:46][web:53][web:270]

That is well suited to large, policy-heavy codebases where every commit needs ownership review, automated verification and a clear audit trail. Google’s predominant internal review system is now Critique; Gerrit remains especially important for public-facing projects rather than being the sole review system for all Google code. [web:266]

---

## 4. Stacked diffs and change-based review

### Why critics call the PR-as-branch model “wrong”

“Wrong” is value-laden, but the technical criticism is precise: **a branch is a mutable line of development, while a reviewable change is a logical unit with versions and dependencies. They are not the same object.**

With a conventional GitHub PR:

- Identity comes from `head branch → base branch`.
- The displayed patch is recomputed from current branch tips and their merge base.
- Amend/rebase rewrites commit hashes.
- Changing a lower dependency changes every descendant branch.
- Merging a lower PR changes the base of the next PR.
- Force pushes can invalidate review context and checks.
- The system has no intrinsic “patch set 3 of logical change X” object.
- One branch may contain several conceptual changes, while one conceptual change can span several branches.

This is simple and intuitive for isolated contributions, but it models stacks indirectly.

### Gerrit versus Differential

| Property | GitHub pull request | Gerrit change | Phabricator/Phorge Differential |
|---|---|---|---|
| Primary identity | Head/base branch pair and PR number | Stable `Change-Id` plus change number | Stable Differential Revision, e.g. `D123` |
| Review unit | Aggregate branch delta | Normally one commit/logical change | Uploaded patch or commit-sized revision |
| New version | Push more commits, amend or force-push branch | Amend commit and upload a new numbered patch set | `arc diff --update` uploads a new diff/version |
| Version comparison | Available but can become awkward after rebases/base changes | Explicit patch-set-to-patch-set comparison | Explicit revision update history |
| Stack representation | Multiple branches/PRs targeting one another | Related changes with commit/dependency structure | Dependent Differential revisions |
| Landing | Merge/squash/rebase PR | Submit approved latest patch set | Author/bot runs `arc land` or automated landing |
| Repository coupling | PR is inherently tied to hosted branches | Gerrit is both Git server and review gate | Differential is conceptually separate from Diffusion; it can review uploaded changes before they exist on the public target branch |
| Best property | Low-friction collaboration and forks | Rigorous versioned change review | Flexible pre-commit review and stacked revision workflow |

Phabricator’s normal lifecycle is “prepare revision → review/discuss → update revision → accept → land”; `arc land` can squash and publish the accepted result. [web:57][web:422][web:433]

### Tools that work around GitHub

| Tool | Representation and value | Limitation |
|---|---|---|
| GitHub `gh stack` | Native 2026 mechanism for creating a chain of dependent PRs, navigating it and cascading rebases through higher branches. [web:3][web:8] | Still one branch and PR per layer; updates use force-with-lease and the GitHub PR data model. |
| Graphite | CLI/UI maintains branch dependencies, submits a full stack and restacks/merges it in order. [web:1][web:9] | Commercial overlay on GitHub. |
| `ghstack` | Converts each commit in a local stack into a separate GitHub PR and manages synthetic branches. [web:14] | Branch proliferation and generated branch conventions; review still lives in GitHub. |
| `spr` | Client-side tool where each commit becomes a PR and one local branch can represent a stack; `spr update` synchronizes amended commits. [web:136][web:137] | No special server support, so it must continually translate commit semantics into branch PRs. |
| Sapling `sl pr` / `sl ghstack` | Rich local stack manipulation and submission; descendants can be automatically rebased after editing a middle commit. [web:377][web:388] | Requires Sapling conventions and preferably ReviewStack for correct presentation. |
| ReviewStack | Alternative GitHub PR viewer that recognizes Sapling/ghstack stacks and shows the intended per-commit diff with stack navigation. [web:146] | Viewer only; GitHub remains authoritative. |
| `git-branchless` | Enhances local Git with a commit graph, revsets, undo/restack operations and submission support for selected stacks. Its submit command pushes commits and associates them with forge objects. [web:144] | Less turnkey than Graphite; forge adapters and workflow conventions still matter. |

---

## 5. Federation and decentralization

**ForgeFed** is an ActivityPub extension intended to let users on one forge follow projects, open or discuss issues and merge requests, and interact with repositories hosted on another forge without creating an account there. Vervis is the reference implementation; Forgejo is the primary production-forge implementer; Pagure’s plug-in is unmaintained. [web:91]

It has not taken off for several reinforcing reasons:

| Barrier | Why it is harder than social-media federation |
|---|---|
| Authorization | Reading a post is simple; accepting a patch, changing an issue, approving code or triggering CI requires repository-specific capabilities. ForgeFed’s own work has had to address object capabilities because ActivityPub does not provide them directly. [web:97] |
| Identity and trust | A forge must map remote identities to authorship, signatures, bans, contributor agreements, branch permissions and audit records. |
| Mutable/versioned objects | Reviews have patch sets, force pushes, comments tied to lines, merge states, CI statuses and dependency graphs—not just immutable notes and replies. |
| Git transport is separate | ActivityPub can federate metadata, but Git objects, LFS data, release assets and forks still need transfer, mirroring and integrity rules. |
| Spam and abuse | Open issue/PR federation gives remote actors a path to consume moderation, database, email and CI resources. |
| Product-schema mismatch | GitHub PRs, GitLab merge requests, Gerrit changes and Differential revisions do not have one lossless common model. |
| Network effects | Federation becomes useful only when several large forges interoperate. GitHub has little commercial incentive to commoditize its social graph and metadata. |
| Implementation maturity | ForgeFed itself acknowledges that implementation progressed more slowly than hoped; Vervis remains proof-of-concept-level, while Forgejo’s early visible functionality began with comparatively small features such as federated stars. [web:97][web:100][web:104] |

Thus, decentralization today usually means **self-hosting plus Git mirroring**, not transparent cross-forge PR/review federation.

---

## 6. Economics of code hosting

### What costs money

| Cost center | Cost behavior |
|---|---|
| Git repositories | Ordinary source is relatively cheap: compressed Git objects, refs and packfiles. Fork/object deduplication can lower physical storage further. Large histories, generated files and frequent repacking increase I/O and CPU. |
| Database/search | Issues, comments, reviews, permissions, audit events, notifications, code indexes and dependency/security graphs are generally more operationally complex than bare Git storage. |
| Replication and backups | High availability requires multiple copies, snapshots, disaster recovery, restore testing and often geographically distributed storage. |
| Bandwidth/egress | Clones, fetches, LFS binaries, releases, packages, Pages and CI artifacts can cost more than storage. As a retail benchmark, AWS S3 Ireland internet egress is about $0.09/GB, while Cloudflare R2 lists $0.015/GB-month storage and no direct egress charge, illustrating how strongly architecture/provider choice affects forge economics. [web:331][web:333] |
| CI compute | Usually the largest variable expense: VM booting, CPU/RAM/GPU time, operating-system licensing, concurrency, logs, artifacts and cache traffic. |
| Security and abuse | Malware scanning, secret scanning, DDoS protection, spam accounts, cryptomining prevention and isolation of untrusted PR workloads. |
| People | SREs, security response, support, moderation, database/search engineering and legal/compliance commonly dominate the cost of a serious public service. |

### Illustrative—not GitHub’s internal cost

At public retail rates:

- **1 TB-month of R2 standard object storage:** about **$15**, before request charges.
- **1 TB-month of S3 Standard:** roughly **$23** before operations.
- **10 TB of S3 internet egress:** roughly **$900** at a $0.09/GB headline rate.
- **100,000 GitHub Linux 2-core Actions minutes:** approximately **$600** at $0.006/minute.
- **100,000 macOS minutes:** approximately **$6,200** at $0.062/minute. [web:123][web:331][web:333]

The lesson is not that a forge can be run for $15: source blobs are cheap, while egress, CI, databases, redundancy and labor are not.

### Hosted pricing versus underlying economics

| Provider | How costs are recovered |
|---|---|
| GitHub | Free public hosting creates network effects; private collaboration is monetized through seats, Actions/Codespaces/storage/LFS usage, Copilot, security add-ons and enterprise support/compliance. Public Actions alone consumed 11.5 billion free minutes in the 2025 reporting year, a substantial subsidy. [web:451] |
| GitLab | Free tier plus $29/user/month Premium and negotiated Ultimate; extra shared-runner compute at $10/1,000 minutes and storage at $5 per 10 GiB-month. Self-managed customers pay GitLab licensing/support while supplying infrastructure and operations themselves. [web:124][web:135] |
| Bitbucket/Azure DevOps | Seat tiers combined with build-minute, storage or parallel-job limits; profitable integration with the larger Jira or Azure/Microsoft ecosystem. [web:302][web:304] |
| Gitea/Forgejo self-hosting | No mandatory per-seat software cost, but infrastructure, backups, security, upgrades and staff become the operator’s responsibility. |
| Codeberg | Donations and membership subsidize free FOSS hosting. Its published FAQ explicitly says donations fund not only minimum operation but additional CI and search capacity. [web:323] |
| sourcehut | Direct low-cost subscriptions rather than advertising or investor-subsidized free accounts. An earlier financial report illustrates the real mix: colocation/power/network, European hardware investment, payment fees, backups and staff-related sustainability—not merely disks. [web:289] |

Open-source hosting therefore has a public-goods problem: popular projects generate storage, search, download, moderation and CI costs, while users expect free access. Sustainable models are donations/membership, paid hosted accounts, sponsorship, institutional subsidy, consulting/support, or restricting free service to genuine FOSS and fair use.

---

## 7. Migration and lock-in

### What is portable

| Asset | Portability | Notes |
|---|---|---|
| Git commits and history | **High** | A clone contains reachable commits, trees and blobs. `git clone --mirror` plus `git push --mirror` transfers refs, branches and tags. |
| Commit authorship | **High, but identity mapping is imperfect** | Names/emails are in commits; connection to current forge accounts, verified signatures or enterprise identities may not survive. |
| Tags | **High** | Lightweight and annotated tags transfer as Git refs, assuming mirrored refs are included. |
| Git notes and unusual refs | **Medium–high** | Can transfer with a true mirror, but destination products may not display or retain every namespace. |
| Git LFS pointer files | **High** | Pointers are Git blobs. |
| Git LFS binary objects | **Low unless explicitly copied** | They are stored outside Git. GitHub’s migration documentation says pointers migrate but binaries must be pushed separately. [web:407] |
| Issues, comments, labels, milestones | **Medium** | API/importer dependent. Basic content often migrates, but authors, timestamps, reactions, cross-links, event history and attachments may be altered. |
| Pull/merge requests | **Medium–low** | Importers may reconstruct them, but review threads, obsolete diffs, approvals, requested reviewers, merge commits and branch/fork relationships vary. |
| Releases and assets | **Medium** | Release records may import; binaries require explicit migration and validation. |
| Wiki | **Medium–high** | Often a separate Git repository, but importer behavior varies. |
| CI configuration files | **Syntactically portable, operationally low** | YAML travels as source, but `.github/workflows`, `.gitlab-ci.yml`, Bitbucket Pipelines and Azure Pipelines use different event, secret, runner and permission models. |
| Secrets, variables and environments | **Low** | Deliberately not exportable in plaintext; must be recreated and rotated. |
| Permissions, teams and enterprise identity | **Low** | Role models do not map exactly; GitHub explicitly does not migrate GitLab repository permissions, group settings or membership. [web:409] |
| Branch protection/rulesets | **Low** | Semantics differ and commonly require manual reconstruction. |
| Projects/boards/reporting | **Low** | Custom fields, views, automations and portfolio relationships are highly proprietary. |
| Actions history, artifacts and caches | **Very low** | GitHub-to-GitHub Enterprise Importer itself omits runners, run history, workflow artifacts, secrets and variables. [web:413] |
| Apps, webhooks and marketplace subscriptions | **Very low** | Endpoints, app installations, tokens and vendor contracts must be reconfigured. |
| Package registries | **Low–medium** | Package files can be downloaded and republished, but coordinates, signatures, provenance, permissions and downstream references need migration. |
| Search/security indexes | **None** | Rebuilt by the destination; alerts, code-search indexes and dependency graphs are not Git data. |

### Available migration tools

| Tool/path | What it can do | Important limitation |
|---|---|---|
| `git clone --mirror` / `git push --mirror` | Vendor-neutral transfer of Git refs and history. | No issues, PRs, permissions, releases, CI state, LFS binaries or organization metadata. |
| GitHub Enterprise Importer | GitHub-to-GitHub and GitLab-to-GitHub migration of Git source and supported metadata. The GitLab route uses `gh gl2gh`, exporting GitLab project data and converting merge requests to PRs. [web:116][web:410] | Does not migrate GitLab CI pipelines/schedules, policies, groups, permissions, webhooks, variables, job traces/artifacts or LFS objects. [web:407] |
| GitHub Actions Importer | Audits and converts supported Jenkins, Azure DevOps, GitLab CI, CircleCI and Travis pipelines into approximate Actions workflows. | Translation requires manual remediation for custom plugins, secrets, runners, deployment environments and unsupported expressions. |
| GitLab GitHub importer | Imports repository data and maps GitHub PRs to GitLab merge requests, with staged workers for issues, collaborators, reviews and metadata. [web:117] | API limits, unavailable users and model mismatches can produce partial imports; historical events and collaborators have required separate fixes. [web:106][web:417] |
| Gitea/Forgejo built-in migration | Can import Git plus selected issues, PRs, labels, milestones, releases, comments and wikis from supported services. [web:362][web:371] | Not every review event, app, workflow, permission or identity can be represented; a mirror only synchronizes Git changes, not newly created issues/PRs. |
| `ghe-migrator` | Rich migration among supported GitHub Enterprise products, including teams, reviews, releases and event timestamps. [web:113] | GitHub-specific and not a general cross-forge standard. |
| API scripts/export archives | GraphQL/REST scripts, GitLab exports and third-party tools can preserve otherwise unsupported metadata in JSON or recreate destination objects. | Recreated comments may be owned by a migration bot; timestamps, IDs, links, approvals and audit authenticity may be lost. |

### Practical migration procedure

1. **Inventory** repositories, LFS, releases, packages, Actions, Apps, webhooks, teams, rules and compliance dependencies.
2. **Mirror Git and LFS first**, and verify branch/tag counts and object integrity.
3. **Export metadata independently** to durable JSON/archive form, even when using an importer.
4. **Build explicit mappings** for users, teams, labels, milestones, permissions and protected branches.
5. **Translate CI**, then rotate secrets rather than copying old credentials.
6. **Run source and destination in parallel**, freezing writes or performing a final delta synchronization.
7. **Preserve the old forge read-only** for audit links and review history that cannot be reconstructed.
8. **Test a representative repository**—including forks, old PRs, LFS, releases and CI—before bulk migration.
9. **Maintain an independent mirror and release backup** after migration.

The most useful lock-in test is therefore not “Can we clone our code?” It is: **Can we reproduce review decisions, work queues, automation, permissions, releases and deployment authority somewhere else within an acceptable outage window?** For most substantial GitHub organizations, the answer requires prior engineering rather than a one-command export.

Citations:
[31] https://github.com/pricing
[32] https://docs.github.com/en/get-started/learning-about-github/githubs-plans
[33] https://github.blog/news-insights/octoverse/octoverse-a-new-developer-joins-github-every-second-as-ai-leads-typescript-to-1/
[34] https://docs.github.com/en/enterprise-cloud@latest/get-started/learning-about-github/githubs-plans
[35] https://docs.github.com/billing/managing-billing-for-github-codespaces/about-billing-for-github-codespaces
[36] https://github.com/pricing/calculator
[37] https://ahmedatoui.com/en/articles/github-octoverse-2025-ai-repositories
[38] https://docs.github.com/en/billing/concepts/product-billing/github-actions
[39] https://polomodov.tech/en/book-cube/2-3-octoverse-2025-a-new-developer-joins-4240/
[40] https://rockstardeveloperuniversity.com/open-source-contribution-statistics/
[41] https://docs.github.com/en/enterprise-cloud@latest/billing/concepts/product-billing
[42] https://docs.github.com/en/billing/reference/product-usage-included
[43] https://azure.microsoft.com/en-us/pricing/details/githubenterprise/
[44] https://www.getpanto.ai/blog/github-statistics
[45] https://docs.github.com/en/billing/concepts/product-billing/github-codespaces
[76] https://github.com/orgs/community/discussions/141109
[77] https://docs.github.com/en/pull-requests/how-tos/review-pull-requests/reviewing-proposed-changes-in-a-pull-request
[78] https://github.com/orgs/community/discussions/11122
[79] https://github.blog/changelog/2026-02-05-improved-pull-request-files-changed-february-5-updates/
[80] https://github.com/orgs/community/discussions/163932
[81] https://github.com/orgs/community/discussions/39341
[82] https://stackoverflow.com/questions/79109299/repeatedly-getting-very-large-diffs-on-github-pull-requests
[83] https://docs.github.com/en/pull-requests/how-tos/review-pull-requests/reviewing-stacked-pull-requests
[84] https://stackoverflow.com/questions/71946575/after-rebase-and-force-push-unresolved-conversations-remain-outdated-and-block
[85] https://dshfind.com/en/docs/cookbook/responding-to-pr-review-on-a-stack
[86] https://docs.github.com/en/enterprise-cloud@latest/pull-requests/how-tos/review-pull-requests/reviewing-stacked-pull-requests
[87] https://docs.github.com/en/enterprise-cloud@latest/pull-requests/collaborating-with-pull-requests/reviewing-changes-in-pull-requests/reviewing-proposed-changes-in-a-pull-request
[88] https://github.com/isaacs/github/issues/443
[89] https://medium.com/@rgsuhas/githubs-diff-checker-sucks-but-there-s-a-way-ad6796dfcd8d
[90] https://discourse.llvm.org/t/is-there-any-stacked-pr-methodology-consensus/91558
[61] https://docs.github.com/en/billing/concepts/product-billing/git-lfs
[62] https://docs.github.com/billing/managing-billing-for-git-large-file-storage/about-billing-for-git-large-file-storage
[63] https://docs.github.com/en/enterprise-cloud@latest/billing/concepts/product-billing/git-lfs
[64] https://storagebites.com/git-lfs
[65] https://github.blog/news-insights/product-news/announcing-git-large-file-storage-lfs/
[66] https://github.com/pricing/calculator
[67] https://github.com/github/docs/blob/main/content/billing/how-tos/products/upgrade-git-lfs-storage.md
[68] https://github.blog/changelog/2024-06-03-new-enterprise-accounts-have-metered-billing-for-git-lfs/
[69] https://docs.github.com/en/billing/concepts/product-billing/github-actions
[70] https://docs.github.com/ja/enterprise-cloud@latest/billing/concepts/product-billing/git-lfs?apiVersion=2022-11-28
[71] https://docs.github.com/en/enterprise-cloud@latest/repositories/working-with-files/managing-large-files/about-storage-and-bandwidth-usage
[72] https://github.com/orgs/community/discussions/61362
[73] https://buildpulse.io/blog/github-actions-cache-optimization-benchmarks
[74] https://docs.github.com/en/billing/reference/product-usage-included
[75] https://runs-on.com/blog/github-actions-slow-and-expensive-what-are-the-alternatives/
[16] https://docs.gitlab.com/ci/
[17] https://about.gitlab.com/topics/ci-cd/
[18] https://handbook.gitlab.com/handbook/engineering/architecture/design-documents/mlops/
[19] https://codeberg.org/forgejo/governance/issues/58
[20] https://en.wikipedia.org/wiki/Forgejo
[21] https://gitlab.com/groups/gitlab-org/-/work_items/15224
[22] https://blog.codeberg.org/codeberg-launches-forgejo.html
[23] https://forgejo.org/2024-02-monthly-update/
[24] https://blog.stephane-robert.info/docs/services/devops/forgejo/presentation/
[25] https://docs.gitlab.com/topics/build_your_application/
[26] https://forgejo.org/2024-02-forking-forward/
[27] https://alm.developpez.com/actu/368227/Forgejo-une-forge-logicielle-legere-autohebergee-concue-pour-faciliter-la-gestion-collaborative-des-projets-de-developpement-le-projet-est-un-fork-de-Gitea-et-met-l-accent-sur-des-performances-optimales/
[28] https://europeanstack.com/software/sourcehut
[29] https://docs.gitlab.com/ci/examples/
[30] https://forgejo.org/2024-10-monthly-update/
[46] https://gerrit-review.googlesource.com/Documentation/concept-patch-sets.html
[47] https://gerrit-review.googlesource.com/Documentation/concept-changes.html
[48] https://gerrit-review.googlesource.com/Documentation/cmd-review.html
[49] https://gerrit-review.googlesource.com/Documentation/intro-user.html
[50] https://source.android.com/docs/setup/contribute/view-patches
[51] https://gerrit-review.googlesource.com/Documentation/json.html
[52] https://www.gerritcodereview.com/
[53] https://gerrit-review.googlesource.com/Documentation/config-labels.html
[54] https://android.googlesource.com/platform/external/openscreen/+/HEAD/docs/advanced_gerrit.md
[55] https://gerrit-review.googlesource.com/Documentation/cmd-query.html
[56] https://gerrit-review.googlesource.com/Documentation/note-db.html
[57] https://secure.phabricator.com/book/phabricator/article/differential/
[58] https://graphite.com/guides/gerrits-approach-to-code-review
[59] https://we.phorge.it/differential/
[60] https://github.com/revinet/revi.xyz/blob/master/CONTRIBUTING.md
[1] https://graphite.com/docs/create-submit-prs
[2] https://github.com/mrmans0n/git-gud
[3] https://docs.github.com/en/pull-requests/how-tos/stacked-pull-requests
[4] https://docs.github.com/ja/pull-requests/reference/stacked-prs-cli-commands
[5] https://gist.github.com/dbalatero/67b7e0063a3121043a039cc5f2f5bf25
[6] https://graphite.com/blog/stacked-prs
[7] https://nlopez.io/introducing-git-gud-a-stacked-diffs-cli-for-github-and-gitlab/
[8] https://docs.github.com/en/pull-requests/get-started/stacked-prs-quickstart
[9] https://graphite.com/docs/merge-stack-prs-github
[10] https://github.com/shish/stack-guide
[11] https://docs.github.com/en/pull-requests/how-tos/create-pull-requests/managing-stacked-pull-requests
[12] https://github.com/facebook/sapling/issues/699
[13] https://docs.github.com/en/enterprise-cloud@latest/pull-requests/reference/stacked-prs-cli-commands
[14] https://github.com/ezyang/ghstack
[15] https://docs.github.com/en/pull-requests/get-started/about-stacked-prs
[91] https://forgefed.org/
[92] https://github.com/forgefed/forgefed
[93] https://nesbitt.io/2026/01/25/pkgfed-activitypub-for-package-releases.html
[94] https://biggo.com/news/202508140712_ForgeFed_Implementation_Progress_Reality_Check
[95] https://codeberg.org/forgejo/forgejo/issues/59
[96] https://media.libreplanet.org/mgoblin_media/media_entries/2861/Federated-Forges.pdf
[97] https://forgefed.org/blog/stabilizing-ocaps/
[98] https://gitea.com/6543/forgefed
[99] https://forgejo.org/2023-01-10-answering-forgejo-federation-questions/
[100] https://forgefed.org/blog/vervis-actor-refactoring/
[101] https://news.ycombinator.com/item?id=44873341
[102] https://www.reddit.com/r/opensource/comments/1npqk6v/what_happened_to_forgefed_a_federated_git_service/
[103] https://forum.gitea.com/t/forgefed-federation-in-gitea/1157
[104] https://archive.fosdem.org/2025/events/attachments/fosdem-2025-5610-show-and-tell-federation-at-forgejo/slides/238275/2025_02_0_i9uW1ym.pdf
[105] https://gitlab.com/gitlab-org/gitlab/-/issues/30672
[121] https://docs.github.com/en/billing/concepts/product-billing/github-actions
[122] https://github.com/resources/insights/2026-pricing-changes-for-github-actions
[123] https://docs.github.com/en/billing/reference/actions-runner-pricing
[124] https://about.gitlab.com/pricing/
[125] https://github.com/pricing/calculator
[126] https://www.usagepricing.com/blueprint/gitlab
[127] https://cicdpipelinecost.com/github-actions-pricing
[128] https://docs.github.com/en/enterprise-server@3.17/billing/reference/actions-runner-pricing
[129] https://checkthat.ai/brands/gitlab/pricing
[130] https://www.stackscored.com/pricing/git-hosting/gitlab/
[131] https://plans.apis.io/plans/gitlab-ci/gitlab-ci-plans-pricing/
[132] https://github.blog/changelog/2026-01-01-reduced-pricing-for-github-hosted-runners-usage/
[133] https://docs.github.com/en/enterprise-cloud@latest/billing/reference/actions-runner-pricing
[134] https://data.tansohq.com/c/gitlab
[135] https://about.gitlab.com/pricing/faq-compute-minutes/
[106] https://gitlab.com/gitlab-org/gitlab/-/issues/388869
[107] https://docs.github.com/en/migrations/using-github-enterprise-importer/migrating-between-github-products/about-migrations-between-github-products
[108] https://gitlab.com/gitlab-org/gitlab/-/issues/355137
[109] https://github.com/itchyny/github-migrator
[110] https://gitlab.com/gitlab-org/gitlab/-/issues/349908
[111] https://gitlab.com/gitlab-org/gitlab/-/issues/433536
[112] https://github.com/timrogers/gh-migrate-project
[113] https://docs.github.com/en/migrations/using-ghe-migrator/about-ghe-migrator
[114] https://docs.github.com/en/migrations
[115] https://github.com/michaelgwelch/migrate-issues
[116] https://docs.github.com/en/migrations/using-github-enterprise-importer/migrate-from-gitlab/understand-migrations
[117] https://docs.gitlab.com/development/github_importer/
[118] https://gitlab.com/gitlab-org/gitlab/-/issues/354938
[119] https://github.com/timrogers/migrate-github-project
[120] https://github.com/NicholasBoll/github-migration
[181] https://docs.github.com/en
[182] https://docs.github.com/organizations/managing-user-access-to-your-organizations-repositories/repository-roles-for-an-organization
[183] https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-rulesets/creating-rulesets-for-a-repository
[184] https://docs.github.com/en/copilot/concepts/agents/cloud-agent/about-cloud-agent
[185] https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-rulesets/about-rulesets
[186] https://docs.github.com/en/repositories
[187] https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-protected-branches/managing-a-branch-protection-rule
[188] https://github.blog/engineering/architecture-optimization/the-technology-behind-githubs-new-code-search/
[189] https://docs.github.com/en/organizations/managing-user-access-to-your-organizations-repositories/managing-repository-roles
[190] https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-rulesets/available-rules-for-rulesets
[191] https://github.com/github/docs
[192] https://github.com/github/docs/blob/main/content/repositories/configuring-branches-and-merges-in-your-repository/managing-protected-branches/about-protected-branches.md
[193] https://github.com/github/docs/blob/main/content/index.md
[194] https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-rulesets/converting-branch-protections-to-rulesets
[195] https://docs.github.com/en/enterprise-cloud@latest/organizations/managing-peoples-access-to-your-organization-with-roles/permissions-of-custom-organization-roles
[226] https://github.com/pricing
[227] https://www.nocode.mba/articles/github-copilot-pricing
[228] https://www.githubcopilot.dev/pricing
[229] https://github.blog/news-insights/company-news/github-copilot-is-moving-to-usage-based-billing/
[230] https://www.getpricepulse.com/blog/github-pricing-2026-complete-guide.html
[231] https://docs.github.com/en/enterprise-cloud@latest/copilot/get-started/plans
[232] https://docs.github.com/en/copilot/get-started/plans
[233] https://docs.github.com/en/enterprise-cloud@latest/copilot/reference/copilot-billing/models-and-pricing
[234] https://usagebox.com/articles/github-copilot-usage-based-billing-2026
[235] https://checkthat.ai/brands/github-copilot/pricing
[236] https://docs.github.com/en/copilot/concepts/billing/organizations-and-enterprises
[237] https://samexpert.com/github-copilot-licensing-guide/
[238] https://docs.github.com/en/billing/concepts/product-billing/github-copilot-licenses
[239] https://capitalandcompute.net/blog/github-pricing-plans-cost/
[240] https://docs.github.com/en/copilot/reference/copilot-billing/models-and-pricing
[196] https://pickuma.com/for-dev/github-projects-review-built-in-project-management-2026/
[197] https://tianchi.blog/blog/linear-vs-github-vs-jira/
[198] https://www.devtoolreviews.com/reviews/linear-vs-github-issues-vs-jira
[199] https://octopusbuilds.com/blog/linear-vs-jira-vs-github-issues-ai-driven-development
[200] https://www.ideaplan.io/compare/github-issues-vs-jira
[201] https://gist.github.com/nzakas/bb025e31583076241d9bac8caee4ba82
[202] https://github.com/orgs/community/discussions/39688
[203] https://www.stackfyi.com/guides/linear-vs-jira-vs-github-issues-2026
[204] https://toolradar.com/compare/github-issues-vs-linear
[205] https://nira.com/github-issues-vs-jira/
[206] https://medium.com/momenton/jira-vs-github-issue-tracking-4cdf3bdd1437
[207] https://lists.apache.org/thread/smy6wwklkkrhj3brmr8oo1ktkrlfo48k
[208] https://github.com/orgs/community/discussions/186939
[209] https://tech-insider.org/linear-vs-jira-2026/
[210] https://cotera.co/articles/linear-vs-jira-comparison
[166] https://github.com/github/docs/blob/main/content/search-github/searching-on-github/searching-code.md
[167] https://docs.github.com/en/search-github/github-code-search/about-github-code-search
[168] https://github.com/orgs/community/discussions/64775
[169] https://devactivity.com/insights/unlocking-visibility-a-guide-to-github-repository-indexing-and-achieving-your-software-development-goals/
[170] https://techoral.com/github-advanced/phase-2-repo-architecture.html
[171] https://github.com/orgs/community/discussions/18624
[172] https://devactivity.com/posts/development-integrations/github-code-search-ensuring-your-repository-is-indexed-for-ai-team-productivity/
[173] https://docs.github.com/en/repositories/working-with-files/managing-large-files/about-large-files-on-github
[174] https://www.codewithkarani.com/blog/monorepo-github-actions-path-filters-shared-code
[175] https://github.com/github/docs/blob/main/content/search-github/searching-on-github/searching-for-repositories.md
[176] https://github.com/orgs/community/discussions/84737
[177] https://www.devopsness.com/blog/github-actions-monorepo-ci-how-we-cut-build-times-without-breaking-main-2026-03-11
[178] https://ninkovic.dev/blog/2025/think-twice-before-using-github-actions
[179] https://docs.github.com/en/search-github/github-code-search/using-github-code-search
[180] https://github.com/marketplace/actions/paths-changes-filter
[241] https://www.githubstatus.com/
[242] https://www.theverge.com/news/757461/microsoft-github-thomas-dohmke-resignation-coreai-team-transition
[243] https://nationalcioreview.com/articles-insights/extra-bytes/microsoft-folds-github-into-coreai-as-ceo-steps-down/
[244] https://blog.incidenthub.cloud/github-reliability-outage-history-2025-2026
[245] https://getcoai.com/news/microsoft-absorbs-github-into-coreai-division-as-ceo-departs/
[246] https://www.theregister.com/software/2025/09/05/github-copilot-on-autopilot-as-community-complaints-persist/599985
[247] https://statusgator.com/services/github/outage-history
[248] https://github.blog/news-insights/company-news/github-availability-report-december-2025/
[249] https://www.crn.com/news/ai/2025/microsoft-moves-github-into-coreai-group-ceo-confirms-upcoming-departure
[250] https://www.cnbc.com/2025/08/11/microsofts-github-chief-is-leaving-competition-ramps-up-in-ai-coding.html
[251] https://windowsforum.com/threads/github-ceo-to-step-down-as-coreai-tightens-microsoft-ai-strategy.377347/
[252] https://www.theregister.com/software/2026/03/30/github-backs-down-kills-copilot-pr-tips-after-backlash/5224768
[253] https://www.mediapost.com/publications/article/408087/github-ceo-departs-microsoft-puts-it-under-core-a.html
[254] https://www.theregister.com/software/2025/08/29/github-engineer-team-coerced-to-put-grok-in-copilot/602238
[255] https://www.thestack.technology/github-suffers-outage/
[211] https://docs.gitea.com/
[212] https://jira.atlassian.com/browse/JRACLOUD-97223
[213] https://marketplace.microsoft.com/en-us/product/virtual-machines/cloud-infrastructure-services.gitea-ubuntu?tab=overview
[214] https://about.gitea.com/
[215] https://codeberg.org/
[216] https://support.atlassian.com/bitbucket-cloud/docs/integrate-jira-and-pipelines/
[217] https://www.opentechhub.io/gitea/
[218] https://github.com/MicrosoftDocs/azure-devops-docs/blob/main/docs/pipelines/repos/on-premises-bitbucket.md
[219] https://gitea.com/
[220] https://www.linkedin.com/pulse/azure-pipeline-jira-software-integration-scenario-mustafa-%C3%B6zcan-k%C3%B6ker
[221] https://learn.microsoft.com/en-us/azure/devops/pipelines/repos/bitbucket?view=azure-devops
[222] https://marketplace.microsoft.com/en-us/product/saas/pcloudhosting.gitea-1?tab=overview
[223] https://www.linuxlinks.com/gitea-painless-self-hosted-git-service/
[224] https://codeberg.org/Codeberg/Community/issues/856
[225] https://jira.atlassian.com/browse/BCLOUD-20718?filter=98686
[256] https://opensource.google/projects/gerrit
[257] https://en.wikipedia.org/wiki/Gerrit_(software)
[258] https://gerrit.googlesource.com/homepage/+/md-pages/about.md
[259] https://gerrit-review.googlesource.com/Documentation/intro-gerrit-walkthrough-github.html
[260] https://gerrit-review.googlesource.com/Documentation/intro-quick.html
[261] https://gerrit-review.googlesource.com/Documentation/
[262] https://lwn.net/Articles/359489/
[263] https://gerrit.googlesource.com/gerrit/
[264] https://gerrit-review.googlesource.com/Documentation/intro-gerrit-walkthrough.html
[265] https://www.cabird.com/static/d6c0b785cd514a12e9092916900fcd45/mukadam2013gerrit.pdf
[266] https://graphite.com/blog/how-google-does-code-review
[267] https://groups.google.com/g/repo-discuss/c/CF9XcPL9R64
[268] https://source.android.com/docs/setup/download/source-control-tools
[269] https://wiki.lineageos.org/how-to/using-gerrit/
[270] https://gerrit-review.googlesource.com/Documentation/intro-user.html
[151] https://phorge.it/
[152] https://news.ycombinator.com/item?id=27773747
[153] https://we.phorge.it/w/why_migrating_from_phabricator_to_phorge/
[154] http://github.com/arxanas/git-branchless/discussions/1573
[155] https://en.wikipedia.org/wiki/Phabricator
[156] https://archive.fosdem.org/2024/schedule/event/fosdem-2024-3610-unpack-phabricator-welcome-phorge-forking-the-opinionated-open-source-project-manager/
[157] https://forums.freebsd.org/threads/phabricator-to-phorge.85728/
[158] https://laoutaris.org/blog/phabricator/
[159] https://github.com/llvm/llvm-iwg/issues/44
[160] https://newsletter.pragmaticengineer.com/p/stacked-diffs-and-tooling-at-meta
[161] https://discuss.kde.org/t/kde-phabricator-maintenance-phorge-it/1108
[162] https://ppwwyyxx.com/blog/2023/Stacked-Diffs-Pull-Requests/
[163] https://phabricator.wikimedia.org/phame/blog/view/14/
[164] https://phabricator.wikimedia.org/T117058
[165] https://we.phorge.it/source/phorge/
[136] https://github.com/ejoffe/spr
[137] https://ejoffe.github.io/spr/
[138] https://github.com/ejoffe/spr/blob/master/readme.md
[139] https://context7.com/ejoffe/spr
[140] https://medium.com/@benedict.pregler/manage-stacked-prs-with-spr-295de2744e00
[141] https://olivernguyen.io/w/stacked.prs/
[142] https://pkg.go.dev/github.com/ejoffe/spr@v0.16.0/github
[143] https://mcpmarket.com/tools/skills/git-stacked-pr-submitter
[144] https://man.archlinux.org/man/git-branchless-submit.1.en
[145] https://blog.csdn.net/gitblog_00024/article/details/139080935
[146] https://sapling-scm.com/docs/addons/reviewstack/
[147] https://github.com/arxanas/git-branchless/pull/541
[148] https://docs.github.com/en/pull-requests/how-tos/create-pull-requests/managing-stacked-pull-requests
[149] https://discussion.fedoraproject.org/t/bennetb01-spr/40088
[150] https://docs.github.com/en/pull-requests/how-tos/stacked-pull-requests
[271] https://sourcehut.org/
[272] https://europeanstack.com/software/sourcehut
[273] https://sourcehut.org/pricing/
[274] https://sourcehut.org/blog/2020-07-14-setting-up-ci-for-mailing-lists/
[275] https://sourcehut.org/blog/2025-12-01-proposed-pricing-changes/
[276] https://alexjenkins.tech/apps/sourcehut/
[277] https://nullbuffer.com/articles/sourcehut_impressions.html
[278] https://docs.klutch.sh/guides/open-source-software/sourcehut/
[279] https://sourcehut.org/blog/2022-11-15-four-years/
[280] https://europeanpurpose.com/tool/sourcehut
[281] https://privacytools.io/app/sourcehut
[282] https://man.sr.ht/
[283] https://man.sr.ht/~ireas/guides/contributing.md
[284] https://sourcehut.org/community
[285] https://blog.strus.guru/2022/04/going-sourcehut/
[301] https://learn.microsoft.com/en-us/azure/devops/user-guide/what-is-azure-devops?view=azure-devops
[302] https://azure.microsoft.com/en-us/pricing/details/devops/azure-devops-services/
[303] https://www.wasilzafar.com/pages/series/software-engineering/deep-dive-azure-devops-01-platform-overview.html
[304] https://www.atlassian.com/software/bitbucket/pricing
[305] https://learn.microsoft.com/en-us/azure/devops/get-started/?view=azure-devops
[306] https://learn.microsoft.com/en-us/azure/devops/?view=azure-devops
[307] https://medium.com/@bisinet/azure-devops-services-a-summary-e1c96a36db0b
[308] https://learn.microsoft.com/en-us/azure/devops/release-notes/2018/sep-10-azure-devops-launch
[309] https://learn.microsoft.com/is-is/azure/devops/user-guide/services?view=azure-devops
[310] https://dev.to/rhuturaj_takle/azure-devops-microsofts-platform-for-repos-pipelines-boards-and-deployments-24b2
[311] https://azure.microsoft.com/en-us/products/devops
[312] https://azure.microsoft.com/en-us/blog/introducing-azure-devops/
[313] https://www.atlassian.com/software/bitbucket/features/pipelines
[314] https://apis.io/plans/azure-devops/azure-devops-plans-pricing/
[315] https://learn.microsoft.com/en-us/azure/devops/cross-service/cross-service-overview?view=azure-devops
[316] https://codeberg.org/forgejo
[317] https://codeberg.org/
[318] https://codeberg.org/Codeberg
[319] https://codeberg.codeberg.page/codeberg-fosdem-stand-site/
[320] https://codeberg.org/forgejo/pages
[321] https://blog.codeberg.org/
[322] https://www.europeanstack.io/en/products/codeberg
[323] https://docs.codeberg.org/getting-started/faq/
[324] https://en.wikipedia.org/wiki/Codeberg
[325] https://docs.codeberg.org/getting-started/what-is-codeberg/
[326] https://blog.codeberg.org/codebergs-budget-of-2026.html
[327] https://apis.io/plans/codeberg/codeberg-plans-pricing/
[328] https://codeberg.org/forgejo/forgejo
[329] https://de.wikipedia.org/wiki/Codeberg
[330] https://codeberg.org/forgejo/sustainability
[331] https://aws.amazon.com/s3/pricing/
[332] https://developers.cloudflare.com/r2/pricing/
[333] https://www.cloudflare.com/products/r2/
[334] https://frontdeskreview.com/software/object-storage/amazon-s3/
[335] https://www.usage.ai/blogs/aws/storage-cost/s3/cost-calculator/
[336] https://www.cloudzero.com/blog/s3-pricing/
[337] https://frontdeskreview.com/software/object-storage/cloudflare-r2/
[338] https://developers.cloudflare.com/r2/llms-full.txt
[339] https://blog.cloudflare.com/r2-ga/
[340] https://cloudburn.io/blog/amazon-s3-pricing
[341] https://blog.cloudflare.com/r2-open-beta/
[342] https://r2-calculator.cloudflare.com/
[343] https://pricingapis.com/storage/cloudflare-r2
[344] https://www.cloudflare.com/en-gb/developer-platform/products/r2/
[345] https://www.volcengine.com/article/378276
[286] https://www.sec.gov/Archives/edgar/data/1964789/000155837025001997/hut-20250303xex99d1.htm
[287] https://sourcehut.org/blog
[288] https://sourcehut.org/blog/2024-06-04-status-and-plans/
[289] https://sourcehut.org/blog/2023-03-27-2022-financial-report/
[290] https://codeberg.org/forgejo/sustainability
[291] https://www.prnewswire.com/news-releases/hut-8-reports-fourth-quarter-and-full-year-2025-results-302696352.html
[292] https://blog.codeberg.org/
[293] https://sourcehut.org/blog/2025-12-01-proposed-pricing-changes/
[294] https://www.atlassian.com/blog/development/updates-to-forge-pricing-effective-january-2026
[295] https://blog.codeberg.org/codebergs-budget-of-2026.html
[296] https://en.wikipedia.org/wiki/Codeberg
[297] https://matslats.net/ccmag-freemium
[298] https://www.stocktitan.net/sec-filings/HUT/10-k-hut-8-corp-files-annual-report-5ab1a143c1b8.html
[299] https://blog.codeberg.org/letter-from-codeberg-looking-into-the-new-year.html
[300] https://codeberg.org/Codeberg
[391] https://github.com/orgs/community/discussions/54747
[392] https://github.com/orgs/community/discussions/25087
[393] https://github.com/actions/cache
[394] https://devopsboys.com/blog/github-actions-cache-not-working-fix-2026
[395] https://github.com/orgs/community/discussions/27059
[396] https://github.com/orgs/community/discussions/163260
[397] https://github.com/vercel/vercel/discussions/9340
[398] https://github.com/orgs/community/discussions/18549
[399] https://github.com/mozilla/sccache/issues/1485
[400] https://markaicode.com/errors/github-actions-cache-miss-fix/
[401] https://prosopo.io/blog/github-actions-cache-chaos/
[402] https://github.com/actions/cache/blob/main/tips-and-workarounds.md
[403] https://www.remio.ai/post/github-actions-pricing-shock-is-fueling-developer-backlash-against-cloud-bloat
[404] https://github.com/orgs/community/discussions/58583
[405] https://docs.github.com/en/actions/reference/workflows-and-actions/dependency-caching
[346] https://docs.github.com/en/billing/concepts/product-billing/github-actions
[347] https://docs.github.com/en/actions/reference/limits
[348] https://docs.github.com/en/actions/reference/workflows-and-actions/dependency-caching
[349] https://docs.github.com/en/actions/concepts/billing-and-usage
[350] https://docs.github.com/en/enterprise-cloud@latest/billing/managing-billing-for-your-products/managing-billing-for-github-actions/about-billing-for-github-actions
[351] https://docs.github.com/en/enterprise-cloud@latest/billing/concepts/product-billing/github-actions
[352] https://github.blog/changelog/2025-11-20-github-actions-cache-size-can-now-exceed-10-gb-per-repository/
[353] https://github.com/actions/cache
[354] https://docs.github.com/en/billing/reference/actions-runner-pricing
[355] https://starsling.dev/fast-github-actions
[356] https://docs.github.com/en/enterprise-server@3.16/billing/reference/actions-runner-pricing
[357] https://docs.github.com/en/enterprise-server@3.20/billing/reference/actions-runner-pricing
[358] https://docs.github.com/en/enterprise-server@3.19/billing/reference/actions-runner-pricing
[359] https://docs.github.com/en/enterprise-server@3.21/billing/reference/actions-runner-pricing
[360] https://docs.github.com/en/enterprise-cloud@latest/billing/reference/actions-runner-pricing
[376] https://sapling-scm.com/docs/addons/reviewstack/
[377] https://sapling-scm.com/docs/git/sapling-stack/
[378] https://engineering.fb.com/2022/11/16/culture/meta-code-review-time-improving/
[379] https://newsletter.pragmaticengineer.com/p/stacked-diffs-and-tooling-at-meta
[380] https://engineering.fb.com/2012/07/06/web/under-the-hood-timeline-apps-behind-facebook-engineering/
[381] https://sapling-scm.com/docs/commands/diff/
[382] https://sapling-scm.com/
[383] https://sapling-scm.com/docs/overview/stacks/
[384] https://github.com/facebook/sapling/issues/699
[385] https://noamlerner.com/posts/sapling_test/
[386] https://developers.facebook.com/blog/post/2022/11/15/meta-developers-workflow-exploring-tools-used-to-code/
[387] https://wiki.freebsd.org/Phabricator
[388] https://engineering.fb.com/2022/11/15/open-source/sapling-source-control-scalable/
[389] https://cloud.tencent.com/developer/article/2388683
[390] https://claudemarketplaces.com/skills/flora131/atomic/sl-submit-diff
[421] https://releases.llvm.org/14.0.0/docs/Phabricator.html
[422] https://www.mediawiki.org/wiki/Phabricator/Differential/Code_Review_Workflows
[423] https://phabricator.wikimedia.org/T121751
[424] https://community.kde.org/Infrastructure/Phabricator
[425] https://ciao-lang.org/ciao/build/doc/ciao_devel.html/CodeReviews.html
[426] https://www.qodo.ai/blog/open-source-code-review-tools/
[427] https://gist.github.com/StevenACoffman/cbb8e95960d2daac2077b0f163f99929
[428] https://secure.phabricator.com/book/phabricator/article/arcanist_diff/
[429] https://thomas-barthelemy.github.io/2015/08/20/phabricator-arcanist-flow/
[430] https://secure.phabricator.com/book/phabricator/article/arcanist/
[431] https://arxiv.org/html/2203.07473v1
[432] https://www.mediawiki.org/wiki/Phabricator/Differential
[433] https://secure.phabricator.com/book/phabricator/article/differential_land/
[434] https://graphite.com/guides/gerrit-vs-phabricator-key-differences
[435] https://www.phacility.com/phabricator/differential/
[406] https://docs.gitlab.com/user/project/import/github/
[407] https://docs.github.com/en/migrations/using-github-enterprise-importer/migrate-from-gitlab/understand-migrations
[408] https://docs.github.com/en/migrations/using-github-enterprise-importer/migrate-from-gitlab/manage-access
[409] https://docs.github.com/en/migrations/using-github-enterprise-importer/migrate-from-gitlab/plan-your-migration
[410] https://docs.github.com/en/migrations/using-github-enterprise-importer/migrate-from-gitlab/migrate-your-repositories
[411] https://docs.github.com/en/enterprise-cloud@latest/migrations/using-github-enterprise-importer/migrate-from-gitlab/understand-migrations
[412] https://docs.github.com/en/migrations/overview/migration-paths-to-github
[413] https://docs.github.com/en/migrations/using-github-enterprise-importer/migrating-between-github-products/about-migrations-between-github-products
[414] https://docs.gitlab.com/user/project/import/troubleshooting_github_import/
[415] https://docs.gitlab.com/user/import/
[416] https://pennstate.service-now.com/kb_view.do?sysparm_article=KB0020751
[417] https://gitlab.com/gitlab-org/gitlab/-/issues/388716
[418] https://stackoverflow.com/questions/58373698/gitlab-to-github
[419] https://docs.gitlab.com/user/project/integrations/github/
[420] https://github.com/diffblue/gitlab/blob/master/doc/user/project/import/github.md
[361] https://blog.gitea.com/release-of-1.9.0/
[362] https://docs.gitea.com/usage/repository/migration/
[363] https://docs.gitea.com/api/1.23/operations/repo-migrate/
[364] https://docs.gitea.com/api/1.24/operations/repo-migrate/
[365] https://docs.gitea.com/1.24/usage/migration/
[366] https://docs.gitea.com/api/1.25/operations/repo-migrate/
[367] https://dropbars.be/blog/backing-up-github-repos-properly/
[368] https://docs.gitea.com/
[369] https://go-gitea-gitea.mintlify.app/integrations/migrations
[370] https://gitea.com/gitea/migrator
[371] https://docs.codeberg.org/advanced/migrating-repos/
[372] https://github.com/go-gitea/gitea/issues/18369
[373] https://blog.csdn.net/gitblog_00385/article/details/150994423
[374] https://docs.gitea.com/usage/repository/repo-mirror/
[375] https://docs.gitea.com/usage/issues-prs/
[436] https://github.com/pricing
[437] https://github.com/pricing/calculator
[438] https://docs.github.com/en/billing
[439] https://github.com/features/copilot/plans
[440] https://docs.github.com/en/get-started/learning-about-github/faq-about-changes-to-githubs-plans
[441] https://docs.github.com/en/billing/get-started/how-billing-works
[442] https://docs.github.com/en/billing/reference/product-usage-included
[443] https://docs.github.com/en/billing/concepts/product-billing
[444] https://docs.github.com/get-started/learning-about-github/githubs-products
[445] https://userjot.com/blog/github-copilot-pricing-guide-2025
[446] https://docs.github.com/en/billing/concepts/product-billing/github-packages
[447] https://www.getpricepulse.com/companies/github-pricing.html
[448] https://www.getpricepulse.com/blog/github-pricing-2026-complete-guide.html
[449] https://docs.github.com/en/billing/concepts/enterprise-billing/billing-for-enterprises
[450] https://docs.github.com/en/billing/concepts/billing-cycles
