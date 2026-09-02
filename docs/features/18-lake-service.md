# lake.live — the hosted service

> Research: [21-consumer-sync-and-web](../research/21-consumer-sync-and-web/) ·
> [12-artifact-distribution](../research/12-artifact-distribution/) ·
> [20-deployment-runtime-k8s](../research/20-deployment-runtime-k8s/)

**liquid** is the tool. **lake** is where it collects.

`lake.live` is the hosted expression of everything liquid does — a mix of **GitHub,
Google Drive and Railway**, over one content-addressed store.

## Why one service and not three

The three products people would name separately are the same three questions asked
of the same data:

| Surface | Question | Audience |
|---|---|---|
| **Forge** (GitHub-like) | *How did this change?* | Developers |
| **Drive** (Drive-like) | *Where is my stuff?* | Everyone |
| **Platform** (Railway-like) | *Is it running?* | Whoever ships it |

Underneath all three: one chunk store ([01](01-object-model-and-storage.md)), one
CRDT metadata layer ([17](17-implementation.md)), one action graph
([04](04-build-engine.md)), one identity ([19](19-identity.md)).

Every existing stack pays an integration tax between these. GitHub does not know what
your artifact registry holds; your registry does not know what is deployed; your PaaS
re-guesses how to build what your forge stores; none of them are where your
non-developer colleague keeps the spreadsheet that drives the whole thing.

lake is one system because they are one dataset.

## The forge

Repository hosting for liquid, with the parts that make a forge a forge.

**Change review, not pull requests.** liquid records changes with stable identity
([02](02-version-control.md)), so review attaches to a **change**, not to a branch.
That fixes what stacked-diff tooling (Graphite, ghstack, spr) exists to work around:

- A rebase does not orphan the review. Same change identity, same discussion.
- Stacked work is native — a chain of changes, each reviewable independently, without
  a branch per change or a bot rewriting your history.
- Review comments anchor to **cursors** that survive edits and merges, so a comment
  does not detach when the line moves.
- Diffs are structural ([03](03-diff-and-merge.md)), so a reformatting commit shows
  as empty rather than as 4,000 changed lines.

This is closer to Gerrit's one-change-one-review model than to GitHub's, and it is
available because the underlying VCS was designed for it.

**CI is not a separate product.** This is the largest structural difference from every
existing forge. liquid *is* the build system, so:

- There is no `.github/workflows` YAML describing how to build. lake already knows —
  it built it.
- A push does not trigger a fresh container that reinstalls dependencies. It executes
  the actions that are not already cached ([05](05-caching.md),
  [06](06-distributed-execution.md)).
- Most pushes require almost no compute, because most of the graph is unchanged.
- Every result carries provenance automatically ([10](10-supply-chain.md)).

The GitHub Actions cost model — pay per minute for machines that spend most of their
time reinstalling things they already had — is not a pricing decision lake has to
copy. It is an artifact of CI not knowing what the build system knows.

**Also:** issues and discussion, org/team permissions and branch protection, releases,
webhooks and an API, and code search over history and published artifacts
([12](12-search.md)).

## The drive

The same repositories, presented for people who do not think in commits
([15](15-consumer-sync-and-web.md)).

Local sync on Windows (CFAPI), macOS (File Provider) and Linux (FUSE); web browse,
preview, search and share; version history as a query rather than a backup product.

The point is that a designer's assets, a scientist's HDF5 datasets and an engineer's
source live in **one system with one permission model** — and the diff of a
spreadsheet is a real diff ([03](03-diff-and-merge.md)), not a filename suffix.

## The platform

`liquid deploy` targets lake ([13](13-deployment-runtime.md)): environments,
per-change preview environments, managed data services, private networking, volumes,
cron, logs, metrics and traces, and rollback to any prior state.

No buildpacks — lake does not guess how to build a repository, because it built it.

Preview environments are cheap here in a way they are not elsewhere: images share
chunks and builds share a cache, so a per-change environment costs very little
([08](08-packaging-and-images.md)).

## The artifact registry

Everything in [09](09-artifact-service.md), hosted: OCI, npm, PyPI, Maven, apt and
generic views over one deduplicated store, with a transparency log, signed provenance,
and a debuginfod endpoint serving symbols by build-id.

## What lake must not do

The research on forges is unambiguous about where they lose trust:

1. **No lock-in.** History, artifacts, issues, review discussion, permissions and
   deployment configuration are all exportable. What is portable between forges today
   is essentially only git history; everything else is hostage. lake exports the whole
   thing, and self-hosting is a supported configuration rather than a grudging
   enterprise tier.
2. **No pricing that punishes the storage model.** liquid deduplicates aggressively,
   so charging per nominal repository size would bill customers for bytes lake does
   not store. Git LFS quotas — where exceeding a limit silently gives you *pointer
   files instead of your data* — are the specific failure to never reproduce
   ([02](../research/02-large-files-and-binary-assets/)).
3. **No degraded offline.** liquid works fully offline against the local store. lake
   is an accelerator and a meeting point, never a requirement.

## Open questions

- **How much of the forge social layer to build** — stars, follows, discovery, forks
  as a social object — versus treating lake as infrastructure.
- **Federation.** ForgeFed and ActivityPub for forges have not taken off; whether
  lake should attempt cross-instance identity and change proposals, or stay
  centralised with strong export.
- **Migration path from GitHub.** History imports losslessly; issues, PRs and Actions
  do not. How much fidelity is worth building.
- **The economics.** Storage, bandwidth and CI compute are the three costs. liquid's
  deduplication and caching change all three substantially in lake's favour, but egress
  is still egress and the pricing model needs to be designed around what is actually
  consumed.
- **`.live` as a TLD.** Fine for a product domain; slightly more friction than `.com`
  for email deliverability and some corporate filters. Worth knowing before it becomes
  the sender domain for notifications.
