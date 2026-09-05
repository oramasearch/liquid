# Deployment runtime

> Research: [20-deployment-runtime-k8s](../research/20-deployment-runtime-k8s/)
>
> A minimal Kubernetes distribution in the spirit of k0s, with Railway-style
> developer features. This is where liquid's chain terminates.

## Why this belongs in liquid

Source → objects → packages → image → running process is the whole thesis. The
runtime is where incremental builds become observably valuable: a one-line change
becomes one recompiled object, a handful of new chunks, one new image and one rolling
update — in seconds rather than minutes.

## The distribution

**Decided: a private copy of k0s, modified.** Not built from scratch, and not a public
fork.

[`oramasearch/k0s`](https://github.com/oramasearch/k0s) is a private copy of
[`k0sproject/k0s`](https://github.com/k0sproject/k0s) kept in sync with upstream, with
our changes isolated on a `liquid` branch so `git diff main..liquid` is always exactly
our delta. k0s is **Apache 2.0** (its `docs/` is CC-BY-SA-4.0), so private modification
carries no publication obligation; distributing modified binaries requires retaining
`LICENSE` and `NOTICE` and stating that files were changed. The branch model satisfies
the last of those by construction.

Rationale: building a control plane is not the hard part — *"a credible new distribution
is mostly a lifecycle-integration project."* Starting from k0s buys the lifecycle work
and leaves us to do the part that is actually differentiated, which is the chunk-aware
node integration below.

See `ORAMA-FORK.md` in that repository for the branch model and sync procedure.

## What we inherit

Two axes, deliberately separated because conflating them causes bad decisions.

**Control plane — follow k0s.** A single statically linked binary with no runtime
dependencies. Controllers run control-plane components as **host processes with no
kubelet**, so workloads structurally cannot be scheduled onto them. That is a
security and stability property obtained by construction rather than by policy.

**Node OS — follow Talos.** Immutable, API-driven, no SSH and no package manager,
configured declaratively. A node runs a signed image identified by hash — which is
exactly liquid's worldview, and liquid produces and signs that image itself
([08](08-packaging-and-images.md), [10](10-supply-chain.md)).

**But keep a real break-glass path.** Losing shell access is Talos users' loudest
complaint, and "the API is unreachable" needs an answer better than IPMI.

**Storage:** embedded etcd by default, with a Kine-style option over an external
database for small deployments.

## Bundle deliberately, version explicitly

k3s is pleasant because it bundles Traefik, ServiceLB, Flannel and local-path
storage — and its worst complaints come from exactly that, because bundled components
shift under users at upgrade time and fight replacements.

liquid picks opinionated defaults — Cilium/eBPF, Gateway API, local-path plus one
distributed option — but ships them as **replaceable components with declared
versions**, not as implicit parts of the distribution.

The research's blunt conclusion applies: *"A credible new distribution is mostly a
lifecycle-integration project."* Running `kube-apiserver` is easy. Networking,
storage, certificates, upgrades, backup/restore, node replacement, observability and
multi-tenancy are the work.

## The developer experience

```
liquid deploy
```

No Kubernetes knowledge required for the common path. What that provides:

- Deploy on change, from the repository.
- Environments, including per-change preview environments.
- Managed data services, private networking, volumes, cron.
- Logs, metrics and traces.
- Rollback to any prior state.

**No buildpacks.** Railway, Render and Heroku use heuristics (Nixpacks, Railpack,
CNB) to guess how to build a repository. liquid does not guess — it *knows*, because
it built it ([04](04-build-engine.md)). Every deployment carries exact provenance for
free, and the entire class of "the buildpack guessed wrong" problems disappears.

## The failure to avoid

Kubernetes-native PaaS attempts failed by exposing too much Kubernetes, or by
abstracting completely with no escape hatch. Both are fatal: expose too much and the
complexity was never removed; abstract totally and the first non-standard requirement
is a wall.

liquid **owns the entire common path**, and underneath remains ordinary Kubernetes
that an operator can inspect and override with `kubectl`.

## Chunk-aware distribution to nodes

Because liquid's store is content-defined chunks and liquid may control the
snapshotter, nodes fetch only chunks they lack — **across images, not just within
one** (the Nydus/RAFS model, [08](08-packaging-and-images.md)).

This is available to liquid in a way it is not to a general-purpose distribution, and
it is what makes preview environments cheap: images share chunks, builds share a
cache, so a per-change environment costs very little.

## Managed services, honestly labelled

Railway's standard databases are explicitly **unmanaged** — backup, tuning, security
and maintenance are the customer's problem — while the product *feels* managed. That
gap bites users.

liquid either operates a data service properly, with backup, restore, upgrade and
failover all tested, or labels it accurately. No implied guarantees.

## Secrets

Referenced from the repository, minted at deploy time
([11](11-secrets-and-encryption.md)). One-way delivery to a cluster, in the manner of
Sealed Secrets, so committing a secret does not require the author to be able to read
it back.

**Implemented in liquid #204:** liquid's versioned canonical-CBOR envelope uses a fresh X25519
ephemeral key and XChaCha20-Poly1305 for each value. The authenticated scope binds the target
cluster and, when supplied, namespace and secret name exactly. The public API and CLI seal to a
Lake environment's published key; the operator-side unseal and Lake capability remain companion
work. See [the byte-level format and fixture](../library/sealed-secrets.md).

## Open questions

- ~~Whether to build a distribution at all.~~ **Resolved:** a private, synced copy of
  k0s ([`oramasearch/k0s`](https://github.com/oramasearch/k0s)), modified on a `liquid`
  branch. Still open is how much of Talos's immutable-node model to adopt on top.
- **Break-glass design** for an immutable node OS.
- **Multi-tenancy model** — namespaces are not a security boundary, and the
  alternatives (vcluster, separate clusters, microVMs) have very different costs.
- **How much of the PaaS surface to build before the build story is finished.** The
  runtime is the payoff, not the foundation.
