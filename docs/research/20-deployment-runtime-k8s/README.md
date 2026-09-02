# Deployment runtime: minimal Kubernetes and PaaS

> Evidence: [`sources.md`](sources.md)
>
> **Scope:** a minimal k8s distribution in the spirit of k0s, carrying
> Railway.com-like developer features. This is liquid's delivery target — the place
> the artifacts it builds actually run.

## "Minimal Kubernetes" means two different things

The research is clear that these are separate axes, and conflating them causes bad
architecture decisions:

- **Packaging the control plane more conveniently** — k0s, k3s, MicroK8s.
- **Minimising and locking down the host OS** — Talos Linux, Bottlerocket.

Bottlerocket is *not* a control-plane distribution at all; it is a node OS. Talos
supplies both. Choosing where liquid sits on both axes is the first decision.

| Distro | Model | Datastore | Notable |
|---|---|---|---|
| **k0s** | One statically linked binary that embeds and supervises components. Controllers run control-plane components as **host processes with no kubelet or container engine**, so workloads cannot land on them. `k0sctl` manages hosts over SSH. | Embedded etcd by default; **Kine** gives an etcd-compatible API over SQLite/Postgres/MySQL/dqlite | ~1 vCPU/1 GB controller, 0.5 GB worker. No RPM/snap/runtime dependencies. Cleanest, least opinionated. |
| **k3s** | Server/agent. Servers are schedulable by default. Bundles containerd, CoreDNS, Traefik, local-path storage, metrics-server, Flannel, ServiceLB. | Kine over embedded SQLite (single server); embedded etcd or external SQL for HA | 2 cores/2 GB server, 1 core/512 MB agent. "Batteries included" creates **hidden coupling** — packaged Traefik/ServiceLB/Flannel conflict with replacements and change across upgrades. |
| **MicroK8s** | One snap, add-on system. | **dqlite** (Raft-distributed SQLite) | ~540 MB advertised. dqlite is the single largest recurring operational complaint — high CPU/disk, lost quorum, hard recovery. |
| **Talos** | Immutable API-driven OS. **No SSH, no shell, no package manager.** `talosctl` over mutually authenticated gRPC; everything declared in a machine config. | Embedded etcd | Under 100 MB OS. Upgrades install a complete signed image and reboot. Debugging requires serial/IPMI when the API is unreachable. |
| **kubeadm** | The baseline. | etcd | Useful mainly as a measure of how much a distribution must otherwise assemble. |

**The most important finding for liquid:** *"A credible new distribution is mostly a
lifecycle-integration project."* Running `kube-apiserver` is easy. Safely
coordinating networking, storage, certificates, upgrades, backup/restore, node
replacement, observability and multi-tenancy is the hard part — and that is where
all the effort goes.

Two design choices worth stealing outright:

- **k0s's controller isolation** — control-plane components as host processes with
  no kubelet means workloads structurally cannot be scheduled onto controllers. A
  security and stability property obtained by construction rather than by policy.
- **Talos's immutable, API-only host** — no SSH, no shell, no drift. This aligns
  perfectly with liquid's content-addressed, reproducible worldview: a node runs a
  signed image identified by hash, and configuration is declarative data.

The tension between them is real: Talos's model is stronger, and it is also the one
whose users complain loudest about losing break-glass access. Any adoption of it
needs a genuine answer for "the API is unreachable, now what?"

## What Railway actually provides

Worth being precise, because the product is not "hosted Kubernetes":

> *Railway's important product is the integrated control loop from repository
> event → detected build → image → release → networking/database/volume/
> observability UI.*

Concretely: git push to deploy; automatic build detection (Nixpacks, now Railpack);
managed-looking Postgres/Redis/Mongo templates; environments including per-PR
environments; private networking; volumes; cron; observability; usage-based
pricing.

One caveat the research surfaces and that liquid should not repeat naively:
Railway's standard database offerings are explicitly **unmanaged services** —
backup, tuning, security and maintenance are the customer's problem. The *feel* of
managed services is doing a lot of work there.

The competitive landscape: Render and Heroku take the same "own the whole common
path" approach; Fly.io won a more infrastructure-minded audience with
understandable VM-level primitives (Firecracker microVMs, Machines) *without*
requiring Kubernetes APIs; Coolify, Dokku, CapRover and Porter serve the
self-hosted end; Knative, KubeVela, Crossplane and the Open Application Model are
the Kubernetes-native attempts.

## Why Kubernetes-native PaaS attempts underperformed

The research's diagnosis is sharp and matches experience:

> *Kubernetes-based PaaS efforts often exposed either too much Kubernetes or an
> abstraction with no escape hatch.*

Both failure modes are fatal. Expose too much and you have not actually removed the
complexity — the user still needs to understand ingress, CSI and RBAC. Abstract
completely with no escape hatch and the first non-standard requirement is a wall.

Railway, Render and Heroku won by **owning the entire common path** so that the
90% case needs no Kubernetes knowledge at all. Fly.io won by offering primitives
simple enough to reason about directly.

## The hard operational parts

A new distribution must make defensible choices on all of these, and each is a
project:

- **CNI** — Cilium/eBPF, Calico, Flannel.
- **Ingress** — Traefik, nginx, Envoy Gateway, and the Gateway API transition.
- **Storage/CSI** — local-path, Longhorn, OpenEBS, Rook/Ceph.
- **Certificates, secrets, multi-tenancy and namespace isolation.**
- **Autoscaling** — HPA/VPA/KEDA, Karpenter.
- **Observability** — Prometheus, OpenTelemetry.

k3s's experience is the warning: bundling defaults is what makes it pleasant *and*
is the source of its worst complaints, because bundled components change across
upgrades and fight replacements. k0s's approach — bundle less, make selection
deliberate — trades initial convenience for long-term predictability.

## The build-to-deploy path

Buildpacks (CNB/pack/kpack/Paketo, Nixpacks, Railpack) exist to answer "what is
this repository and how do I build it?" without a Dockerfile. They are heuristic,
which is both their appeal and their limit.

**liquid does not need heuristics.** It already has an exact dependency graph, an
exact toolchain and an exact set of actions. Where a buildpack *guesses* that this
is a Node project needing `npm ci`, liquid *knows* what the build is, because it
built it. That is a substantially stronger position than any existing PaaS occupies.

## Implications for liquid

1. **This is the payoff for everything else, not a separate product.** The chain
   source → objects → packages → image → running process is exactly liquid's
   "changes form" thesis. The k8s distro is where that chain terminates, and it is
   what makes incremental builds observably valuable: a one-line change becomes one
   recompiled object, one changed chunk, one new image, one rolling update — in
   seconds.

2. **Follow k0s on packaging and Talos on the host.** A single static binary with
   no runtime dependencies, controllers isolated from workloads by construction, and
   an immutable API-driven node OS whose image is content-addressed and signed by
   liquid ([13](../13-supply-chain-security/)). But provide a real break-glass
   path — Talos users' loudest complaint is losing it.

3. **Bundle deliberately, and version the bundle explicitly.** k3s's hidden coupling
   is the failure to avoid. Pick opinionated defaults (Cilium, Gateway API,
   local-path plus one distributed option) but make them *replaceable components with
   declared versions* rather than implicit parts of the distribution that shift
   under users at upgrade time.

4. **Skip buildpacks entirely.** Build from liquid's graph. This removes the whole
   class of "the buildpack guessed wrong" problems and gives every deployment exact
   provenance for free.

5. **Own the common path completely, and keep a real escape hatch.** The diagnosed
   failure of Kubernetes-native PaaS. `liquid deploy` should require no Kubernetes
   knowledge; underneath, it must remain ordinary Kubernetes that an operator can
   inspect and override with `kubectl`.

6. **Chunk-aware image distribution to the nodes.** Since liquid's store is
   content-defined chunks and it may control the snapshotter, Nydus/RAFS-style
   chunk-addressed lazy pulling ([11](../11-container-images/)) is available in a
   way it is not to a general-purpose distro. A node fetches only the chunks it does
   not already have — across images, not just within one.

7. **Environments and PR previews are cheap here.** They are the feature developers
   love most about Railway, and they are nearly free when images share chunks and
   builds share a cache. This is a case where liquid's storage model produces a
   product advantage directly.

8. **Be honest about managed services.** Railway's databases being unmanaged is a
   real gap that bites users. Either operate them properly — backup, restore,
   upgrade, failover, all tested — or label them accurately.

9. **Hot update is the long-term differentiator.** The runtime ambition in
   [17-runtime-hot-update](../17-runtime-hot-update/) lands here: updating a running
   workload in place from an incremental build, rather than rolling a new pod. Defer
   it, but let the artifact and deployment model express it.
