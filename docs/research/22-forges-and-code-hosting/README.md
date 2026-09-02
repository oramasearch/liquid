# Forges and code hosting

> Evidence: [`sources.md`](sources.md)
>
> Context for **[lake.live](../../features/18-lake-service.md)** — a mix of GitHub,
> Google Drive and Railway.

## What a forge actually is

GitHub is four layers, and confusing them is why "just build a Git host" underestimates
the work by an order of magnitude:

1. **Git storage and browsing** — the portable part.
2. **Collaboration metadata** — pull requests, review state, issues, projects. *Not
   portable.*
3. **Execution and artifact services** — Actions, Packages, Codespaces, code search.
4. **Identity, integrations and commerce** — orgs, teams, rulesets, Apps, webhooks,
   Marketplace, security scanning, Copilot.

The research's summary of GitHub's position is precise: *"Its advantage is the
integrated network... Its recurring weaknesses follow from the same design:
repository- and branch-centric abstractions, metered cloud services, centralized
availability, proprietary collaboration metadata and growing Copilot/Microsoft
coupling."*

The strength and the complaints have the same root.

## The central defect: review attached to branches

**GitHub's collaboration abstraction is branch-based, not a persistent logical
change.** Everything below follows from that one decision.

- **Review after rebase breaks.** Rebasing changes commit IDs and line ancestry.
  Conversations go "outdated", commit-linked comments vanish from the review context,
  and "changes since last review" becomes unreliable. Developers specifically report
  being unable to reconstruct what they originally reviewed after a stacked rebase.
- **"Files changed" loses context.** The UI is optimised for the *latest aggregate
  branch delta*, not for comparing patch set 4 with patch set 5 of one logical change.
  Rewrites and base-branch movement detach comments or silently change what the
  reviewer is looking at versus what they approved.

The research states the cause plainly: this is a *"fundamental consequence of
attaching review presentation to moving branch/commit comparisons rather than to
stable change IDs and explicit patch-set versions."*

**Gerrit and Phabricator/Phorge get this right** with one-change-one-review and
explicit patch sets — which is why Google and Android use Gerrit despite its UI. An
entire tooling industry exists to retrofit the model onto GitHub: **Graphite,
ghstack, spr, git-branchless, Sapling's ReviewStack**. Tools whose whole purpose is
working around the host's data model are strong evidence the data model is wrong.

## What people complain about, with specifics

| Complaint | The concrete detail |
|---|---|
| **Actions cost** | Billed by time, OS and machine size; matrices multiply minutes. macOS is especially expensive. A continually occupied standard Linux runner costs **hundreds of dollars a month** at retail minute rates — poor economics for sustained utilisation. |
| **Actions cold starts** | A runner must be allocated and an image prepared; dependency downloads recur unless caching succeeds. Worst for many short jobs, where provisioning and checkout dominate useful execution. |
| **Actions caching** | Cache keys are immutable and branch-scoped in significant ways. Default allowance **10 GB per repository**, LRU eviction, with unused caches historically evicted after a week. Monorepos and large matrices churn the shared cache and pay in both time and storage. |
| **Notifications** | Watching, mentions, review requests and bots feed one high-volume inbox. Users disable nearly everything except direct mentions and still cannot triage by urgency. Event-oriented, not a personal work queue. |
| **Issues vs Jira/Linear** | Good for code-adjacent work; shallower workflow enforcement, portfolio rollups, and reporting (velocity, cumulative flow, cycle time). Note the fair correction: sub-issues, dependencies, custom fields and roadmaps now exist, so "only labels and open/closed" is outdated. |
| **Search is indexed, not exhaustive** | Blackbird excludes vendored/generated code, binaries, non-UTF-8 files, files over **350 KiB**, and very large repositories; long lines truncate; **only default branches** are searched. Excellent interactively; not proof that text does not exist. |
| **Monorepo limits** | GitHub recommends repositories **under 1 GB**, strongly under **5 GB**; files over **100 MiB** blocked without LFS; very large repositories may not be indexed. |
| **LFS quotas** | Free/Pro get **10 GiB storage and 10 GiB bandwidth monthly**, metered beyond. Costly for popular binary releases, media, games, or repeated CI downloads. |
| **Lock-in** | Git history clones cleanly. PR review state, issue event history, Projects, Actions secrets and runs, Apps, Packages, policies and identity mappings do not — **even GitHub-to-GitHub migrations omit categories.** The lock-in is *workflow and metadata*, not source. |
| **Centralised availability** | Local clones protect history, not the control plane. An August 2026 incident delayed PR-triggered workflows and affected mergeability and the merge button. |
| **Microsoft/Copilot coupling** | After GitHub's CEO departed, leadership reported into Microsoft's CoreAI organisation rather than to a replacement CEO. Complaints included being unable to block Copilot-created issues/PRs or disable Copilot review; GitHub withdrew misplaced Copilot "tips" after backlash. The concern is **loss of product neutrality** — forge, editor, cloud, identity and AI as one commercial surface. |

## The alternatives

- **GitLab** — the closest integrated alternative; single application, strong built-in
  CI/CD, credible self-hosting.
- **Gitea / Forgejo** — the practical self-hosted options. Forgejo forked Gitea over
  governance after Gitea moved to a for-profit entity; Codeberg runs Forgejo.
- **sourcehut** — deliberately minimal, email-driven, no JavaScript required. Proof
  that a forge need not be a web application.
- **Gerrit** — change-based review with explicit patch sets. The model to learn from.
- **Phabricator / Phorge** — differential review; Phabricator's upstream ended,
  Phorge continues it.
- **Bitbucket, Azure DevOps** — enterprise-integrated.

## Federation has not worked

**ForgeFed** and ActivityPub-for-forges exist and have not taken off. Worth knowing
before betting on it: the demand for cross-instance identity and change proposals is
real, the delivered systems are not. Strong export is the pragmatic substitute.

## Economics

Three costs: **storage, bandwidth, CI compute**. Bandwidth and compute dominate;
storage is comparatively cheap until binaries arrive. GitHub's model meters compute
and LFS bandwidth precisely because those are where usage is unbounded.

This matters directly for lake, because liquid changes all three: deduplication cuts
storage, chunk-level transfer cuts bandwidth, and a build system that knows what
changed cuts CI compute to near zero on most pushes.

## Implications for liquid and lake

1. **Review attaches to a change, not a branch.** liquid records change identity
   ([01](../01-version-control/)), so review survives rebase, stacked work is native,
   and patch sets are explicit. This is the single clearest product advantage
   available, and it exists only because the VCS underneath was designed for it.

2. **Anchor comments to cursors, not line numbers.** The datastore provides positions
   that survive edits and merges
   ([17-implementation](../../features/17-implementation.md)), so a comment does not
   detach when code moves.

3. **CI is not a separate product with its own YAML.** No `.github/workflows`
   describing how to build — lake already knows, because it built it. Most pushes
   need almost no compute because most of the graph is unchanged
   ([06-build-and-compiler-caching](../06-build-and-compiler-caching/)). The Actions
   cost, cold-start and cache-eviction complaints are all artifacts of CI not knowing
   what the build system knows, and lake does not have to inherit them.

4. **Search must cover more than default branches and small text files.** GitHub's
   350 KiB limit, binary exclusions and default-branch-only indexing are the bar to
   beat, and liquid indexes by blob across history and *inside* artifacts
   ([19-full-text-search](../19-full-text-search/)).

5. **Monorepos and large binaries are the differentiator, not the exception.** GitHub
   advises staying under 1 GB and meters LFS. liquid's chunk store has no equivalent
   cliff ([02-large-files](../02-large-files-and-binary-assets/)), and **must never
   reproduce LFS's failure of handing back pointer files when a quota is hit.**

6. **Export everything, and mean it.** The lock-in is metadata, not source. lake
   exports review discussion, issues, permissions, policies and deployment
   configuration — and self-hosting is a supported configuration, not a grudging
   enterprise tier. This is the credibility argument against an incumbent with a
   network effect.

7. **Do not require the control plane to be up.** liquid works fully offline. lake is
   a meeting point and an accelerator, never a precondition for working.

8. **Product neutrality is a live differentiator.** The Copilot-coupling complaints
   are recent and specific. A forge that does not force an AI surface on its users,
   and does not train on their code without consent, has something to say in 2026.
