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

## Open questions

- **Whether to build a distribution at all, or a layer over k0s/Talos.** Building on
  them is far less work and loses the chunk-aware snapshotter integration.
- **Break-glass design** for an immutable node OS.
- **Multi-tenancy model** — namespaces are not a security boundary, and the
  alternatives (vcluster, separate clusters, microVMs) have very different costs.
- **How much of the PaaS surface to build before the build story is finished.** The
  runtime is the payoff, not the foundation.
