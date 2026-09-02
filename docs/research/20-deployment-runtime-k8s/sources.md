<!-- Raw research source. Generated via Perplexity `perplexity_research` (web-grounded,
     multi-source) on 2026-09-01 for the liquid project. Lightly reformatted only.
     Treat as evidence, not as liquid design decisions - see README.md in this
     directory for the curated analysis and the implications drawn for liquid. -->

# Technical research report: minimal Kubernetes and PaaS-on-Kubernetes, 2024–2026

## Executive conclusions

1. **“Minimal Kubernetes” has two different meanings.** k0s, k3s and MicroK8s package Kubernetes more conveniently; Talos Linux and Bottlerocket minimize and lock down the **host OS**. Bottlerocket is not itself a Kubernetes control-plane distribution, while Talos supplies an OS and Kubernetes lifecycle together.
2. **k3s is the most batteries-included small-cluster distribution; k0s is the cleaner, less opinionated package; MicroK8s is the Ubuntu/snap-centric option.** kubeadm remains the useful baseline because it exposes how much a distribution must otherwise assemble.
3. **Railway is a developer PaaS, not merely hosted Kubernetes.** Its important product is the integrated control loop from repository event → detected build → image → release → networking/database/volume/observability UI. Its PostgreSQL, Redis and MongoDB offerings are convenient Railway-maintained templates, but the standard offerings are explicitly **unmanaged services**, leaving backup configuration, tuning, security and maintenance to the customer. [web:95]
4. **A credible new distribution is mostly a lifecycle-integration project.** Installing `kube-apiserver` is comparatively easy; safely coordinating networking, storage, certificates, upgrades, backup/restore, node replacement, observability and multi-tenancy is the hard part.
5. **Kubernetes-based PaaS efforts often exposed either too much Kubernetes or an abstraction with no escape hatch.** Railway, Render and Heroku won affection by owning the whole common path; Fly.io won a more infrastructure-oriented audience by offering understandable VM-level primitives without requiring Kubernetes APIs.

---

## 1. Minimal Kubernetes distributions

Footprint numbers below are documented minimums or advertised idle footprints, not directly comparable benchmarks. Actual use depends heavily on etcd history, CNI, metrics, ingress and workloads.

| Distribution / baseline | Architecture and roles | Footprint / dependencies | Control-plane storage | HA model | Upgrade mechanism | Recurring complaints / trade-offs |
|---|---|---|---|---|---|---|
| **k0s** | One statically linked, self-extracting binary embeds and supervises Kubernetes components. A controller normally runs control-plane components as host processes without kubelet or a container engine, so workloads cannot accidentally land there; workers run kubelet/containerd. Combined controller-worker and single-node modes are available. `k0sctl` installs and manages hosts over SSH. [web:64][web:67] | No RPM, snap or external runtime dependencies beyond a suitable host kernel. Current documented minima are roughly 1 vCPU/1 GB for controller or combined node, 0.5 GB RAM for worker; k0s disk use is about 0.5 GB controller, 1.3 GB worker or 1.7 GB combined. [web:484] | Managed embedded **etcd by default**. Embedded Kine provides an etcd-compatible API over SQLite, PostgreSQL, MySQL or dqlite; external SQL can therefore place state outside controllers. [web:61] | Usually three or more controllers with co-located managed etcd behind an external API load balancer. k0s adjusts etcd membership when controllers join; external HA PostgreSQL/MySQL is another model through Kine. [web:61][web:74] | Replace the binary/node version directly, use the autopilot controller, or declare a version in `k0sctl.yaml` and run `k0sctl apply`, which performs a coordinated rollout. [web:68] | Fewer built-in application services than k3s: ingress, load balancer and durable storage still need deliberate selection. Users report SSH/interface-validation problems in k0sctl and cases where changed Helm-extension values were not reapplied as expected. [web:423][web:433] |
| **k3s** | One distribution binary uses a **server/agent** model. Server nodes run API server, scheduler, controller manager and datastore and are schedulable by default; agents run workloads. It embeds containerd and packages CoreDNS, Traefik, local-path storage, metrics-server, Flannel and ServiceLB/Klipper unless disabled. [web:1][web:5] | Current supported minima are 2 cores/2 GB RAM for a server and 1 core/512 MB for an agent. Its original design goal was Kubernetes for resource-constrained, low-touch edge, ARM, IoT and CI environments rather than datacenter-only installations. [web:494][web:14] | Single-server default is **Kine over embedded SQLite**. SQLite cannot support multiple server nodes. HA uses embedded etcd, or Kine with external PostgreSQL/MySQL; external etcd is also supported. [web:2][web:3] | Embedded-etcd HA requires at least three server nodes. External-database HA can use two or more servers plus a fixed registration/API endpoint, with database HA delegated to the external service. [web:1][web:4] | Re-run the installer or replace the `k3s` binary and restart nodes sequentially; SUSE’s system-upgrade-controller can orchestrate plans across servers and agents. Manual replacement is simple but flags must remain consistent. [web:434] | “Batteries included” creates hidden coupling: packaged Traefik, ServiceLB, Helm controller and Flannel can conflict with replacements or change during upgrades. Reported failures include Traefik routing regressions, upgrade pods killing their own k3s process and iptables/networking regressions. [web:422][web:425][web:426] |
| **MicroK8s** | Upstream Kubernetes services packaged as one snap, with a `microk8s` command namespace and an extensive enable/disable add-on system. It uses containerd; current launch defaults include dqlite and Calico, although older/non-HA modes have used Flannel. [web:451][web:458] | Canonical advertises operation in about **540 MB**, but recommends 4 GB RAM and 20 GB disk for workloads. It is closely tied to snap packaging and is most natural on Ubuntu. [web:456] | **dqlite**, a Raft-distributed SQLite derivative, replaces etcd in the normal HA design. | At three or more nodes, all nodes can run control-plane services while at least three voting nodes maintain dqlite replicas; additional nodes can hold non-voting spare copies. [web:76] | Snap channels pin Kubernetes minor versions. `snap refresh --channel=<version>/stable`, normally one drained node at a time, performs upgrades; unattended snap refresh behavior must be controlled operationally. [web:82] | Strong opinions around snaps and automatic refreshes; dqlite is the largest recurring operational complaint. Reported incidents include high CPU or disk writes, lost quorum, difficult recovery and a read-only-looking API after datastore failure. [web:86][web:397][web:402][web:405] |
| **Talos Linux** | A purpose-built, immutable Kubernetes OS and lifecycle system, not merely a repackaged binary. There is no SSH daemon, login shell, package manager or console login. `talosctl` talks to a mutually authenticated gRPC API; networking, disks, kubelet, control-plane settings and extensions are declared in a machine-config document. [web:33][web:38] | Talos itself uses under 100 MB of disk, but documented practical minima are 2 GiB/2 cores/10 GiB for control plane and 1 GiB/1 core/10 GiB for a worker; more is recommended for production. [web:42] | Conventional embedded **etcd** on control-plane nodes. [web:35] | Typically three control-plane machines with etcd quorum and a stable API endpoint; workers are separate. The declarative OS removes host drift but does not remove etcd quorum or load-balancer requirements. | `talosctl upgrade` installs a complete signed OS image and reboots nodes; `talosctl upgrade-k8s` coordinates Kubernetes component upgrades. Control planes are upgraded one at a time with quorum checks. [web:37][web:45] | The security model changes debugging habits: if API networking is broken, operators may require serial/IPMI/video console access. Teams also complain about learning machine-config patches and losing familiar shell/package-manager break-glass procedures. [web:398] |
| **Bottlerocket** | An immutable, API-driven container host from AWS. Kubernetes variants run kubelet and a runtime, but Bottlerocket does **not** create or manage the Kubernetes control plane. It has no SSH server or host shell; administrative and control containers provide constrained break-glass access. [web:46][web:57] | Very small read-only host surface, dm-verity-protected root and persistent data partition. Exact consumption varies by cloud/architecture variant; it should be compared with Talos as a node OS, not with k3s as a cluster distribution. | None: supplied by EKS, kubeadm, Cluster API or another Kubernetes distribution/control plane. | Node availability comes from node groups, autoscaling and disruption budgets; Kubernetes control-plane HA belongs to the surrounding platform. | Full filesystem images replace the inactive A/B partition, then reboot; failed boots can roll back. Updates are API-driven and can be automated by the Bottlerocket update operator/Brupop. [web:49][web:53][web:59] | AWS/EKS-oriented variants and lifecycle; restricted host customization; no normal shell or package manager; troubleshooting often requires control/admin containers. These are deliberate security choices but surprise conventional Linux operators. |
| **kubeadm baseline** | Upstream bootstrap tool, not a complete distribution. It writes certificates and static-pod manifests and initializes or joins control-plane/worker nodes. The operator supplies the Linux OS, container runtime, CNI, ingress, storage, load balancer, backup and node-management automation. [web:22][web:30] | Official setup guidance calls for at least 2 GiB RAM per host and two CPUs on a control-plane host, plus a preinstalled CRI runtime. [web:22] | Standard **etcd**, either stacked as a static pod on each control-plane node or in a separate external etcd cluster. [web:18] | Stacked topology normally uses three control-plane/etcd nodes behind a load balancer. External topology separates at least three etcd hosts from multiple control-plane hosts, improving failure isolation at higher cost. | Install the new kubeadm package, run `kubeadm upgrade plan/apply` on the first controller and `upgrade node` elsewhere, then drain and upgrade kubelet/kubectl. External-etcd upgrades remain an operator responsibility. [web:21][web:27] | Most transparent but most labor-intensive: package repositories, runtime, kernel/sysctl setup, CNI, certificate renewal, etcd backup, load balancing and every add-on lifecycle remain separate operator concerns. |

### Practical selection

| Requirement | Most defensible choice | Why |
|---|---|---|
| Small edge/site cluster with useful defaults | **k3s** | Lowest integration effort; local-path, ingress, DNS, metrics, ServiceLB and runtime are present immediately. |
| Minimal but relatively neutral Kubernetes package | **k0s** | Clean controller/worker separation, zero host-package dependencies and flexible Kine/external-database support. |
| Ubuntu appliance, workstation or Canonical estate | **MicroK8s** | Snap channels and enable/disable add-ons integrate well with Canonical operations. |
| Secure immutable bare-metal/cloud Kubernetes | **Talos Linux** | Declarative machine API manages both host OS and Kubernetes lifecycle without SSH drift. |
| Locked-down worker OS for EKS or an existing control plane | **Bottlerocket** | Small immutable attack surface and image-based A/B updates. |
| Maximum upstream transparency or distribution engineering baseline | **kubeadm** | Makes every missing architectural choice explicit. |

---

## 2. Railway and competing developer PaaS platforms

### Hosted and self-hosted developer platforms

| Platform | What it actually provides | Build/deploy model | Data, networking and operations | Best fit / principal trade-off |
|---|---|---|---|---|
| **Railway** | Projects contain services and named environments; GitHub pushes, `railway up`, templates or registry images create deployments. PR deployments can receive ephemeral environment-specific configuration. Logs are aggregated in an environment-level observability view. [web:91][web:101][web:105] | A root `Dockerfile` always wins; otherwise **Railpack**, the successor to Nixpacks, detects language/framework, generates a build plan and packages an OCI image. GitHub webhooks trigger builds, with monorepo watch paths controlling whether a change is relevant. [web:376][web:378][web:380][web:381] | Private service DNS, public domains/TCP proxy, persistent volumes, cron executions, logs/metrics and Railway-maintained PostgreSQL/Redis/Mongo templates. Crucially, normal database templates are **unmanaged**, even though backups and HA templates exist. [web:95][web:103][web:389] | Excellent “repository to running service” UX and flexible per-second CPU/RAM/storage billing. Current published rates are $10/GB-RAM-month, $20/vCPU-month, $0.15/GB-month volume and $0.05/GB egress, metered by actual use. Costs can be less predictable than fixed instances. [web:92][web:96] |
| **Render** | Heroku-like web services, private services, workers, static sites, cron jobs, managed PostgreSQL/Key Value, persistent disks, private networking, autoscaling, Blueprints and full-stack preview environments. [web:137][web:143][web:147] | Git-triggered native language runtimes, Dockerfile builds or prebuilt images. Blueprint YAML describes multi-service environments. [web:145] | More conventionally “managed” application/database product than Railway’s database containers; compute and storage are per-service and prorated by the second, plus workspace and bandwidth charges. [web:140][web:146] | Production-minded Heroku replacement with predictable service sizes; less globally distributed and less low-level than Fly. |
| **Fly.io** | Global placement of **Fly Machines**, individually addressable, fast-starting Firecracker microVMs controlled through CLI/API; Anycast proxy and private WireGuard networking. [web:121][web:126][web:134] | Usually builds from a Dockerfile/build configuration or deploys an existing image. Machines can stop/start in subsecond time rather than being Kubernetes pods hidden behind a PaaS API. | Local persistent volumes, regional placement and **LiteFS**, which transparently replicates SQLite. Managed data products are less “one-click PaaS database” oriented than Heroku/Render. [web:132] | Best when developers want global topology, VM isolation, custom processes and some infrastructure control. More concepts—regions, Machines, volumes, allocation and replication—than Railway or Render. |
| **Heroku** | Canonical `git push` PaaS: dynos/process types, config vars, add-on marketplace, managed Postgres/Key-Value, pipelines, promotions, Review Apps and enterprise Private Spaces. [web:166][web:167] | Heroku buildpacks detect code; Container Registry also accepts Docker images, and Dockerfiles can be built and deployed. [web:168][web:169] | Mature managed data/add-on ecosystem and operational conventions, but higher production pricing and less infrastructure flexibility. | Still the clearest model for twelve-factor applications; strongest convention and ecosystem, but cost and product pace drove users toward newer alternatives. |
| **Vercel** | Git-integrated frontend platform with immutable preview deployments, CDN/edge delivery, Functions, Fluid Compute, analytics/observability and managed Blob/KV/database integrations. | Framework-aware builds—especially Next.js—and automatic preview/production promotion rather than general-purpose daemon/container orchestration. | Global request routing and function execution; not a hosted Kubernetes product and not ideal for arbitrary long-running stateful containers. Vercel explicitly says it neither runs nor hosts user Kubernetes clusters. [web:118] | Best frontend and full-stack web experience; weaker fit for generic workers, custom networking, stateful services or arbitrary runtimes. |
| **Coolify** | Open-source, self-hosted control panel/PaaS for Docker-compatible applications, databases and one-click services across the user’s own servers. GitHub/GitLab/Bitbucket/Gitea integrations and GitHub PR previews are supported. [web:151][web:160] | Dockerfile, Docker Compose, static/Nixpacks-style application builds and existing images, depending on resource type. | The owner retains servers, backups, availability and security; Coolify automates proxying, TLS and deployment but is not the infrastructure operator. | Attractive for sovereignty and lower software cost; operational burden merely moves back to the user. |
| **Dokku** | Small, plugin-driven, Heroku-like PaaS, traditionally on one server. `git push dokku@host:app` builds and releases Docker containers; current architecture can use local Docker or a k3s scheduler. [web:153] | Herokuish buildpacks by default; a root Dockerfile takes precedence unless buildpack configuration overrides it. [web:152][web:161] | Plugins supply PostgreSQL/Redis, TLS, storage, schedulers and proxies. HA, backups and upgrades are assembled by the operator. | Excellent minimal self-hosted PaaS and teaching tool; less suitable as a turnkey multi-node production control plane. |
| **CapRover** | Self-hosted Docker Swarm PaaS with dashboard, CLI/webhook/Git deployment, NGINX routing, TLS, one-click apps and multi-node Swarm support. [web:163] | Builds source into images or deploys existing images/Compose-style one-click definitions. | Databases and monitoring are deployable applications, not externally managed services; the user owns Swarm and storage reliability. | Simpler than Kubernetes and easy for a few servers, but tied to Docker Swarm and a smaller operational ecosystem. |
| **Porter** | Kubernetes-powered PaaS that provisions Kubernetes in the customer’s cloud or attaches an existing cluster, then deploys from Git or a registry. [web:211][web:224] | GitHub Actions builds Dockerfiles or buildpacks, pushes images and deploys them into the attached cluster. [web:220] | Keeps workloads and cloud bill in the customer account while adding application UI, environments and operational automation. | Good bridge for teams wanting Kubernetes ownership without exposing it to every developer; still inherits cluster/cloud complexity and a second control-plane product. |
| **Northflank** | Managed or bring-your-own-cloud platform for builds, services, jobs, databases, volumes, pipelines and releases. Preview Blueprints create complete isolated PR environments including services, databases and URLs. [web:214][web:215] | Separate build and deployment services; Dockerfile or buildpack, registry artifact, and graph/Blueprint-controlled release triggers. | Strong multi-service environment modeling, private/public networking and managed add-ons. | Closest broad competitor to Railway for complex backend platforms; more explicit platform concepts can mean more setup but also more control. |

### Why Railway’s wording matters

Railway offers a very polished **database deployment experience**, but it should not automatically be equated with AWS RDS, Heroku Postgres or Render Postgres. Railway’s documentation says its templates are maintained and preconfigured by Railway, yet the normal services remain customer-operated for backup policy, disaster recovery, tuning, access control, monitoring and maintenance. [web:95]

---

## 3. Kubernetes-native application-platform building blocks

These products are complementary rather than direct Railway replacements.

| Kubernetes-native component | Layer and technical role | What it does not solve |
|---|---|---|
| **Knative Serving / Eventing** | Revisioned serverless services, routing and event integration. Knative Pod Autoscaler can scale HTTP workloads to zero and activate them on new traffic. [web:196][web:198] | Source builds, databases, durable volumes, broad developer portal, cluster lifecycle or general stateful applications. |
| **KubeVela** | Application delivery control plane implementing an OAM-style `Application`: components plus operational traits, policies and workflows, rendered using CUE across clusters/cloud resources. [web:202][web:204] | It adds another API and definition language; platform teams must design and maintain useful abstractions. |
| **Crossplane** | Extends Kubernetes as a cloud/resource control plane. Managed Resources represent external resources; Compositions combine them into reusable platform APIs. [web:208][web:209] | Application source build, runtime logs, request routing and a complete Heroku/Railway UX. |
| **Shipwright** | Kubernetes CRDs for builds: `Build` specifies source, strategy and output; namespace/cluster `BuildStrategy` describes how; `BuildRun` invokes an in-cluster build, typically as a pod/job. [web:182][web:183][web:184] | Release promotion, runtime reconciliation and application operations after the image is produced. |
| **Cloud Native Buildpacks (CNB)** | Standardized lifecycle: analyze previous image, detect compatible buildpacks, restore cache, build and export an OCI image. Build and runtime layers are separated, enabling rebasing of compatible run images. [web:187][web:188] | Build triggers, registry hosting and runtime deployment. |
| **`pack`** | Developer/operator CLI that runs the CNB lifecycle against a chosen builder image. | Persistent in-cluster build controller or application deployment. |
| **kpack** | Kubernetes controller that continuously builds OCI images from source using CNB builders, stores and stacks/run images; useful for rebuilding when source or builder changes. [web:191] | Runtime deployment and environment management. |
| **Paketo Buildpacks** | Production buildpack implementations/builders for common languages, suitable for `pack`, kpack or other CNB platforms. | Triggering, registry, deployment and Kubernetes operations. |
| **Argo CD** | Application-oriented declarative GitOps CD. An `Application` points to Git/Helm/Kustomize and a target cluster; the controller compares desired and live state and synchronizes it. [web:226][web:227] | Source-to-image build is intentionally separate; automated image-tag updates generally need another controller or Git commit. |
| **Flux** | Composable source, Kustomize, Helm, notification and image-automation controllers. Flux can scan registries, select tags, commit manifest changes and reconcile the resulting Git state. [web:226][web:450] | Turnkey PaaS UI, source build and managed application services. |
| **Open Application Model (OAM)** | Runtime-neutral conceptual model separating components/workloads from operator-supplied traits and scopes. A platform chooses which component and trait types it supports. [web:244][web:246] | It is a specification, not a runtime. Adoption outside KubeVela and a few implementations has remained limited, partly because custom abstractions do not automatically become portable between platforms. [web:249] |

---

## 4. Build-to-deploy path

### Build mechanism comparison

| Build mechanism | Detection and execution | Advantages | Failure modes / governance implications |
|---|---|---|---|
| **Cloud Native Buildpacks** | Each buildpack runs a detect contract; the first valid ordered group supplies a build plan, layers and process metadata. The lifecycle produces an OCI image. [web:188][web:193] | Very low application configuration; centrally governed language versions and secure run images; reusable dependency layers; SBOM support; run-image rebasing can patch base OS layers without recompiling application code. | Detection can be opaque; unusual native dependencies or monorepos need configuration/custom buildpacks; platform team owns builder compatibility and update cadence. |
| **Dockerfile** | Repository explicitly defines base image and every build step; BuildKit or another builder executes it. | Universal escape hatch, easy to reproduce locally, maximum control over native tools, multi-stage builds and runtime. | Every team owns base-image pinning, cache design, root/user settings and patching. Mutable tags and unpinned package installs reduce reproducibility; copying secrets into layers is easy to do accidentally. |
| **Nix / Nixpacks** | Nixpacks analyzes files, merges provider and user configuration into setup/install/build/start phases, installs Nix/Apt dependencies and emits an OCI image through BuildKit. The plan can be serialized and reused. [web:406][web:413] | Broad package catalog and comparatively reproducible system dependencies without hand-written Dockerfiles. | Nix adds evaluation/store complexity and large closures; framework detection can be surprising. Railway consequently replaced Nixpacks with Railpack after operating it at scale. [web:411][web:418] |
| **Railpack** | Detects source, creates a structured build plan and translates the plan into a BuildKit build graph, packaging an optimized OCI image. It is Railway’s default unless a Dockerfile is found. [web:378][web:383] | Keeps buildpack-like zero configuration while giving Railway tighter caching and BuildKit integration than Nixpacks. | Railway-specific defaults can still mis-detect unconventional repositories; Dockerfile remains the explicit escape hatch. |
| **Prebuilt image** | CI signs and pushes an image identified ideally by immutable digest; platform only releases that artifact. | Separates build trust from runtime, supports “build once, promote exactly the same digest,” and works with any language/toolchain. | Requires registry credentials, retention/GC, vulnerability/signature policy and a reliable event linking new digest to desired deployment. |

### Recommended end-to-end control loop

| Stage | Platform decision and trigger | Recommended implementation |
|---|---|---|
| **1. Observe source** | Git push, merged PR, release tag, manual command or scheduled rebuild. Apply branch rules and monorepo watch paths before spending build resources. Railway, for example, skips commits outside configured watch paths. [web:377] | Record repository, commit SHA, subdirectory and configuration revision as immutable build inputs. |
| **2. Choose builder** | Explicit configuration first; then a root/path-configured Dockerfile; otherwise language detection/buildpack/Railpack. Railway always chooses a detected Dockerfile over Railpack. [web:378] | Make the choice visible in logs and expose an override; hidden detection without explanation becomes an abstraction failure. |
| **3. Build securely** | Run unprivileged/rootless BuildKit, CNB, Buildah or a controlled remote builder; isolate untrusted PR builds. | Generate SBOM/provenance, scan dependencies, avoid mounting reusable production credentials, and sign the result. |
| **4. Publish** | Push to an external registry or an in-cluster Distribution/Harbor service. | Reference release artifacts by digest. In-cluster registries need HA object/block storage, TLS, authentication, replication, backup and garbage collection; Distribution GC deletes unreferenced blobs, while Harbor adds scheduled GC and scanning/replication services. [web:437][web:438][web:444] |
| **5. Promote desired state** | Push-style controller updates a Deployment, or CI commits a new digest to Git for Argo CD/Flux reconciliation. | Prefer one build promoted across dev/staging/prod, not separate rebuilds that may produce different bits. |
| **6. Roll out** | Deployment/StatefulSet/Knative revision changes only when the pod-template image digest or another pod-template field changes. Merely overwriting `:latest` does not create a rollout. | Use immutable digests or have image automation commit a changed manifest. Railway’s registry-image updater explicitly redeploys mutable tags when the underlying digest changes. [web:385][web:388] |
| **7. Verify and retain** | Readiness/startup probes, migration job, health/SLO gates and progressive rollout determine success. | Retain prior image digest and manifest for rollback; never treat “container started” as successful deployment. |

---

## 5. Hard operational components a new distribution must solve

| Operational area | Main options | Engineering decision and hard parts |
|---|---|---|
| **CNI** | **Cilium**, **Calico**, **Flannel** | Cilium uses eBPF for service load balancing, identity-aware L3–L7 policy and Hubble flow visibility, and can replace kube-proxy; powerful but kernel-sensitive and operationally richer. [web:317][web:330] Calico offers mature NetworkPolicy, BGP/routed networking and iptables or eBPF data planes. Flannel is the simplest VXLAN/host-gw pod network but needs another policy engine. A distro must own kernel compatibility, MTU detection, dual-stack, upgrades and node-IP changes—not merely install a DaemonSet. |
| **North–south routing** | Traefik, supported NGINX products, Gateway API implementations, Envoy Gateway | Traefik is convenient and k3s-compatible. New platforms should expose **Gateway API**, which separates Gateway infrastructure ownership from application Routes and is the successor to Ingress. [web:306] The community `kubernetes/ingress-nginx` controller was retired in March 2026; existing installations continue but receive no new security fixes. [web:301] Envoy Gateway is a strong neutral Gateway API implementation where Envoy’s footprint is acceptable. |
| **Service load balancing** | Cloud LB controller, MetalLB, Cilium LB-IPAM/BGP, k3s ServiceLB | Bare metal needs address allocation and L2/BGP advertisement. k3s ServiceLB consumes host ports and is simple but unsuitable for every topology; a distro must avoid claiming that `Service type=LoadBalancer` works without defining where addresses come from. |
| **CSI / persistent storage** | local-path/hostPath, Longhorn, OpenEBS, Rook/Ceph, cloud CSI | local-path is tiny but ties data to one node and is not HA. Longhorn synchronously replicates per-volume block replicas and controllers across nodes. [web:286] OpenEBS ranges from local PVs to replicated Mayastor block storage over NVMe-oF. [web:291][web:292] Rook automates a full Ceph system offering block, filesystem and object storage through Ceph-CSI, but adds monitors, OSDs, placement and recovery complexity. [web:290] The distro must handle disk discovery, failure domains, expansion, snapshots, backups and storage upgrades. |
| **Certificates** | cert-manager with ACME/Vault/private PKI; internal CA for cluster components | Bootstrap and rotation for Kubernetes component certificates are separate from application TLS. cert-manager’s namespaced `Issuer` and cluster-wide `ClusterIssuer` turn certificate issuance into reconciliation; ACME still requires account keys, DNS/HTTP solvers and rate-limit handling. [web:331][web:339] |
| **Secrets** | Native Secret plus encryption-at-rest/KMS; External Secrets/Secrets Store CSI; Vault/cloud secret managers; Sealed Secrets/SOPS for Git | Base64 is not encryption. The platform must define API-server encryption and key rotation, tenant read permissions, secret injection, external-manager outage behavior, audit logging and safe GitOps handling. |
| **Multi-tenancy** | Namespaces, RBAC, NetworkPolicy, ResourceQuota/LimitRange, Pod Security Admission; virtual clusters or separate physical clusters for stronger isolation | Namespaces scope names and many RBAC resources but are not a hard security boundary by themselves. Kubernetes guidance combines namespace-scoped RBAC and NetworkPolicy; production platforms also need default-deny traffic, quotas, restricted pod security, admission policy and controls on cluster-scoped CRDs/webhooks. [web:335][web:342] Hostile tenants generally justify virtual or separate clusters. |
| **Pod autoscaling** | HPA, VPA, KEDA, Knative autoscaler | HPA scales replicas from resource/custom metrics but depends on valid requests and a metrics pipeline. VPA recommends or changes requests, possibly evicting pods; using VPA and CPU-utilization HPA together can form feedback loops. KEDA turns queue/database/API events into external metrics, directly handles zero-to-one/one-to-zero and delegates one-to-N to HPA. [web:348] |
| **Node autoscaling** | Cluster Autoscaler, **Karpenter**, cloud node-pool controllers | The platform must satisfy pending pods’ zones, architecture, GPU, taints and storage topology, then safely consolidate underutilized nodes around PDBs and daemonsets. Karpenter NodePools express constraints, expiry and consolidation; consolidation removes empty nodes or replaces capacity with cheaper alternatives. [web:346][web:355] |
| **Observability** | Prometheus/Alertmanager/Grafana; OpenTelemetry Collector/Operator; Loki or another log backend; Tempo/Jaeger or commercial backend | Prometheus remains the natural Kubernetes metrics/alerting system, but long retention and high-cardinality labels require capacity planning. OpenTelemetry is the vendor-neutral traces/metrics/logs pipeline; its Collector can receive OTLP, enrich events with Kubernetes metadata and collect kubelet/node/container statistics. [web:273][web:274] A distro must also expose control-plane, etcd/dqlite, CNI, DNS, storage and upgrade health—not only application CPU charts. |
| **Backup and disaster recovery** | etcd/dqlite snapshots, SQL-native backups, Velero, CSI snapshots and application-native database backups | A Kubernetes object backup does not necessarily create a transactionally consistent database backup. Restore testing, encryption keys, off-cluster copies and version compatibility are distribution responsibilities. |
| **Upgrades and compatibility** | Version skew policy, staged channels, canaries, drain/PDB coordination and rollback | Core Kubernetes, CRI, CNI, CSI, ingress/Gateway, CRDs and webhooks all have independent compatibility windows. The distribution needs tested version matrices and health-gated rollouts; “replace one binary” is only the first layer. |

---

## 6. Why developers complain about Kubernetes

| Complaint | Underlying technical cause | Why Railway / Render / Fly feel better |
|---|---|---|
| **Too many concepts before the first deployment** | A simple web process can require image build, registry, Deployment, Service, Ingress/Gateway, DNS, certificate, ConfigMaps, Secrets, probes, requests/limits and often Helm/GitOps. | Railway and Render infer the common case from a repository and expose one service object. Fly uses a small number of VM, process, volume and region concepts rather than the Kubernetes resource graph. |
| **YAML and CRD sprawl** | Every add-on creates another API, version, controller and reconciliation state; Helm/Kustomize frequently generate more YAML rather than eliminating the underlying model. Practitioners specifically report hundreds of lines, nested structures, silently ignored fields and API-version churn. [web:369] | PaaS settings form a product-level schema with a UI, validation and integrated logs rather than raw provider CRDs. |
| **Debugging crosses layers** | A request can traverse DNS, cloud LB, Gateway/Ingress, Service, kube-proxy/eBPF, sidecar, pod and storage. Desired state may be accepted by the API while a controller fails asynchronously. | PaaS narrows supported combinations and correlates build logs, deployment state, domains, metrics and runtime logs around one service. |
| **The platform is assembled, not complete** | Kubernetes intentionally does not select CNI, ingress, storage class, registry, certificate issuer, log backend, secret manager or deployment workflow. | Hosted PaaS owns those decisions and their upgrades. Users buy the integrated operating model, not just container scheduling. |
| **Cost and capacity are indirect** | Requests, limits, bin-packing, replicas, node pools and autoscaler delays mediate cost; idle control-plane and add-on overhead matters to small teams. | Railway meters service CPU/RAM; Render presents service sizes; Fly exposes each billed Machine. The bill can still surprise, but its objects map more directly to the product UI. |
| **Self-service abstractions leak** | When an internal platform hides Kubernetes completely, unusual networking, storage or rollout failures still require knowledge of the hidden layer, but developers may lack both access and observability. Industry platform teams report that developers still need some Kubernetes/cloud knowledge despite a “golden path.” [web:262] | Successful PaaS products provide opinionated defaults, visible build/release logs and an escape hatch—usually Dockerfile, prebuilt image, CLI/API or a lower-level runtime primitive. |

## 7. What Kubernetes-based PaaS attempts commonly got wrong

| Mistake | Consequence | Better design |
|---|---|---|
| **Treating Kubernetes itself as the developer API** | Developers must understand Deployments, Services, Ingress, RBAC, storage classes and controller failure modes just to ship code. | Present a stable application/service API and treat Kubernetes manifests as an implementation detail. |
| **Providing a thin dashboard over YAML** | A nicer form does not own builds, database lifecycle, DNS, TLS, rollback or debugging; cognitive load remains. | Integrate source, build, artifact, release, environment, networking, data and observability into one lifecycle. |
| **Over-generalizing the application model** | OAM/CUE/templating schemas become another language, while every exception requires platform engineers to write a new trait or composition. | Keep the 80% path very small; expose transparent generated state and Docker/image/Kubernetes escape hatches for the remainder. |
| **Running builds in the production cluster without isolation** | Untrusted repository code receives network reachability, credentials and a path to cluster compromise; builds also starve workloads. | Isolate builders in dedicated nodes/clusters/accounts, use short-lived credentials and publish signed immutable artifacts. |
| **Pretending installed databases are managed databases** | Users discover too late that backup, restore, upgrades, failover and corruption recovery are their responsibility. | State the responsibility boundary explicitly; offer tested HA, backup/restore, maintenance and support as a real managed product. Railway’s own documentation now makes this unmanaged-template distinction explicit. [web:95] |
| **No opinionated day-two lifecycle** | The platform demos well but fails on CNI/CSI upgrades, certificate expiration, etcd recovery, node replacement and CVEs. | Version and test the whole bill of materials; provide health-gated upgrades, backups, rollback and deprecation policy. |
| **No escape hatch—or too many knobs** | With no escape hatch, advanced teams leave; with every Kubernetes knob exposed, beginners gain no simplification. | Progressive disclosure: zero-config build and deploy, explicit advanced settings, then Dockerfile/image and finally controlled raw Kubernetes access. |
| **Ignoring economics** | Per-team namespaces and always-on replicas are cheap conceptually but expensive at small scale; idle platform services overwhelm application cost. | Scale-to-zero where latency permits, bin-pack safely, show cost per service/environment and auto-expire PR environments. |

## Bottom line

A compelling new “minimal distro plus PaaS” should not attempt to out-feature Kubernetes. It should:

1. use **k0s- or k3s-like packaging** or **Talos-like immutable nodes**;
2. choose and support one tested default for CNI, Gateway, CSI, certificates and observability;
3. expose a small application API centered on **source/image, processes, environment, routes, volumes and dependencies**;
4. implement Dockerfile plus buildpack/Railpack-style detection, with immutable image promotion;
5. reconcile deployments through GitOps or an equally auditable desired-state store;
6. make backup, upgrade and tenant-isolation responsibilities explicit; and
7. preserve an escape hatch without forcing every developer to understand the underlying Kubernetes graph.

That combination—not the scheduler footprint alone—is what separates a useful minimal Kubernetes distribution from a platform developers actually enjoy.

Citations:
[61] https://docs.k0sproject.io/head/architecture/
[62] https://docs.k0sproject.io/v1.23.1+k0s.0/architecture/
[63] https://docs.k0sproject.io/v1.21.2+k0s.1/architecture/
[64] https://docs.k0sproject.io/v1.29.5+k0s.0/architecture/
[65] https://docs.k0sproject.io/v1.21.3+k0s.0/architecture/
[66] https://vadosware.io/post/upgrading-a-k0s-cluster-from-single-to-ha/
[67] https://github.com/k0sproject/k0s
[68] https://k0s-family.luc.run/k0sctl/upgrade/
[69] https://oneuptime.com/blog/post/2026-02-26-argocd-with-k0s/view
[70] https://deepwiki.com/k0sproject/k0s/2-installation-and-deployment
[71] https://citizix.com/k0s-the-simple-solid-and-certified-kubernetes-distribution/
[72] https://docs.k0sproject.io/v1.33.2+k0s.0/governance/cncf/gtr/
[73] https://docs.k0sproject.io/v1.30.5+k0s.0/configuration/
[74] https://docs.k0sproject.io/v1.23.6+k0s.2/high-availability/
[75] https://github.com/k0sproject/k0s/blob/main/README.md
[1] https://docs.k3s.io/architecture
[2] https://docs.k3s.io/datastore
[3] https://deepwiki.com/k3s-io/docs/3.1-datastore-options-(sqlite-etcd-external-db)
[4] https://docs.k3s.io/datastore/ha-embedded
[5] https://docs.k3s.io/installation/packaged-components
[6] https://iotdigitaltwinplm.com/k3s-edge-kubernetes-production-guide/
[7] https://deepwiki.com/k3s-io/docs
[8] https://rafftechnologies.com/learn/guides/k3s-high-availability-embedded-etcd-vs-external-database
[9] https://www.fierce-network.com/wireless/rancher-labs-slims-down-kubernetes-for-edge-iot-devices
[10] https://blog.8grams.tech/k3s-lightweight-kubernetes-for-single-sever-edge-and-iot-deployments/
[11] https://documentation.suse.com/cloudnative/k3s/latest/zh/architecture.html
[12] https://medium.com/uleap/kubernetes-for-iot-the-magic-behind-k3s-791f0f04477b
[13] https://www.suse.com/c/rancher_blog/why-k3s-is-the-future-of-kubernetes-at-the-edge/?hss_channel=tw-309890367
[14] https://www.suse.com/c/rancher_blog/introducing-k3s-the-lightweight-kubernetes-distribution-built-for-the-edge/
[15] https://www.suse.com/c/zh-hans/rancher_blog/introducing-k3s-the-lightweight-kubernetes-distribution-built-for-the-edge/
[76] https://canonical.com/microk8s/docs/high-availability
[77] https://canonical.com/blog/microk8s-ha-tech-preview-is-now-available
[78] https://www.faun.dev/c/stories/eon01/microk8s-vs-k3s/
[79] https://discuss.kubernetes.io/t/snap-autoupdate-on-hacluster/14430
[80] https://github.com/canonical/microk8s/issues/1127
[81] https://oneuptime.com/blog/post/2026-03-02-how-to-set-up-microk8s-for-edge-deployments-on-ubuntu/view
[82] https://canonical.com/microk8s/docs/upgrading
[83] https://deepwiki.com/canonical/microk8s/5.2-high-availability
[84] https://docs.gazeley.uk/books/microk8s/page/microk8s
[85] https://discuss.kubernetes.io/t/is-microk8s-considered-secure-enough-for-production-use/7974
[86] https://github.com/canonical/microk8s/issues/5016
[87] https://www.cncf.io/wp-content/uploads/2020/11/MicroK8s-High-Availability-Alex-Chalkias.pdf
[88] https://k8s.guru/blog/2019/06/15/lightweight-kubernetes-k3s-k0s-microk8s/
[89] https://programming-group.com/assets/pdf/papers/2023_Lightweight-Kubernetes-Distributions.pdf
[90] https://dev.to/pendelabhargavasai/the-definitive-guide-to-lightweight-kubernetes-kind-minikube-microk8s-k3s-vcluster-k0s-and-3be1
[31] https://oneuptime.com/blog/post/2026-03-03-understand-the-etcd-service-in-talos-linux/view
[32] https://sneekes.app/posts/upgrade-talos-and-kubernetes/
[33] https://www.siderolabs.com/talos-linux
[34] https://wachs.software/reports/2026-07-23-safely-upgrading-talos-nodes/
[35] https://docs.siderolabs.com/talos/v1.13/learn-more/control-plane
[36] https://oneuptime.com/blog/post/2026-03-03-upgrade-talos-linux-with-zero-downtime/view
[37] https://www.talos.dev/v1.10/kubernetes-guides/upgrading-kubernetes/
[38] https://alexandre-vazquez.com/talos-linux-guide/
[39] https://www.talos.dev/v1.8/kubernetes-guides/upgrading-kubernetes/
[40] https://www.alexlebens.dev/blog/talos-upgrade-56a01f1f56b0/
[41] https://oneuptime.com/blog/post/2026-03-03-understand-talos-linux-immutable-security-model/view
[42] https://www.talos.dev/v1.10/introduction/system-requirements/
[43] https://www.talos.dev/v1.8/introduction/system-requirements/
[44] https://albertogalvez.com/posts/kubernetes/k8s-talos/upgrade/
[45] https://oneuptime.com/blog/post/2026-03-03-plan-a-talos-linux-upgrade-strategy/view
[46] https://aws.amazon.com/blogs/containers/benefiting-from-bottlerocket-an-open-source-container-optimized-operating-system/
[47] https://bottlerocket.dev/en/os/1.63.x/concepts/
[48] https://bottlerocket.dev/en/os/1.49.x/concepts/api-driven/
[49] https://github.com/bottlerocket-os/bottlerocket
[50] https://bottlerocket.dev/en/os/1.54.x/concepts/api-driven/
[51] https://bottlerocket.dev/en/os/1.39.x/concepts/api-driven/
[52] https://bottlerocket.dev/en/os/1.21.x/concepts/api-driven/
[53] https://deepwiki.com/bottlerocket-os/bottlerocket/1.1-system-architecture
[54] https://www.kubeblogs.com/bottlerocket-operating-system/
[55] https://github.com/bottlerocket-os/bottlerocket/blob/develop/SECURITY_FEATURES.md
[56] https://aws.amazon.com/blogs/containers/bottlerocket-a-special-purpose-container-operating-system/
[57] https://bottlerocket.dev/en/os/1.37.x/concepts/components/
[58] https://deepwiki.com/bottlerocket-os/bottlerocket/5-api-system
[59] https://bottlerocket.dev/en/
[60] https://bottlerocket.dev/en/os/1.62.x/concepts/components/
[16] https://kubernetes.io/docs/setup/production-environment/tools/kubeadm/high-availability/
[17] https://kubernetes.io/docs/setup/production-environment/tools/kubeadm/setup-ha-etcd-with-kubeadm/
[18] https://kubernetes.io/docs/setup/production-environment/tools/kubeadm/ha-topology/
[19] https://cubepath.com/docs/docker-kubernetes/kubernetes-installation-with-kubeadm
[20] https://blog.codefarm.me/bootstrapping-kubernetes-clusters-with-kubeadm/
[21] https://github.com/kubernetes/kubeadm/issues/1658
[22] https://kubernetes.io/docs/setup/production-environment/tools/kubeadm/create-cluster-kubeadm/
[23] https://unanswered.io/guide/kubernetes-minimum-requirements
[24] https://oneuptime.com/blog/post/2026-02-09-stacked-vs-external-etcd-topology/view
[25] https://kodekloud.com/community/t/installing-kubernetes-1-26-with-kubeadm/321687
[26] https://blog.jklug.work/k8s/kubernetes-kubeadm-ha-etcd/
[27] https://dev.to/admantium/kubernetes-with-kubeadm-3f4k
[28] https://sigridjin.medium.com/building-a-kubernetes-cluster-with-kubeadm-from-theory-to-practice-faebea2eebd5
[29] https://medium.com/@perezmark.tomcat/kubernetes-installation-using-kubeadm-on-centos-7-9e78547b1d8
[30] https://mohammadshaker.com/en/courses/kubernetes/day-27-kubeadm-cluster-setup
[91] https://docs.railway.com/observability/logs
[92] https://railway.com/pricing.md
[93] https://railway.com/pricing
[94] https://www.budgetforge.dev/tools/railway-pricing-2026
[95] https://docs.railway.com/databases
[96] https://docs.railway.com/pricing/plans
[97] https://www.saaspricepulse.com/blog/railway-pricing-history
[98] https://docs.railway.com/
[99] https://docs.railway.com/pricing
[100] https://www.srvrlss.io/provider/railway/
[101] https://docs.railway.com/cli/deploying
[102] https://railway.com/deploy/flexprice-updated-aug-26--flexprice
[103] https://docs.railway.com/overview/advanced-concepts
[104] https://docs.railway.com/databases/postgresql
[105] https://docs.railway.com/config-as-code/reference
[136] https://render.com/changelog/updated-plans-for-render-workspaces
[137] https://render.com/docs/preview-environments
[138] https://preprice.app/ai-costs/render
[139] https://kuberns.com/blogs/render-pricing/
[140] https://render.com/pricing
[141] https://render.com/docs/new-workspace-plans
[142] https://render.com/blog/better-pricing-for-fast-growing-teams
[143] https://github.com/marketplace/render
[144] https://jwatte.com/blog/render-com-platform-review/
[145] https://render.com/docs/native-runtimes
[146] https://render.com/docs/faq
[147] https://render.com/docs/platform-features-by-plan
[148] https://render.com/docs/deploys
[149] https://render.com/docs/workflows-limits
[150] https://render.com/articles/how-much-does-cloud-application-hosting-cost-for-small-businesses
[121] https://fly.io/docs/reference/architecture/
[122] https://www.budgetforge.dev/tools/fly-io-pricing-2026-2
[123] https://theagentexaminer.com/pricing/fly-machines
[124] https://fly.io/docs/reference/
[125] https://comparedge.com/tools/flyio/cost-guide
[126] https://fly.io/docs/machines/
[127] https://fly.io/machines/
[128] https://comparedge.com/tools/flyio/pricing
[129] https://fly.io/learn/firecracker-vm/
[130] https://www.budgetforge.dev/tools/fly-io-pricing-2026
[131] https://www.srvrlss.io/provider/fly/
[132] https://fly.io/docs/litefs/
[133] https://www.buildmvpfast.com/tools/api-pricing-estimator/fly-io
[134] https://fly.io/docs/machines/overview/
[135] https://apis.io/plans/fly-io/fly-io-plans-pricing/
[166] https://devcenter.heroku.com/articles/github-integration-review-apps
[167] https://devcenter.heroku.com/articles/pipelines
[168] https://devcenter.heroku.com/articles/container-registry-and-runtime
[169] https://www.heroku.com/deploy-with-docker/
[170] https://dev.to/nexgendata/heroku-is-getting-expensive-a-2026-cost-calculator-migration-guide-a2a
[171] https://www.heroku.com/blog/container-registry-and-runtime/
[172] https://granite.so/blog/heroku-vs-granite-postgresql-pricing-and-limits-in-2026
[173] https://www.heroku.com/blog/heroku-review-apps-ga/
[174] https://devcenter.heroku.com/changelog-items/1255
[175] https://hostingpicker.dev/providers/heroku/
[176] https://www.getpricepulse.com/companies/heroku-pricing.html
[177] https://officebook.dev/tools/heroku/
[178] https://servercompass.app/blog/true-cost-of-heroku-2026
[179] https://apis.io/plans/heroku/heroku-plans-pricing/
[180] https://expresstech.io/heroku-vs-render-an-honest-comparison-for-2026/
[106] https://www.usagepricing.com/blueprint/activity/vercel-2026-06-30-price-change
[107] https://livemy.app/blog/vercel-pricing
[108] https://vercel.com/docs/pricing
[109] https://www.usagepricing.com/blueprint/vercel
[110] https://projectcostestimator.com/blog/vercel-cost-2026
[111] https://vercel.com/pricing
[112] https://vercel.com/docs/services/pricing
[113] https://vercel.com/docs/functions/usage-and-pricing
[114] https://flexprice.io/blog/vercel-pricing-breakdown
[115] https://vercel.com/docs/fluid-compute
[116] https://kuberns.com/blogs/vercel-pricing/
[117] https://makerkit.dev/blog/saas/vercel-cost
[118] https://vercel.com/kb/guide/vercel-kubernetes-support
[119] https://vercel.com/docs/pricing/manage-and-optimize-usage
[120] https://vercel.com/docs/functions
[151] https://coolify.io/docs
[152] https://dokku.com/docs~v0.3.26/application-deployment/
[153] https://dokku.com/docs/development/architecture/
[154] https://dokku.com/docs~v0.13.4/deployment/methods/git/
[155] https://dokku-docs.readthedocs.io/_/downloads/en/latest/pdf/
[156] https://caprover.com/docs/one-click-apps.html
[157] https://dokku.com/docs~v0.5.8/deployment/buildpacks/
[158] https://tech-insider.org/fr/coolify-tutoriel-paas-auto-heberge-13-etapes-2026/
[159] https://github.com/caprover/one-click-apps
[160] https://coolify.io/docs/applications
[161] https://dokku.com/docs~v0.8.2/deployment/methods/dockerfiles/
[162] https://repocloud.io/details/CapRover/
[163] https://caprover.com/
[164] https://dokku.com/docs~v0.14.6/deployment/methods/buildpacks/
[165] https://ericmjl.github.io/essays-on-data-science/miscellaneous/static-sites-on-dokku/
[211] https://github.com/jnfrati/porter
[212] https://github.com/porter-dev/porter-archive
[213] https://makerstack.co/reviews/porter-review/
[214] https://northflank.com/docs/v1/application/getting-started/set-up-environments
[215] https://northflank.com/docs/v1/application/release/set-up-preview-blueprints
[216] https://latest.sh/tool/porter-dev
[217] https://github.com/porter-dev/porter/blob/master/README.md
[218] https://github.com/porter-dev/porter-archive/pkgs/container/porter%2Fporter-cli
[219] https://github.com/porter-dev/porter-archive/blob/master/README.md
[220] https://docs.porter.run/applications/deploy/builds
[221] https://northflank.com/docs/v1/application/release/manage-preview-blueprints
[222] https://northflank.com/docs
[223] https://northflank.com/
[224] https://docs.porter.run/applications/deploy/overview
[225] https://www.gappsy.com/tools/porter/
[196] https://knative.dev/docs/serving/autoscaling/scale-to-zero/
[197] https://knative.dev/docs/serving/autoscaling/
[198] https://knative.dev/docs/
[199] https://docs.crossplane.io/latest/
[200] https://knative.dev/docs/serving/autoscaling/scale-bounds/
[201] https://knative.dev/docs/getting-started/first-autoscale/
[202] https://kubevela.net/docs/v1.1/core-concepts/architecture
[203] http://kubevela.net/docs/v1.2/platform-engineers/oam/oam-model
[204] http://kubevela.net/docs/v1.0/concepts
[205] https://github.com/knative/serving/blob/master/docs/scaling/SYSTEM.md
[206] https://docs.redhat.com/en/documentation/red_hat_openshift_serverless/1.35/html/serving/autoscaling
[207] https://knative.dev/docs/serving/autoscaling/autoscaler-types/
[208] https://docs.crossplane.io/v1.20/concepts/compositions/
[209] https://docs.crossplane.io/latest/get-started/get-started-with-managed-resources/
[210] http://kubevela.net/zh/docs/v1.4/getting-started/architecture
[181] https://shipwright.io/docs/build/buildstrategies/
[182] https://shipwright.io/docs/build/build/
[183] https://shipwright.io/docs/build/buildrun/
[184] https://shipwright.io/docs/introduction/
[185] https://shipwright.io/docs/ref/api/build/
[186] https://shipwright.io/docs/next/category/builds
[187] https://github.com/buildpacks/lifecycle
[188] https://buildpacks.io/docs/for-platform-operators/concepts/lifecycle/
[189] https://buildpacks.io/docs/for-app-developers/
[190] https://docs.redhat.com/en/documentation/builds_for_red_hat_openshift/1.3/html/configure/configuring-build-strategies
[191] https://github.com/buildpacks-community/kpack/blob/main/docs/tutorial_kp.md
[192] https://github.com/shipwright-io/build/blob/main/docs/README.md
[193] https://buildpacks.io/docs/for-platform-operators/concepts/lifecycle/detect/
[194] https://buildpacks.io/docs/for-platform-operators/concepts/lifecycle/create/
[195] https://buildpacks.io/docs/
[226] https://devopsaitoolkit.com/blog/argocd-vs-flux-gitops/
[227] https://argo-cd.readthedocs.io/
[228] https://computecentral.in/kubernetes/cicd-and-gitops/04-gitops-with-argocd-and-flux/
[229] https://tech-insider.org/argocd-vs-flux-2026/
[230] https://argo-cd.readthedocs.io/en/stable/
[231] https://www.doit.com/blog/avoid-fluxcd-for-gitops
[232] https://github.com/argoproj/argo-cd
[233] https://www.matthewswong.com/en/blog/gitops-argocd-flux/
[234] https://www.youngju.dev/transcribe/culture/2026-04-14-gitops-argocd-flux-continuous-delivery-guide-2025.en
[235] https://cyberpanel.net/blog/argo-cd-vs-flux
[236] https://argoproj.github.io/cd/
[237] https://argo-cd.readthedocs.io/en/release-2.13/
[238] https://www.cosmiclearn.com/microservices/gitops-argo-flux.php
[239] https://docs.aws.amazon.com/eks/latest/userguide/argocd.html
[240] https://www.youngju.dev/blog/culture/2026-04-14-gitops-argocd-flux-continuous-delivery-guide-2025.en
[241] https://www.cncf.io/wp-content/uploads/2020/08/OAM-Webinar-V2.pdf
[242] https://dicom.nema.org/Dicom/News/June2021/docs/sups/sup224-slides.pdf
[243] https://github.com/oam-dev/spec/blob/master/SPEC.md
[244] https://docs.oracle.com/en/cloud/iaas/verrazzano/1.1/vzdoc/docs/concepts/verrazzanooam/
[245] https://github.com/oam-dev/spec/tree/v0.2.1
[246] https://opensource.microsoft.com/blog/2020/05/27/open-application-model-oam-v1alpha2-crossplane/
[247] https://github.com/oam-dev/spec
[248] https://dev.to/itnext/building-cloud-native-apps-intro-to-open-application-model-and-rudr-bba
[249] https://dev.to/rajibdk/open-application-model-oam-vs-devops-why-oam-is-a-step-backward-5790
[250] https://github.com/kubevela/kubevela
[251] https://stackharbor.com/en/knowledge-base/oam-open-application-model-overview/
[252] https://github.com/crossplane/oam-kubernetes-runtime/blob/master/design/one-pager-trait-scope-workload-interaction-mechanism.md
[253] https://jimmysong.io/zh/book/kubernetes-handbook/cloud-native/spec/
[254] https://docs.oracle.com/cd/F51697_01/1.2/vzdoc/docs/concepts/verrazzanooam/
[255] https://cloud.tencent.com/developer/article/2326828
[256] https://tianpan.co/forum/t/over-abstraction-in-platform-engineering-when-golden-paths-become-golden-cages/3373
[257] https://livebook.manning.com/book/platform-engineering-on-kubernetes/chapter-6
[258] https://dokumen.pub/platform-engineering-on-kubernetes-1nbsped-1617299324-9781617299322.html
[259] https://handsonk8s.substack.com/p/lesson-55-platform-engineering-building
[260] https://www.linkedin.com/pulse/abstractions-kubernetes-its-turtles-all-way-down-steven-peh-i6y3c
[261] https://www.reddit.com/r/kubernetes/comments/1klykub/stop_building_platforms_nobody_uses_pick_the/
[262] https://www.techtarget.com/it-infrastructure/news/252526594/Platform-engineers-plug-abstraction-leaks
[263] https://www.cloudfoundry.org/event/kubernetes-ci-cd-made-easy-with-cloud-foundry/
[264] https://outplane.com/blog/cloud-native-architecture-small-teams
[265] https://octopus.com/devops/platform-engineering/platform-engineering-on-kubernetes/
[266] https://www.techtimes.com/articles/315763/20260409/kubernetes-fatigue-driving-devops-teams-toward-smarter-abstraction-platform-engineering.htm
[267] https://www.comparethecloud.net/opinions/oss-paas-rundown-comparing-dokku-flynn-and-deis-workflow
[268] https://www.oreilly.com/library/view/platform-engineering-on/9781617299322/
[269] https://northflank.com/blog/kubernetes-paas
[270] https://www.linkedin.com/pulse/engineers-compass-navigating-chasm-between-platform-product-gary-yang-j9jhe
[316] https://docs.cilium.io/en/stable/network/kubernetes/kubeproxy-free/
[317] https://cilium.io/
[318] https://cilium.io/blog/2020/02/18/cilium-17/
[319] https://docs.cilium.io/en/stable/observability/visibility/
[320] https://dev.to/le_beltagy/i-replaced-kube-proxy-with-ebpf-in-production-and-why-my-monitoring-went-blind-for-6-hours-1n18
[321] https://cilium.io/blog/2019/08/20/cilium-16/
[322] https://notes.kodekloud.com/docs/Prep-Course-Cilium-Certified-Associate-CCA-Certification/Cilium-Overview/Architecture/page
[323] https://kloudvin.com/article/cilium-ebpf-kubernetes-network-policy-hubble-observability/
[324] https://docs.cilium.io/en/stable/network/kubernetes/local-redirect-policy/
[325] https://michealgarner.co.uk/blog/cilium-kubernetes-networking-and-security-built-on-ebpf/
[326] https://docs.tigera.io/calico/latest/reference/architecture/data-path
[327] https://yindongliang.com/docs/Kubernetes/calico-networking-bgp-ipip-vxlan-ebpf/
[328] https://docs.tigera.io/calico/latest/about/kubernetes-training/about-kubernetes-services
[329] https://www.ibm.com/docs/tr/SSBS6K_3.1.2/manage_network/calico.html
[330] https://github.com/cilium/cilium
[301] https://www.kubernetes.dev/blog/2025/11/12/ingress-nginx-retirement/
[302] https://blog.aks.azure.com/2025/11/13/ingress-nginx-update
[303] https://aws.amazon.com/blogs/networking-and-content-delivery/navigating-the-nginx-ingress-retirement-a-practical-guide-to-migration-on-aws/
[304] https://kubernetes.recipes/recipes/networking/kubernetes-gateway-api-guide/
[305] https://www.umb.ch/en/blog/news/detail/ingress-nginx-will-be-retired-in-march-2026-what-you-need-to-know-now
[306] https://kubernetes.io/docs/concepts/services-networking/gateway/
[307] https://www.herodevs.com/blog-posts/ingress-nginx-end-of-life-2026-migration-and-support
[308] https://blog.nginx.org/blog/the-ingress-nginx-alternative-open-source-nginx-ingress-controller-for-the-long-term
[309] https://x.com/kubernetesio/status/2013613003392844285
[310] https://www.cloudmagazin.com/en/2026/06/24/ingress-nginx-is-discontinued-the-path-to-gateway-api/
[311] https://wz-it.com/en/knowledge/kubernetes/gateway-api-vs-ingress/
[312] https://www.fairwinds.com/blog/kubernetes-ingress-nginx-2026-migration
[313] https://www.cncf.io/blog/2026/07/09/navigating-the-ingress-nginx-retirement/
[314] https://techcommunity.microsoft.com/blog/azurearchitectureblog/from-ingress-to-gateway-api-a-pragmatic-path-forward-and-why-it-matters-now/4489779
[315] https://minhvo.is-a.dev/blogs/kubernetes-gateway-api-the-future-of-ingress
[286] https://github.com/longhorn/longhorn
[287] https://openebs.io/docs/3.10.x/user-guides/mayastor
[288] https://github.com/longhorn/longhorn/blob/master/README.md
[289] https://openebs.io/docs/3.4.x/
[290] https://rook.io/docs/rook/latest-release/Getting-Started/storage-architecture/
[291] https://openebs.io/docs/4.0.x/user-guides/replicated-storage-user-guide/replicated-pv-mayastor/rs-installation
[292] https://openebs.io/docs/user-guides/replicated-storage-user-guide/replicated-pv-mayastor/rs-overview
[293] https://openebs.io/docs/3.10.x/concepts/architecture
[294] https://openebs.io/docs/concepts/data-engines/replicated-storage
[295] https://github.com/derekbit/longhorn
[296] https://openebs.io/docs/3.2.x/concepts/mayastor
[297] https://openebs.io/docs/main/Solutioning/openebs-on-kubernetes-platforms/gke
[298] https://openebs.io/docs/Solutioning/openebs-on-kubernetes-platforms/gke
[299] https://github.com/openebs/mayastor-docs/blob/develop/quickstart/deploy-mayastor.md
[300] https://oneuptime.com/blog/post/2026-02-20-longhorn-storage-kubernetes/view
[331] https://cert-manager.io/docs/configuration/acme/
[332] https://oneuptime.com/blog/post/2026-02-20-kubernetes-multi-tenancy-guide/view
[333] https://cert-manager.io/docs/configuration/issuers/
[334] https://www.systemshardening.com/articles/kubernetes/multi-tenancy-hardening/
[335] https://kubernetes.io/docs/concepts/security/multi-tenancy/
[336] https://www.mirantis.com/blog/kubernetes-multi-tenancy-best-practices/
[337] https://dev.to/zop_8abedcc7e12/kubernetes-multi-tenancy-namespace-isolation-rbac-and-network-policies-explained-3jjm
[338] https://dev.to/cloud-sky-ops/post-110-multi-tenancy-security-baseline-with-namespaces-quotas-networkpolicies-and-pod-2mfj
[339] https://cert-manager.io/docs/concepts/issuer/
[340] https://dev.to/muskan_8abedcc7e12/kubernetes-multi-tenancy-namespace-isolation-rbac-and-network-policies-explained-3jjm
[341] https://cert-manager.io/docs/
[342] https://www.stakater.com/post/best-practices-for-resource-quotas-and-isolation-in-shared-clusters
[343] https://oneuptime.com/blog/post/2026-02-09-multi-tenancy-namespace-isolation/view
[344] https://northflank.com/blog/kubernetes-multi-tenancy
[345] https://github.com/cert-manager/cert-manager
[346] https://karpenter.sh/docs/
[347] https://keda.sh/docs/2.20/
[348] https://keda.sh/docs/2.20/concepts/
[349] https://keda.sh/docs/2.20/concepts/scaling-deployments/
[350] https://notes.kodekloud.com/docs/Kubernetes-Autoscaling/Vertical-Pod-Autoscaler-VPA/Summary-Closure-and-Limitation-of-HPA-VPA/page
[351] https://karpenter.sh/docs/faq/
[352] https://opensource.microsoft.com/blog/2019/05/06/announcing-keda-kubernetes-event-driven-autoscaling-containers/
[353] https://github.com/kedacore/keda
[354] https://codingprotocols.com/blog/karpenter-v1-deep-dive
[355] https://karpenter.sh/docs/concepts/
[356] https://docs.aws.amazon.com/solutions/event-driven-application-autoscaling-with-keda-on-amazon-eks/
[357] https://notes.kodekloud.com/docs/Kubernetes-Autoscaling/Kubernetes-Event-Driven-Autoscaling-KEDA/KEDA-Introduction/page
[358] https://aws.amazon.com/blogs/containers/using-amazon-ec2-spot-instances-with-karpenter/
[359] https://docs.cloud.google.com/kubernetes-engine/docs/tutorials/scale-to-zero-using-keda
[360] https://learn.microsoft.com/en-us/training/modules/aks-app-scale-keda/2-concept-keda-scaling
[271] https://opentelemetry.opendocs.io/docs/kubernetes/getting-started/
[272] https://opentelemetry.io/docs/specs/otel/logs/
[273] https://opentelemetry.io/docs/platforms/kubernetes/collector/
[274] https://opentelemetry.io/docs/platforms/kubernetes/getting-started/
[275] https://opentelemetry.io/docs/
[276] https://docs.lightstep.com/docs/quick-start-operator
[277] https://blog.nashtechglobal.com/setup-observability-with-open-telemetry-prometheus-loki-tempo-grafana-on-kubernetes/
[278] https://docs.lightstep.com/docs/replace-prometheus-with-an-otel-collector-on-kubernetes
[279] https://jorijn.com/en/knowledge-base/kubernetes/monitoring/kubernetes-opentelemetry-observability/
[280] https://opentelemetry.io/docs/collector/internal-telemetry/
[281] https://imti.co/kubernetes-observability-stack/
[282] https://opentelemetry.io/docs/platforms/kubernetes/collector/components/
[283] https://oneuptime.com/blog/post/2026-02-06-opentelemetry-operator-prometheus-alerts-runbooks/view
[284] https://openobserve.ai/blog/monitoring-kubernetes-with-opentelemetry/
[285] https://opentelemetry.io/blog/2024/prom-and-otel/
[421] https://github.com/k3s-io/k3s/issues/12509
[422] https://github.com/k3s-io/k3s/issues
[423] https://github.com/k0sproject/k0sctl/issues
[424] https://lunar.computer/k3s-updates-to-kubernetes-v1-31-14-with-extensive-20251124
[425] https://github.com/k3s-io/k3s/issues/7203
[426] https://github.com/k3s-io/k3s/issues/11347
[427] https://github.com/k3s-io/k3s/labels
[428] https://github.com/k3s-io/k3s/issues/7096
[429] https://github.com/k3s-io/k3s/issues/1439
[430] https://github.com/k3s-io/k3s/issues/7537
[431] https://github.com/k3s-io/k3s/issues/6442
[432] https://github.com/k0sproject/k0sctl
[433] https://github.com/k0sproject/k0s/issues/3282
[434] https://github.com/k3s-io/k3s/issues/6520
[435] https://github.com/k3s-io/k3s/issues/6314
[391] https://github.com/canonical/microk8s/issues/4374
[392] https://www.reddit.com/r/kubernetes/comments/1i9nu3o/microk8s_is_it_good_option/
[393] https://github.com/canonical/microk8s/issues/3064
[394] https://github.com/canonical/microk8s/issues/3706
[395] https://github.com/canonical/microk8s/issues/4744
[396] https://github.com/canonical/microk8s/issues/3109
[397] https://github.com/canonical/microk8s/issues/2898
[398] https://github.com/siderolabs/talos/discussions/9046
[399] https://github.com/canonical/microk8s/issues/5000
[400] https://oneuptime.com/blog/post/2026-03-03-understand-why-talos-has-no-shell-access/view
[401] https://www.reddit.com/r/homelab/comments/1kks9ng/migration_from_microk8s_dqlite_to_etcd/
[402] https://jonathangazeley.com/2023/03/14/kubernetes-homelab-part-4-hyperconverged-storage/
[403] https://medium.com/@mbaena_43991/how-i-recovered-a-corrupted-microk8s-cluster-by-rebuilding-dqlite-snapshots-941ea11b7925
[404] https://emrul.substack.com/p/microk8s-dqlite-problem
[405] https://news.ycombinator.com/item?id=43593269
[376] https://docs.railway.com/builds/dockerfiles
[377] https://docs.railway.com/deployments/github-autodeploys
[378] https://docs.railway.com/config-as-code/reference
[379] https://docs.railway.com/deployments/github-autodeploys.md
[380] https://docs.railway.com/deployments/reference
[381] https://docs.railway.com/languages-frameworks
[382] https://docs.railway.com/builds
[383] https://docs.railway.com/builds/railpack
[384] https://docs.railway.com/deployments
[385] https://docs.railway.com/services
[386] https://docs.railway.com/builds/build-configuration
[387] https://docs.railway.com/guides/vibe-coding-deploy
[388] https://docs.railway.com/guides/private-container-registry
[389] https://docs.railway.com/volumes/reference
[390] https://docs.railway.com/dynamic/reference/errors/nixpacks-was-unable-to-generate-a-build-plan
[406] https://nixpacks.com/docs/how-it-works
[407] https://deepwiki.com/railwayapp/nixpacks/1-overview
[408] https://deepwiki.com/railwayapp/nixpacks/4.2-dockerfile-generation
[409] https://deepwiki.com/railwayapp/nixpacks/1.3-how-it-works
[410] https://deepwiki.com/railwayapp/nixpacks/1.1-architecture
[411] https://blog.railway.com/p/introducing-railpack
[412] https://tokrepo.com/en/workflows/asset-144dfd67
[413] https://nixpacks.com/docs/configuration/file
[414] https://www.infoq.com/news/2026/08/buildpacks-dockerfile-patching/
[415] https://apis.io/apis/nixpacks/cli/
[416] https://medium.com/@michael.vittrup.larsen/dockerfiles-vs-cloud-native-buildpacks-8acf8149dea1
[417] https://github.com/railwayapp/railpack/pkgs/container/railpack-builder
[418] https://github.com/railwayapp/railpack/blob/main/README.md
[419] https://bex.co/blog/2026/06/04/railpack-replaces-nixpacks-buildkit
[420] https://github.com/railwayapp/railpack
[436] https://kubernetes.recipes/recipes/deployments/private-container-registry-kubernetes/
[437] https://distribution.github.io/distribution/about/garbage-collection/
[438] https://www.cncf.io/blog/2026/02/24/making-harbor-production-ready-essential-considerations-for-deployment/
[439] https://appsecsanta.com/harbor
[440] https://forum.gitlab.com/t/trigger-pull-based-re-deployment-when-image-updates-in-registry/75633
[441] https://github.com/etoews/kubernetes-harbor/blob/master/docs/user_guide.md
[442] https://distribution.github.io/distribution/about/deploying/
[443] https://indico.cern.ch/event/1079490/contributions/4949461/attachments/2507114/4308224/CVMFS%20Workshop%20-%20Harbor%20Registry.pdf
[444] https://www.cncf.io/blog/2026/01/05/deploying-harbor-on-kubernetes-using-helm/
[445] https://lucaberton.com/blog/harbor-container-registry-kubernetes-setup-2026/
[446] https://kubernetes.recipes/recipes/security/kubernetes-harbor-registry-guide/
[447] https://zairalabs.ai/guide/tools/harbor/
[448] https://piotrminkowski.com/2022/06/06/continuous-development-on-kubernetes-with-gitops-approach/
[449] https://novaaiops.com/blog/kubernetes-image-promotion-pipelines
[450] https://fluxcd.io/flux/guides/image-update/
[361] https://stackoverflow.co/company/press/archive/stack-overflow-2025-developer-survey/
[362] https://survey.stackoverflow.co/
[363] https://survey.stackoverflow.co/2025/
[364] https://medium.com/@kanishksinghpujari/kubernetes-is-killing-yaml-inside-the-private-beta-of-k8s-2-0-thats-rewriting-everything-bf1caee847a8
[365] https://stackoverflow.blog/2025/01/01/developers-want-more-more-more-the-2024-results-from-stack-overflow-s-annual-developer-survey/
[366] https://byteiota.com/kubernetes-overkill-90-of-teams-should-skip-it/
[367] https://stackoverflow.blog/2025/10/23/what-leaders-need-to-know-from-the-2025-stack-overflow-developer-survey/
[368] https://dev.to/arbythecoder/why-i-avoided-kubernetes-for-2-years-spoiler-bad-idea-179k
[369] https://kubesphere.medium.com/why-managing-kubernetes-still-feels-broken-in-2026-9aff5aa8a5c2
[370] https://stackoverflow.blog/developer-survey/
[371] https://survey.stackoverflow.co/2025/technology
[372] https://www.clientcues.com/competitors/compare/railway-vs-render-vs-fly-2026-07/
[373] https://survey.stackoverflow.co/2024/
[374] https://devtoolpicks.com/blog/railway-vs-render-vs-fly-io-solo-developers-2026
[375] https://stackoverflow.co/internal/resources/2025-stack-overflow-developer-survey-for-leaders/
[481] https://docs.k0sproject.io/v1.35.2+k0s.0/system-requirements/
[482] https://docs.k0sproject.io/v0.11.0/system-requirements/
[483] https://docs.k0sproject.io/v1.35.4+k0s.0/system-requirements/
[484] https://docs.k0sproject.io/v1.33.4+k0s.0/system-requirements/
[485] https://docs.k0sproject.io/v1.35.1+k0s.1/system-requirements/
[486] https://docs.k0sproject.io/v1.23.1+k0s.1/system-requirements/
[487] https://docs.k0sproject.io/v1.32.13+k0s.0/system-requirements/
[488] https://docs.k0sproject.io/v1.21.0+k0s.0/system-requirements/
[489] https://docs.k0sproject.io/v1.22.1+k0s.0/system-requirements/
[490] https://docs.k0sproject.io/v1.34.1+k0s.1/system-requirements/
[491] https://docs.k0sproject.io/v1.27.9+k0s.0/system-requirements/
[492] https://docs.k0sproject.io/v1.22.4+k0s.1/system-requirements/
[493] https://docs.k3s.io/reference/resource-profiling
[494] https://docs.k3s.io/installation/requirements
[495] https://docs.k0sproject.io/v1.23.1+k0s.0/
[451] https://canonical.com/microk8s/docs/configuring-services
[452] https://canonical.com/microk8s/docs/services-and-ports
[453] https://canonical.com/microk8s/docs/reference
[454] https://canonical.com/microk8s/docs
[455] https://canonical.com/microk8s/compare
[456] https://canonical.com/microk8s/docs/getting-started
[457] https://canonical.com/microk8s/docs/release-notes
[458] https://canonical.com/microk8s/docs/explain-launch-config
[459] https://canonical.com/microk8s
[460] https://canonical.com/microk8s/docs/strict-confinement
[461] https://canonical.com/microk8s/docs/install-windows
[462] https://opensource.siemens.com/events/2023/slides/Konstantinos_Tsakalozos_and_Yalton_Ruiz_MicroK8s_Production-grade_low-footprint_K8s_for_the_Edge.pdf
[463] https://ubuntu.com/blog/canonical-kubernetes-1-25-is-now-generally-available
[464] https://canonical.com/microk8s/docs/install-strict
[465] https://forum.snapcraft.io/t/classic-confinement-for-microk8s-snap/5242
[466] https://docs.k0sproject.io/v1.26.3+k0s.0/configuration/
[467] https://docs.k0sproject.io/v1.35.4+k0s.0/system-requirements/
[468] https://docs.k0sproject.io/v1.35.2+k0s.0/system-requirements/
[469] https://docs.k0sproject.io/stable/system-requirements/
[470] https://docs.k0sproject.io/v1.33.4+k0s.0/system-requirements/
[471] https://docs.k0sproject.io/v1.32.13+k0s.0/system-requirements/
[472] https://docs.k0sproject.io/v0.11.0/system-requirements/
[473] https://docs.k0sproject.io/v1.22.1+k0s.0/system-requirements/
[474] https://docs.k0sproject.io/v1.29.1+k0s.0/system-requirements/
[475] https://docs.k0sproject.io/v1.29.1+k0s.0/
[476] https://docs.k0sproject.io/v1.26.0+k0s.0/configuration/
[477] https://docs.k0sproject.io/v1.22.4+k0s.1/system-requirements/
[478] https://docs.k0sproject.io/v1.26.9+k0s.0/
[479] https://docs.k0sproject.io/v1.23.1+k0s.1/system-requirements/
[480] https://docs.k0sproject.io/v1.27.2+k0s.0/
