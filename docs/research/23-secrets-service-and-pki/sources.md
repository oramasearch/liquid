<!-- Raw research source. Generated via Perplexity `perplexity_research` (web-grounded,
     multi-source) on 2026-09-01 for the liquid project. Lightly reformatted only.
     Treat as evidence, not as liquid design decisions - see README.md in this
     directory for the curated analysis and the implications drawn for liquid. -->

# Technical research report: secrets management, workload identity, and PKI, 2024–2026

**Bottom line:** HashiCorp Vault remains the broadest general-purpose platform when an organization needs dynamic credentials, PKI, encryption-as-a-service, multi-cloud authentication, leasing, and centralized policy. Its main cost is operational complexity. Cloud secret managers and developer-focused products are easier but usually emphasize storage and rotation rather than universal just-in-time credential brokering. OpenBao is now a credible, independently governed Vault fork rather than a frozen compatibility project. For service-to-service identity, SPIFFE/SPIRE is usually a better identity plane than treating Vault authentication itself as workload identity; Vault can then consume that identity and supply secrets or perform cryptographic operations.

## 1. HashiCorp Vault architecture

### Core architecture and storage

A Vault cluster consists of API listeners, the authentication and policy core, mounted auth methods and secrets engines, an expiration/lease manager, identity storage, audit devices, and a durable storage backend.

The storage backend is deliberately **outside the cryptographic barrier and treated as untrusted**. Vault encrypts sensitive persisted objects before writing them. Obtaining a Raft snapshot, Consul database, or raw storage files should therefore not reveal plaintext secrets without the barrier keys. At startup, Vault can locate storage but begins sealed and cannot decrypt it. [web:47]

For current deployments:

| Component | Technical behavior |
|---|---|
| Integrated Storage | Embedded Raft consensus. One leader accepts storage writes and replicates the log to followers; disk latency and inter-node network latency bound write performance. A five-node cluster across three availability zones can tolerate two node failures. [web:291][web:296][web:298] |
| Consul storage | Vault persists encrypted objects in a dedicated Consul KV cluster; Consul supplies HA coordination and service registration. HashiCorp recommends a dedicated Consul cluster rather than sharing one with service discovery or a mesh. [web:293][web:295] |
| Other historical backends | Numerous backends existed, but supported Vault Enterprise deployments are expected to use Integrated Storage or Consul. [web:299] |
| Barrier | The encryption layer between Vault core and physical storage. It maintains the encryption key/keyring used to encrypt values before persistence. |
| HA state | Normally one Vault node is active and the others forward requests. Losing the active node triggers leader election; it does not require reinitialization. |

**Backups remain essential.** Barrier encryption protects confidentiality, not availability. Losing the Raft quorum and snapshots, the KMS/HSM seal key, or the required Shamir shares can still produce permanent data loss.

### Seal, unseal, Shamir, and auto-unseal

The simplified key hierarchy is:

1. Data in the storage backend is encrypted by the barrier keyring.
2. The keyring is protected by Vault’s root key.
3. The root key is protected by the configured seal.
4. With the default Shamir seal, the unseal secret is divided into configurable **N shares**, of which a threshold **T** must be presented.
5. With auto-unseal, an HSM, cloud KMS, or another Vault transit service decrypts the stored, wrapped root key during startup.

This is envelope encryption: the external KMS does not decrypt every Vault secret; it unwraps the key that lets Vault open its barrier. [web:46][web:51]

| Seal mode | Operation | Principal risks |
|---|---|---|
| Shamir | `vault operator init` produces N shares; any T reconstruct the unseal secret. Every restarted node needs threshold participation. Shares should be assigned to separate custodians and systems. | Lost shares can make recovery impossible; copied shares defeat quorum; manual unseal slows unattended recovery. |
| Auto-unseal | At startup Vault authenticates to AWS KMS, Azure Key Vault, GCP Cloud KMS, PKCS#11 HSM, or transit seal and asks it to unwrap the root key. | Vault availability now depends on KMS availability, IAM policy, network reachability, and protection against KMS-key deletion. |
| Recovery shares | In auto-unseal configurations, Shamir-style recovery shares authorize exceptional operations such as root-token generation; they generally do not replace the external seal for routine unsealing. | Operators sometimes discover too late that recovery shares cannot compensate for a destroyed auto-unseal key. |
| Explicit seal | Sealing discards the usable barrier material from memory. Subsequent data operations fail until unsealed. | A powerful incident-response control, but sealing a production cluster is also an immediate outage. |

HSM integration can provide automatic unsealing and, with Enterprise seal-wrap features, additional wrapping of designated sensitive values. It does not eliminate HSM administration: Vault still requires credentials or a PIN, HA capacity, backup, firmware planning, and strict access policy for the HSM. [web:52][web:172]

### Secrets engines

Secrets engines are independently mounted at API path prefixes. The router sends a request to the engine mounted at the longest matching prefix, so the same engine can be mounted multiple times with different configurations and policies. [web:245]

| Secrets engine | Technical function |
|---|---|
| KV v1 | Simple static key/value storage. A write replaces the existing value; no native version history or undelete. |
| KV v2 | Versioned static secrets, check-and-set concurrency protection, configurable retained-version count, soft delete/undelete, rollback, and irreversible version destruction. The default retained history is ten versions. Its API has separate `data/` and `metadata/` paths, which commonly surprises policy authors. [web:241][web:242] |
| Database | Holds a privileged or delegated DB connection and maps Vault roles to creation, renewal, and revocation statements. Reading `database/creds/<role>` creates a unique DB login with role-specific grants and a lease. Default documentation values are normally a one-hour TTL and 24-hour maximum, though operators should set workload-specific values. Static roles rotate an existing named account instead. [web:224][web:468][web:473] |
| Transit | Cryptography-as-a-service: encrypt/decrypt, rewrap after key rotation, sign/verify, HMAC, hashing, random generation, and data-key generation. Vault stores keys but not the caller’s plaintext or ciphertext; applications retain the ciphertext. Versioned keys permit new encryption with the latest version while old ciphertext remains decryptable. [web:541][web:544] |
| PKI | Creates or imports root/intermediate issuers, signs CSRs or produces key-and-certificate bundles, enforces issuance roles and TTLs, revokes certificates, and publishes CRLs and OCSP responses. |
| SSH | Can act as an SSH CA and sign user or host public keys with principals, critical options, extensions, and TTL limits. This avoids distributing the same authorized key to every server. [web:246][web:213] |
| AWS | Creates IAM users/access keys or returns STS credentials according to Vault roles. IAM users are deleted and credentials revoked at lease expiry; STS credentials expire at AWS. Static roles rotate an existing IAM user’s keys. [web:211][web:474] |
| GCP | Generates short-lived OAuth access tokens or service-account keys from configured roles/rolesets and IAM bindings. It is most useful for centrally governed access across multiple projects. [web:251] |
| Azure | Creates or uses service principals and grants configured Azure roles or group memberships for a specified TTL; revocation removes the generated credential or principal assignments. [web:216] |

Vault itself needs a sufficiently privileged “root” credential for many dynamic engines. That credential is therefore a high-value bootstrap secret. Database and AWS engines can rotate their own configured root credentials, reducing routine human knowledge of them. [web:466][web:472]

### Authentication methods

Authentication converts external proof into a Vault token carrying policies, identity metadata, TTL, and sometimes a limited number of uses.

| Auth method | How trust is established | Best use and important caveat |
|---|---|---|
| AppRole | Client presents a relatively stable RoleID plus a SecretID, normally response-wrapped and delivered by a trusted broker. | Generic machine authentication where no native platform identity exists. The SecretID is still a bootstrap secret; poor delivery merely relocates the static-secret problem. [web:256][web:262] |
| Kubernetes | Vault validates a pod’s service-account JWT and normally checks it through the Kubernetes TokenReview API, then binds namespace, service account, audience, or claims to a Vault role. [web:205] | Straightforward pod-to-Vault login. It authenticates to Vault but does not by itself provide continuously rotated mTLS identities between workloads. |
| JWT | Vault verifies a bearer JWT using configured keys or issuer discovery and binds claims/audience to a role. | CI/CD workload federation, projected service-account tokens, and other noninteractive systems. |
| OIDC | Browser-based authorization against an OIDC provider; identity/group claims map to Vault entities and policies. | Human UI/CLI login and SSO. Redirect URIs and group-claim mapping require careful configuration. [web:258] |
| AWS IAM | Verifies a signed AWS API request or EC2 identity evidence and binds AWS account, role, instance, or principal attributes. | Avoids introducing a separate bootstrap password for AWS workloads. |
| GCP | Verifies a Google-signed identity JWT and role constraints; newer configurations can use workload identity federation. [web:257] | Native GCP workloads and cross-cloud federation. |
| Azure | Validates a Microsoft Entra-signed JWT for the configured tenant and checks bound Azure attributes. [web:259] | Azure managed identities, VMs, and federated workloads. |
| SPIFFE | Accepts X.509-SVIDs or JWT-SVIDs using a configured SPIFFE trust bundle. [web:266] | Lets SPIRE be the workload-attestation system while Vault remains the secret and cryptographic service. |

### Tokens, leases, revocation, and policies

Dynamic responses normally include a `lease_id`, `lease_duration`, and `renewable` flag. Renewable leases can be extended only within role, mount, token, and system maximum TTL constraints. Expiration invokes the issuing plugin’s revocation logic—for example `DROP USER`, IAM-user deletion, or removal of an Azure assignment. Operators can revoke one lease, revoke by path prefix, or revoke everything descended from a token. [web:91][web:94]

Tokens are also leased objects. Revoking a parent token recursively revokes child tokens and the dynamic credentials created through them, giving Vault its useful “revoke this compromised workload and everything it obtained” property.

Vault ACL policies are **deny by default** and are expressed in HCL-like path stanzas:

```hcl
path "database/creds/orders-readonly" {
  capabilities = ["read"]
}

path "secret/data/orders/*" {
  capabilities = ["create", "read", "update"]
}

path "secret/metadata/orders/*" {
  capabilities = ["list"]
}

path "sys/leases/revoke-prefix/database/creds/orders-readonly" {
  capabilities = ["update", "sudo"]
}
```

Capabilities include `create`, `read`, `update`, `delete`, `list`, `patch`, `sudo`, and `deny`; they map to Vault API paths rather than an abstract resource model. A `deny` takes precedence. [web:261][web:265] Common mistakes are granting broad wildcard access, forgetting KV v2’s `data`/`metadata` split, and allowing role configuration when a workload only needs credential issuance.

### Namespaces, auditing, and scale

| Facility | Technical behavior |
|---|---|
| Namespaces | Vault Enterprise/HCP creates hierarchical “Vaults within a Vault”: separate auth mounts, secrets engines, tokens, policies, entities, and delegated administrators within one cluster. It is administrative and logical tenant isolation, not a separate process or storage cluster. [web:346][web:348] |
| Audit devices | File, socket, syslog, and other devices receive API request and response records. Most JSON strings are HMAC-SHA-256 protected, allowing correlation or comparison through `sys/audit-hash` without placing secret plaintext in logs. [web:349][web:351] |
| Audit availability rule | Vault attempts to write each event to all configured devices and requires at least one successful device. If none can record, the API request is refused; a blocking device such as a stalled socket can also stall processing. Deploy at least two independent destinations and monitor disk/backpressure. [web:349][web:351] |
| Regular standby | Forwards requests to the active node. It supplies failover, not horizontal request processing. |
| Performance standby | Vault Enterprise standbys serve local read-only work; writes and operations that create tokens or leases coordinate with the active node. Replication is asynchronous, so clients needing read-after-write consistency may use `X-Vault-Index` and forwarding headers. [web:287][web:289] |
| Performance replication | Replicates configuration and supporting secrets across regions. Secondaries issue and manage their own local tokens and leases, reducing WAN dependence for clients. [web:48][web:59] |
| DR replication | Maintains a warm copy of configuration, secrets, tokens, and leases for failover; it is not normally an active traffic-scaling target. [web:48] |

## OpenBao in 2026

HashiCorp changed future Vault releases from MPL-2.0 to BUSL 1.1 in August 2023. BUSL permits source access and most use, but restricts competing offerings; each release is scheduled to convert to MPL after four years. IBM completed its HashiCorp acquisition on February 27, 2025. [web:452][web:454][web:457]

OpenBao forked the last MPL lineage and remains **MPL-2.0**. As of September 2, 2026, it is an **OpenSSF Sandbox project under the Linux Foundation**, governed by a public Technical Steering Committee rather than one vendor. TSC representation includes ControlPlane, SAP/Liquid Reply, Adfinis, Wallix, IOTech, and GitLab. [web:488][web:489][web:494]

| 2026 status | Assessment |
|---|---|
| Current release | OpenBao **2.6.2**, released August 18, 2026, including core and PKI security fixes. [web:556] |
| Compatibility | It retains substantial Vault API, policy, plugin, CLI, and operational compatibility, but compatibility should no longer be interpreted as lockstep implementation. Test providers, agents, plugins, migrations, and seal behavior before switching. |
| Governance | Public TSC meetings and working groups; vendor-neutral OpenSSF/LF oversight; MPL-2.0 code. |
| Major divergence | OpenBao has brought namespaces into its open distribution and is developing features independently rather than merely rebasing Vault. |
| OpenBao 2.6 additions | Per-namespace Shamir seals with separate barrier material, external `kms` auto-unseal plugins, cross-plugin workflows, distroless images, and authenticated replacement root-generation endpoints. [web:487][web:561] |
| Namespace sealing | A tenant can discard its namespace keys and make only that namespace inaccessible. This is cryptographic isolation beyond an API-level namespace lock, although namespace auto-unseal was not yet implemented in 2.6. [web:423][web:429] |
| Relative weakness | Younger operational ecosystem, smaller support market, fewer years of large regulated deployments, and incomplete parity with every mature Vault Enterprise replication, HSM, governance, and managed-service facility. |
| Strategic conclusion | Choose OpenBao when OSI licensing and shared governance matter and the team can qualify its exact feature set. Choose Vault when first-party enterprise support, HCP, and mature multi-region/HSM features outweigh BUSL and vendor control. |

## 2. Competitor assessment

“Complaints” below are recurring user/operator friction, not necessarily product defects.

| System | What it does and does well | Recurring complaints or limitations |
|---|---|---|
| **AWS Secrets Manager** | Fully managed, KMS-encrypted, versioned secret store with IAM, CloudTrail, cross-account/region patterns, managed database rotation, and Lambda-based custom rotation. Strongest when all consumers and targets are in AWS. Rotation events and outcomes are recorded in CloudTrail. [web:318][web:322] | Cost per stored secret plus API calls becomes visible at high cardinality; nonstandard rotation requires Lambda code, VPC connectivity, staging-label discipline, and tests. AWS documents race conditions and Lambda-to-database networking as common rotation failures. [web:226][web:234][web:236] It normally rotates shared credentials; it is not a universal Vault-style lease broker. |
| **AWS Systems Manager Parameter Store** | Hierarchical configuration and `SecureString` storage using KMS, convenient for EC2/ECS/Lambda configuration, with a low-cost standard tier. | No native automatic rotation; AWS recommends Secrets Manager for credentials requiring rotation, cross-region replication, and fine-grained secret controls. Throughput tiers and parameter-size limits create surprises. [web:137][web:143] |
| **GCP Secret Manager** | Managed, IAM-controlled versioned secrets, global or regional replication choices, CMEK, disable/destroy lifecycle, and Cloud Audit Logs. [web:136][web:147][web:319] | A “rotation schedule” sends a Pub/Sub notification; it does not itself change the target credential or update consumers. Teams must operate the rotator, retries, and downstream cutover, and overlapping rotations can be skipped. [web:235] Primarily a static/versioned store, not an arbitrary dynamic DB credential engine. |
| **Azure Key Vault** | Managed secrets, encryption keys, and certificate objects; Entra managed identities, Azure RBAC, private endpoints, Event Grid, and optional HSM-backed keys/Managed HSM. Good fit for Azure-native applications and certificate import/renewal workflows. | Permission models, network rules, private endpoints, and access troubleshooting feel complex. [web:231] Generic secret rotation often means Event Grid plus an Azure Function rather than a complete built-in target integration. [web:439] Per-vault transaction limits lead to HTTP 429 responses if applications fetch on every request; caching and exponential backoff are required. [web:436][web:437] |
| **Infisical** | Developer-oriented open-source/self-hosted or cloud platform with dashboard, CLI, SDKs, secret syncs, machine identities, audit, rotation, Kubernetes operator/CSI delivery, dynamic DB/cloud/Kubernetes/TLS/SSH credentials, and a built-in KMS. [web:361][web:367][web:369] | Advanced dynamic-secret and governance functionality is tier-dependent; rapidly evolving CRDs and product surface create upgrade churn—the older `InfisicalSecret` CRD is deprecated. [web:370][web:372] It has less operational history and fewer deep plugins than Vault, and self-hosting transfers database, HA, upgrade, and recovery work to the customer. |
| **Doppler** | Excellent developer UX for environment/project configuration, CLI injection, references, CI/CD and SaaS syncs, audit, scheduled two-phase rotation, and enterprise dynamic-secret leases with configurable TTL. [web:332][web:337] | Dynamic secrets and deeper controls are enterprise features; users frequently cite pricing growth, short activity-log retention on lower plans, and advanced reporting/auditing being paywalled. [web:333][web:345] Its core abstraction is synchronized application configuration, not a general PKI/transit/seal platform. |
| **1Password Secrets Automation** | Extends an established human password-manager workflow to machines through service accounts, CLI/SDKs, Connect Server caching, Kubernetes injection, and vault-scoped permissions. Connect is useful where applications should read the same items humans govern. [web:153][web:535] | Service accounts have strict hourly and account-wide daily rate limits; Connect avoids repeated-read limits but adds infrastructure. [web:526][web:528] Limited Vault-style dynamic credentials, database leasing, fine-grained path policy, and air-gapped operation; Connect token rotation requires creating and distributing a replacement token. [web:531][web:535] |
| **Akeyless** | SaaS secret manager with customer gateways, static and dynamic secrets, rotation, remote access, and distributed-fragment “zero knowledge” encryption in which the service lacks the customer-held key fragment. [web:18][web:19] | Users cite documentation gaps, assumed prerequisite knowledge, beginner UI friction, and the need for better testing/communication around feature releases. [web:17][web:23][web:24] Gateway deployment and a SaaS control plane add architecture beyond the “no infrastructure” marketing impression. |
| **CyberArk Conjur** | Strong machine-identity authorization, declarative MAML policy-as-code, auditing, Kubernetes/JWT authenticators, and tight integration with CyberArk PAM. Read-only Followers scale authentication and secret reads while Leader/Standby nodes handle writes and failover. [web:514][web:517][web:518] | Deployment, policy structure, certificates, Followers, authenticators, and PAM integration are comparatively heavy. Users report weaker developer ergonomics, opaque bundle pricing, and most value when CyberArk is already strategic. [web:30][web:523] |
| **External Secrets Operator (ESO)** | CNCF Sandbox Kubernetes operator that reconciles many external stores into native Kubernetes `Secret` objects. Supports periodic, create-once, and manually forced refresh patterns. [web:63][web:496] | Secret values still exist in Kubernetes etcd and RBAC, updates are polling/reconciliation based, and changed Secrets do not automatically restart pods. [web:301][web:313] It adds another controller, CRDs, provider compatibility, and authentication failure domain rather than replacing the external manager. |
| **Bitnami Sealed Secrets** | Encrypts a Secret client-side with the cluster controller’s public key, producing a Git-safe `SealedSecret` that only the in-cluster controller can decrypt. Uses an AES-256-GCM content key wrapped with RSA-OAEP. [web:66][web:70] | Cluster coupling is the major issue. Losing or failing to back up all controller private keys makes Git ciphertext unrecoverable; migration and restoration require those keys. [web:302][web:308] Values become ordinary Kubernetes Secrets after decryption, and it provides no dynamic issuance, target rotation, or centralized access audit. |
| **SOPS** | Git-friendly structured-file encryption for YAML, JSON, ENV, INI, and binary files using per-document data keys wrapped by AWS/GCP/Azure KMS, Vault/OpenBao, age, or PGP. It encrypts values while leaving structure reviewable and authenticates the document with a MAC. [web:271][web:272][web:275] | It is an encryption/editor tool, not a runtime secrets service: no leases, revocation, per-read audit, dynamic credentials, or target rotation. Decryption keys must be exposed to CI/CD or a GitOps controller, and plaintext still reaches Kubernetes Secrets or deployment processes. [web:285] |

## 3. Dynamic secrets

### How issuance works

| Credential type | On-demand workflow | Expiration/revocation |
|---|---|---|
| Database login | Workload authenticates to Vault and reads a role. Vault uses its management connection and templated SQL to create a unique user, password, grants, and DB-side expiry. | Lease expiration runs revocation statements such as `DROP USER` or `ALTER ROLE`; explicit lease/token revocation can do it immediately. |
| AWS | Vault creates an IAM user/access key or calls STS with the configured role and policy. | Vault deletes IAM users/keys; STS sessions expire at AWS and generally cannot be individually revoked in the same way. |
| GCP | Vault generates a service-account key or short-lived OAuth token based on a roleset and IAM bindings. | Vault deletes generated keys or lets OAuth tokens expire; IAM-side propagation determines final enforcement speed. |
| Azure | Vault creates a service principal/client credential and assigns configured roles/groups, or uses an existing principal according to role settings. | Generated credentials and assignments are removed at expiry or revocation. |
| SSH | Client generates an SSH key pair and sends the public key; Vault signs it with allowed principals, extensions, and a short TTL. | `sshd` rejects it after `valid-before`; no permanent public key needs to remain in every user’s `authorized_keys`. |

### Why it is usually the largest security gain

Dynamic credentials change the risk model from “protect a valuable password forever” to “authorize a narrowly scoped identity for a limited session”:

- A stolen credential has a bounded lifetime and blast radius.
- Each job or workload can receive a different username, making attribution and selective revocation practical.
- Credentials do not need to be pre-created in source code, CI variables, images, Kubernetes manifests, or developer laptops.
- Removing access can revoke a Vault token and all descendant leases rather than finding every copy.
- Regular issuance continuously exercises the creation and expiry path; quarterly static rotation often discovers broken dependencies only during an emergency.

The benefit comes from **dynamic issuance**, not merely changing one shared password every 30 days. Scheduled rotation still leaves many consumers holding the same credential and requires a synchronized distribution cutover.

### Operational failure modes

| Problem | Why it happens | Engineering response |
|---|---|---|
| Lease storm | Clients request a new credential on every request, restart simultaneously, or fail to cache/renew valid leases. Vault’s expiration manager and storage accumulate millions of leases. | Obtain one lease per workload instance or pool, renew with jitter, stagger deployments, set quotas, and rate-limit aberrant clients. Vault explicitly identifies client non-reuse, simultaneous requests, failed external revocations, and low storage IOPS as causes of lease explosion. [web:93][web:95] |
| Expiration storm | Thousands of leases have the same TTL and issuance timestamp, so revocation jobs hit Vault and the target system simultaneously. | Add jitter, vary TTLs, cap deployment concurrency, and size target APIs/DBs for expiry load. |
| Revocation backlog | Target service is unavailable, management credentials are invalid, or delete operations are slow/rate-limited. The Vault lease expires internally while the backend account remains active. | Monitor revoke errors and orphaned backend identities; build reconciliation that compares Vault leases with DB/cloud accounts; preserve root-credential recovery. |
| Connection pools | Revoking a DB login does not necessarily terminate already-authenticated sessions; alternatively, some DBs terminate them and cause a sudden outage. A pool may continue opening connections with an expired password. | Renew before expiry, create the replacement pool with new credentials, drain the old pool, then revoke. Ensure credential TTL exceeds maximum transaction plus drain time. |
| Process reload | Environment variables do not update inside a running process when the secret changes. Kubernetes Secret updates also do not automatically restart most pods. | Prefer files or local agents with atomic replacement and application reload hooks; otherwise use dual credentials and controlled rolling restart. |
| Mass prefix revocation | `revoke-prefix` is valuable in an incident but can simultaneously drop every DB/cloud session for a role. | Estimate blast radius, support progressive disable where possible, and maintain an audited break-glass role. |
| Control-plane outage | A job that insists on obtaining a fresh credential cannot start while Vault or its KMS is unavailable. | Cache only still-valid leases, use renewable agents, deploy local performance replicas where justified, and explicitly choose fail-closed versus constrained grace behavior. |

## 4. PKI and certificate issuance

### Vault PKI, step-ca, cert-manager, ACME, and Let’s Encrypt

| System | Technical role |
|---|---|
| Vault PKI engine | A general private-CA and certificate-leasing engine. An issuer may be a generated/imported root, generated/imported intermediate, or externally signed intermediate. Roles restrict DNS domains, URI/IP SANs, key algorithms, usages, and maximum TTL. `/issue/<role>` can create the private key and certificate; `/sign/<role>` signs a caller-generated CSR. It publishes CA chains, full/delta CRLs, and OCSP and supports multiple issuers for rotation. Vault recommends keeping a high-value root outside Vault while using Vault for online intermediates. [web:123][web:124][web:128] |
| smallstep `step-ca` | Lightweight, automation-first private CA supporting X.509 and SSH, ACME, SCEP, OIDC, cloud instance identities, JWK tokens, and other provisioners. It is especially strong where teams want a focused CA rather than Vault’s complete secrets platform. [web:4][web:9][web:15] |
| cert-manager | Kubernetes controller implementing `Certificate`, `Issuer`, and cluster-scoped `ClusterIssuer` resources. It creates keys/CSRs, asks ACME, Vault, step-ca, cloud private CAs, or other issuers to sign, stores the result in Kubernetes Secrets, and renews before expiry. A namespaced `Issuer` cannot issue across namespaces. [web:6][web:12] |
| ACME | RFC 8555 machine-to-machine issuance protocol: discover directory, create account and order, satisfy authorization challenges, submit a CSR to the order’s finalize URL, then download the certificate. It standardizes proof and lifecycle automation, not the CA’s policy or trust level. [web:391][web:401][web:402] |
| Let’s Encrypt | Publicly trusted, automated ACME CA. HTTP-01 proves control through port 80, DNS-01 through a DNS TXT record and supports wildcards, while TLS-ALPN-01 proves control over TLS on port 443. [web:393] |

### Recommended private-CA hierarchy

```text
Offline root CA
   ├── Production issuing intermediate
   ├── Non-production issuing intermediate
   ├── Workload/SPIFFE intermediate
   └── Device/user intermediate
```

| Design element | Recommended practice |
|---|---|
| Root CA | Self-signed trust anchor used only to sign intermediates and, occasionally, root CRLs. Keep offline or administratively disabled. Its lifetime can be long because operational exposure is extremely low. |
| Intermediates | Online, constrained, and purpose-specific. Separate production/nonproduction, trust domains, device/user/server use, or regulatory zones. Constrain path length, name spaces, EKUs, and policy OIDs. |
| Issuing keys | Use clustered HSMs or cloud KMS/HSM when compromise would be catastrophic. HSM protection prevents ordinary export but does not prevent an authorized or compromised CA service from requesting malicious signatures. |
| Root custody | M-of-N custodians, split knowledge, independent security and PKI roles, sealed backup material in geographically separate locations, and documented succession when custodians leave. |
| Ceremony | Pre-approved script; inventory and verify hardware/firmware and clean media; initialize HSM; generate non-exportable root key; create root certificate; sign intermediates and initial CRL; verify every artifact; record participants, hashes, outputs, and deviations; export only public artifacts; then shut down and seal the equipment. Formal guidance recommends a physically controlled, air-gapped environment and witnessed multi-person authorization. [web:76][web:90] |
| Publication | Stable AIA, CA-chain, CRL, and OCSP URLs independent of one Vault node or datacenter. Test from each relying environment. |
| Recovery | Back up HSM state or wrapped keys according to the assurance model; rehearse replacement HSM and intermediate reissuance. An “offline root” without a tested recovery path is a single point of permanent failure. |

### Lifetimes and revocation

Short-lived certificates reduce the interval during which a stolen private key remains useful and force organizations to automate inventory and renewal. They do **not** prevent use during the remaining validity window; extremely sensitive identities may still require immediate revocation, deny-listing, or authorization-plane disablement.

| Mechanism | Strength | Weakness |
|---|---|---|
| CRL | Signed, cacheable list; no per-client privacy leak; can be mirrored. Vault supports full and delta CRLs. | Lists grow, updates are periodic, relying parties may use stale copies, and publication failure affects many certificates. |
| OCSP | Per-certificate status and potentially fresher responses. OCSP stapling avoids a client-to-CA query. | Adds availability and latency dependencies; direct queries reveal browsing/service relationships; many clients soft-fail when unavailable. |
| Short lifetime | Fails closed at `notAfter` without needing the client to obtain new revocation information. | Requires highly reliable automated issuance and renewal; compromise remains useful until expiry; a CA outage quickly becomes a fleet outage. |
| Authorization-layer revocation | Remove SPIFFE registration, mesh policy, account, or workload permission even if a certificate remains valid. | Works only where every relying service performs the additional authorization check. |

Vault has supported OCSP and delta CRLs since 1.12, but operators must configure publication URLs and monitor CRL generation on every relevant cluster. [web:122][web:123] For very short-lived certificates, natural expiration can replace revocation infrastructure in some profiles; RFC 9608 provides a “no revocation available” extension for such cases. [web:382]

### Public TLS maximum validity

As of **September 2, 2026**, CA/Browser Forum Ballot SC-081v3 has already reduced the maximum validity of newly issued publicly trusted TLS certificates:

| Issuance date | Maximum public TLS validity |
|---|---:|
| March 15, 2026–March 14, 2027 | **200 days** |
| March 15, 2027–March 14, 2029 | **100 days** |
| On or after March 15, 2029 | **47 days** |

The CA/Browser Forum adopted the phased reduction from 398 to 200, 100, and finally 47 days. [web:182][web:183][web:190] Private PKI is not automatically bound by the browser baseline, but using year-long workload certificates increasingly contradicts the security and automation model.

Let’s Encrypt still defaults to 90 days in September 2026. Its announced plan is an opt-in 45-day profile in 2026, a 64-day default beginning February 10, 2027, and a 45-day default beginning February 16, 2028. [web:5][web:10]

## 5. SPIFFE/SPIRE workload identity

**SPIFFE** defines identity and API standards; **SPIRE** is a production implementation that attests nodes and workloads and issues credentials called SVIDs.

| Element | Technical meaning |
|---|---|
| SPIFFE ID | URI identity such as `spiffe://example.org/ns/payments/sa/processor`, scoped to a trust domain. |
| X.509-SVID | Short-lived X.509 certificate with the SPIFFE ID in a URI SAN, plus a private key and trust bundle. Used directly or through Envoy/service mesh for mTLS. |
| JWT-SVID | Short-lived audience-bound JWT whose subject is the SPIFFE ID. Useful when X.509/mTLS is not the protocol boundary. |
| Node attestation | SPIRE Server verifies an Agent using platform evidence such as cloud instance identity, Kubernetes node evidence, TPM, or another attestor. |
| Workload attestation | Local SPIRE Agent inspects process/runtime selectors—Kubernetes namespace/service account/pod labels, Unix UID, container metadata, and similar attributes—and matches registration entries. |
| Workload API | Local Unix-socket API through which the workload obtains automatically rotated SVIDs and bundles without possessing a bootstrap secret. |
| Federation | Trust bundles can be exchanged between SPIFFE trust domains so separately operated environments authenticate one another. |

SPIRE performs both node and workload attestation, then continuously delivers short-lived credentials; services authenticate the peer’s SPIFFE ID rather than relying on IP address or DNS alone. [web:196][web:197][web:198]

### SPIRE versus Vault Kubernetes auth

| Dimension | SPIFFE/SPIRE | Vault Kubernetes auth |
|---|---|---|
| Primary purpose | General workload identity and mTLS/JWT authentication between services. | Authenticate a Kubernetes workload to Vault and issue a Vault token. |
| Initial evidence | Attested node plus runtime workload selectors. | Kubernetes service-account JWT validated by TokenReview and role bindings. |
| Result | Continuously rotated X.509-SVID and/or audience-bound JWT-SVID. | Vault token with policies, TTL, and access to Vault operations. |
| Peer-to-peer mTLS | Native objective. | Not by itself; workload must obtain a PKI certificate or use another mesh/identity layer. |
| Scope | Kubernetes, VMs, bare metal, multiple clouds, and federated trust domains. | Login boundary for Vault. |
| Best combined architecture | SPIRE attests and names workloads; Vault’s SPIFFE auth accepts the SVID and supplies DB credentials, PKI, KV values, or transit operations. | Avoids creating a separate Kubernetes JWT-to-Vault trust scheme for every cluster. |

Kubernetes auth is simpler when the only question is “may this pod read this Vault path?” SPIRE is stronger when the requirement is “what cryptographically attested workload is this, and can it mutually authenticate to every other service?”

## 6. Sigstore Fulcio and Rekor

Fulcio is a specialized CA for **software-signing identity**, not a general TLS server CA.

### Public keyless-signing flow

1. A developer or CI workload authenticates to an approved OIDC provider.
2. Cosign generates an ephemeral key pair locally.
3. It sends Fulcio the public key/CSR and OIDC token.
4. Fulcio verifies issuer, signature, audience, freshness, and claims.
5. Fulcio issues a short-lived X.509 code-signing certificate binding the ephemeral key to the OIDC identity—such as email, service account, repository, workflow, or Git ref.
6. The client signs the artifact.
7. The artifact digest, signature, certificate, and timing evidence are recorded in Rekor.
8. A verifier checks the artifact signature, certificate chain and identity constraints, and Rekor inclusion/timing proof.

Fulcio’s short lifetime avoids maintaining conventional private signing keys or long-lived certificate revocation. Rekor provides durable evidence that the signature existed while the certificate was valid. Fulcio certificates are required to be logged, and clients should not trust unlogged certificates in the public Sigstore model. [web:31][web:32][web:38]

Rekor is an append-only, cryptographically verifiable transparency service. It does not make a malicious signature legitimate; it makes issuance and signing observable and supplies a trusted timestamp/inclusion proof for verification after Fulcio’s certificate has expired.

### Private/self-hosted Fulcio

A private deployment normally includes:

| Component | Function |
|---|---|
| Private OIDC provider | Supplies internal human, Kubernetes, CI, or workload claims. Claim design becomes the signing authorization boundary. |
| Fulcio | Validates the chosen issuers and signs short-lived certificates using an internal CA key or external/HSM-backed signing backend. |
| Certificate-transparency log | Records Fulcio certificate issuance and helps detect misissuance. |
| Rekor | Records signatures or attestations and returns signed inclusion evidence. |
| TUF/trusted-root repository | Distributes Fulcio roots, Rekor keys, CT-log keys, validity intervals, and endpoint metadata to clients. |
| Cosign signing configuration | Points clients at the private Fulcio, Rekor, OIDC provider, TSA, and trust material rather than Sigstore’s public services. |

Fulcio explicitly supports self-hosting. Sigstore’s scaffold Helm deployment can install Fulcio, Rekor, CT log, and TUF components; production deployments must replace ephemeral development CA keys, secure database/log storage, back up signing material, and operate log monitoring. [web:406][web:408][web:413][web:415]

A private instance sacrifices public ecosystem transparency unless its log roots and monitoring are shared with relevant verifiers. Organizations must also write verification policy against stable claims—for example repository plus workflow path and protected Git ref—not merely “any user from this OIDC issuer.”

## 7. The hard operational problems

| Problem | Failure mode | Required controls |
|---|---|---|
| Key ceremonies | Ceremony is improvised, undocumented, or controlled by one administrator; nobody can reproduce intermediate renewal years later. | Version-controlled offline script and checklist, M-of-N approval, independent witness/auditor, artifact hashes, video or signed record where required, and scheduled rehearsal. |
| Root key custody | Root exists on an online VM, an exportable file, one smart card, or with departed employees. | Offline HSM, multiple independent custodians, geographic backup, succession process, periodic inventory, and tested intermediate-signing procedure. |
| HSM/KMS integration | KMS IAM change, HSM partition loss, quota, PIN loss, firmware upgrade, or key deletion seals Vault or stops CA signing. | HA HSM partitions/regions, deletion protection and organization-level deny policies, separate operator roles, capacity testing, audit-log export, and a rehearsed recovery path. |
| Rotation without downtime | New DB password, transit key, intermediate CA, token, or Connect credential is activated before all consumers can use it. | Two-phase rotation: create new, distribute trust/credential, verify adoption, cut issuance/traffic, drain old use, then revoke. For CA rotation, publish the new chain before issuing from it and retain overlapping issuers until old leaf certificates expire. |
| Secret sprawl | Same credential is copied into cloud stores, CI, Kubernetes, Helm, `.env`, tickets, password managers, and developer machines. No one knows which copy is authoritative. | One authoritative system per secret class, inventory and ownership metadata, secret scanning, consumer mapping, eliminate copies with workload federation/dynamic issuance, and test revocation. Fragmented stores otherwise produce uneven rotation, incomplete logging, and slow incident response. [web:168][web:173] |
| Auditing “who accessed what” | Shared tokens make every request look identical; audit logs show issuance but not actual downstream use; HMAC fields are misunderstood. | Unique workload identities, short-lived tokens, preserve entity/accessor/namespace/path/lease metadata, correlate Vault with cloud/DB logs, export to immutable SIEM storage, and alert on unusual read volume or role use. |
| Audit availability | Full disk or blocked socket causes Vault to refuse or hang requests. | At least two independent audit devices, separate filesystem capacity, log rotation with descriptor reopen, health metrics, and no blocking network sink in the critical path without buffering. |
| Break-glass | Emergency root credentials become everyday credentials, or nobody can retrieve them during an outage. | Offline recovery shares/root-generation procedure, two-person approval, incident ticket, time-bounded credential, isolated admin workstation, real-time alerting, mandatory post-use rotation, and periodic drill. Root tokens should be generated only for the emergency and revoked immediately afterward. |
| Dynamic-secret reconciliation | Vault believes a lease is gone but the target user remains because revocation failed. | Scheduled comparison of target accounts/keys with active leases, orphan quarantine, revoke retries, target-side maximum lifetime, and alarms on expiration-manager errors. |
| Short-certificate automation | CA or DNS outage causes a large fraction of the fleet to expire simultaneously. | Randomized renewal windows, redundant issuers/endpoints, renewal SLOs, expiry telemetry, cached valid chains, staged lifetime reductions, and chaos testing of CA/DNS/API outages. |
| Restore testing | Backups contain encrypted data but not the KMS key, Shamir quorum, Sealed Secrets keys, HSM backup, or correct CA chain. | Treat data, keys, trust bundles, configuration, and procedures as one recovery set; restore into an isolated environment and prove secret reads, lease revocation, CA issuance, and audit output. |

The governing principle is that a secrets or PKI system does not remove key management—it **concentrates it**. Security improves only if the organization gives that concentrated control plane stronger identity, custody, monitoring, availability, recovery, and change-management controls than the scattered secrets it replaced.

Citations:
[46] https://developer.hashicorp.com/vault/docs/concepts/seal
[47] https://developer.hashicorp.com/vault/docs/internals/architecture
[48] https://developer.hashicorp.com/vault/docs/enterprise/replication
[49] https://developer.hashicorp.com/vault/tutorials/enterprise/disaster-recovery
[50] https://developer.hashicorp.com/vault/docs/configuration/seal/seal-best-practices
[51] https://developer.hashicorp.com/vault/docs/about-vault/how-vault-works
[52] https://developer.hashicorp.com/vault/tutorials/archive/seal-wrap
[53] https://notes.kodekloud.com/docs/HashiCorp-Certified-Vault-Associate-Certification/Learning-the-Vault-Architecture/Seal-and-Unseal/page
[54] https://developer.hashicorp.com/vault/tutorials/monitoring/monitor-replication
[55] https://support.hashicorp.com/hc/en-us/articles/360001921007-Vault-CLI-Guide-to-Disaster-Recovery-Replication-Failover
[56] https://onboarding.hashicorp.academy/VaultOnboarding/VaultOnboarding_%20Business%20Continuity%20_%20Disaster%20Recovery%20Operations-v1.pdf
[57] https://www.hashicorp.com/en/blog/vault-0-8
[58] https://notes.kodekloud.com/docs/HashiCorp-Certified-Vault-Operations-Professional-2022/Build-Fault-Tolerant-Vault-Environments/Enable-and-Configure-Disaster-Recovery-DR-Replication/page
[59] https://developer.hashicorp.com/vault/tutorials/enterprise/performance-replication
[60] https://developer.hashicorp.com/vault/docs/concepts/policies
[106] https://openbao.org/blog/cipherboy-ossna-26-talk/
[107] https://daily.dev/posts/sustainable-secrets-management-with-openbao---open-source-siemens-2026-dmuowpt0m
[108] https://www.openbao.ch/sovereignty/
[109] https://lalatenduswain.medium.com/openbao-vs-hashicorp-vault-the-secrets-management-showdown-every-devops-team-needs-to-read-in-2026-458ae0d9a408
[110] https://openbao.org/blog/
[111] https://wetheflywheel.com/en/comparisons/openbao-vs-hashicorp-vault/
[112] https://devtoollab.com/blog/best-secrets-management-tools
[113] https://blog.stderr.at/openshift-platform/security/secrets-management/openbao/2026-02-11-openbao-part-1-introduction/
[114] https://openbao.org/
[115] https://tech-insider.org/ca/hashicorp-vault-setup-2026/
[116] https://news.ycombinator.com/item?id=44133909
[117] https://thenewstack.io/meet-openbao-an-open-source-fork-of-hashicorp-vault/
[118] https://daily.dev/posts/openbao-joins-the-openssf-to-advance-secure-secrets-management-in-open-source-rvwpv3up8
[119] https://robertdebock.nl/blog.html
[120] https://lucaberton.com/blog/openbao-alex-scheel-secrets-management-ai-agents-platformcon-2026/
[136] https://cloud.google.com/secret-manager/docs/how-to
[137] https://docs.aws.amazon.com/secretsmanager/latest/userguide/integrating_parameterstore.html
[138] https://stackharbor.com/en/knowledge-base/awsops-secrets-manager-vs-parameter-store/
[139] https://hidekazu-konishi.com/entry/aws_secrets_manager_and_parameter_store_guide.html
[140] https://learn.microsoft.com/en-us/azure/key-vault/secrets/tutorial-rotation
[141] https://www.reddit.com/r/aws/comments/1fppsw7/aws_secrets_manager_vs_parameter_store_which_is/
[142] https://docs.cloud.google.com/secret-manager/docs/cmek
[143] https://docs.aws.amazon.com/systems-manager/latest/userguide/systems-manager-parameter-store.html
[144] https://docs.cloud.google.com/secret-manager/docs/secret-rotation
[145] https://docs.cloud.google.com/secret-manager/docs/rotation-recommendations
[146] https://shieldsyncsecurity.com/blog/secrets-management-aws
[147] https://docs.cloud.google.com/secret-manager/docs
[148] https://aws.amazon.com/blogs/security/how-to-choose-the-right-aws-service-for-managing-secrets-and-configurations/
[149] https://note.com/yamazaki_cloud/n/n4f745a651b85?hl=en
[150] https://oneuptime.com/blog/post/2026-02-12-secrets-manager-vs-parameter-store/view
[151] https://infisical.com/docs/documentation/platform/dynamic-secrets/overview
[152] https://devopsboys.com/blog/infisical-secrets-manager-review-2026
[153] https://www.1password.dev/secrets-automation
[154] https://infisical.com/docs/documentation/platform/secrets-mgmt/concepts/dynamic-secrets
[155] https://www.1password.dev/service-accounts/get-started
[156] https://startwithidentity.com/vendors/secrets/1password-secrets/
[157] https://bucket.agilebits.com/security/378.2208-Recurity_Labs-2023-1Password_Secrets_Automation-Pentest.pdf
[158] https://infisical.com/docs/documentation/platform/secrets-mgmt/overview
[159] https://www.reddit.com/r/devops/comments/1blxy9u/anyone_using_infisical/
[160] https://aws.amazon.com/marketplace/reviews/reviews-list/prodview-26piwtm572nyg/review/1c375084-504c-3240-8430-971bf7f1639c
[161] https://agilebits.github.io/security-design/secrets.html
[162] https://aws.amazon.com/marketplace/reviews/reviews-list/prodview-fuedwvalknaiy
[163] https://assay.tools/packages/onepassword-api
[164] https://merginit.com/blog/23062026-free-secrets-env-management-comparison
[165] https://www.1password.community/developers-69/automated-connect-server-token-rotation-22061
[16] https://www.cybersectool.com/tools/akeyless/vs-cyberark-conjur
[17] https://aws.amazon.com/marketplace/reviews/reviews-list/prodview-nybsd7lzcdqfy
[18] https://docs.akeyless.io/docs/zero-knowledge
[19] https://docs.akeyless.io/docs/how-to-create-dynamic-secret
[20] https://dev.to/binyam/taming-the-hydra-why-your-kubernetes-secrets-management-is-broken-and-how-cyberark-conjur-fixes-f1j
[21] https://discuss.cyberarkcommons.org/t/cyberark-conjur-solution-for-securely-updating-secrets-in-kubernetes-secrets-resource/2030
[22] https://github.com/cyberark/secrets-provider-for-k8s/blob/main/README.md
[23] https://www.reddit.com/r/devops/comments/11fam1a/is_anyone_here_using_akeyless/
[24] https://aws.amazon.com/marketplace/reviews/reviews-list/prodview-nybsd7lzcdqfy?rating=5&filter=AWSMP
[25] https://docs.cyberark.com/conjur-open-source/latest/en/content/integrations/k8s-ocp/cjr-k8s-secrets-provider-ic-lp.htm
[26] https://www.zendikt.com/product/cyberark-conjur
[27] https://docs.cyberark.com/conjur-open-source/latest/en/content/integrations/k8s-ocp/cjr-secrets-provider-lp.htm
[28] https://www.capterra.com/p/212317/Akeyless-Vault/reviews/
[29] https://docs.cyberark.com/conjur-open-source/latest/en/content/integrations/k8s-ocp/k8s-set-up-apps-lp.htm
[30] https://www.reddit.com/r/CyberARk/comments/1cn6pvx/please_help_me_understand_why_conjur/
[61] https://docs.product-security.expert/cloud-kubernetes-and-infrastructure-security/index/mozilla-sops-age-kms-and-gitops-secrets
[62] https://github.com/external-secrets/external-secrets
[63] https://external-secrets.io/latest/api/externalsecret/
[64] https://external-secrets.io/v0.4.4/api-externalsecret/
[65] https://external-secrets.io/latest/introduction/faq/
[66] https://github.com/bitnami-labs/sealed-secrets/blob/main/README.md
[67] https://external-secrets.io/v0.5.9/provider-kubernetes/
[68] https://external-secrets.io/v0.5.9/api-externalsecret/
[69] https://external-secrets.io/latest/api/spec/
[70] https://github.com/bitnami-labs/sealed-secrets/blob/main/docs/developer/crypto.md
[71] https://docs.akeyless.io/docs/external-secrets-operator
[72] https://deepwiki.com/bitnami-labs/sealed-secrets/1-overview
[73] https://developers.redhat.com/articles/2025/11/11/introducing-external-secrets-operator-openshift
[74] https://github.com/bitnami-labs/sealed-secrets
[75] https://deepwiki.com/external-secrets/external-secrets
[91] https://developer.hashicorp.com/vault/tutorials/db-credentials/manage-dynamic-leases
[92] https://how2.sh/posts/how-to-rotate-database-credentials-with-vault-dynamic-secrets/
[93] https://developer.hashicorp.com/vault/docs/configuration/prevent-lease-explosions
[94] https://developer.hashicorp.com/validated-designs/vault/administration-guide/dynamic-secrets-management
[95] https://developer.hashicorp.com/vault/docs/troubleshoot/lease-issues
[96] https://support.hashicorp.com/hc/en-us/articles/48263792242579-Revoking-Vault-leases-in-bulk
[97] https://developer.hashicorp.com/vault/docs/secrets/databases
[98] https://blog.stephane-robert.info/docs/securiser/secrets/hashicorp-vault/secrets-dynamiques/
[99] https://beefed.ai/en/dynamic-secrets-automated-rotation
[100] https://blog.stephane-robert.info/docs/securiser/secrets/hashicorp-vault/database-secrets/
[101] https://notes.kodekloud.com/docs/HashiCorp-Certified-Vault-Operations-Professional-2022/Create-a-working-Vault-server-configuration-given-a-scenario/Database-Secrets-Engine/page
[102] https://github.com/hashicorp/vault/blob/main/website/content/docs/enterprise/lease-count-quotas.mdx
[103] https://developer.hashicorp.com/vault/tutorials/db-credentials
[104] https://blue.tymyrddin.dev/docs/org/startup/runbooks/vault-dynamic-credentials
[105] https://oneuptime.com/blog/post/2026-02-02-vault-dynamic-database/view
[121] https://developer.hashicorp.com/vault/docs/secrets/pki
[122] https://developer.hashicorp.com/vault/api-docs/secret/pki
[123] https://developer.hashicorp.com/vault/docs/secrets/pki/considerations
[124] https://developer.hashicorp.com/vault/docs/secrets/pki/setup
[125] https://developer.hashicorp.com/vault/tutorials/pki/pki-engine-external-ca
[126] https://developer.hashicorp.com/vault/docs/secrets/pki/rotation-primitives
[127] https://cloudfluently.com/blog/vault-pki-secrets-engine-building-your-own-certificate-authority
[128] https://developer.hashicorp.com/validated-designs/vault/administration-guide/pki-certificate-management
[129] https://tip.boundaryproject.io/vault/tutorials/pki
[130] https://developer.hashicorp.com/vault/docs/secrets/pki/quick-start-root-ca
[131] https://deepwiki.com/songlining/vault-config-as-code/9.2-certificate-issuance-roles
[132] https://nicheelab.com/en/articles/vault/pki-engine/
[133] https://axelspire.com/business/vault-pki-root-of-trust/
[134] https://www.linkedin.com/posts/arbi-ben-slimane-13997161_hashicorpvault-vaultoss-pki-activity-7317445366247796736-qptJ
[135] https://github.com/hashicorp/vault/issues/16060
[1] https://smallstep.com/docs/step-ca/provisioners/
[2] https://smallstep.com/blog/private-acme-server/
[3] https://smallstep.com/docs/tutorials/acme-protocol-acme-clients/
[4] https://smallstep.com/docs/step-ca/
[5] https://letsencrypt.org/2025/12/02/from-90-to-45
[6] https://cert-manager.io/docs/configuration/issuers/
[7] https://letsencrypt.org/2026/02/24/rate-limits-45-day-certs
[8] https://smallstep.com/docs/step-ca/acme-basics/
[9] https://smallstep.com/docs/step-ca/configuration/
[10] https://letsencrypt.org/docs/cert-lifetimes/
[11] https://ubos.tech/news/lets-encrypt-announces-major-certificate-changes-new-roots-short%E2%80%91lived-certs-and-deprecation-of-tls-client-auth/
[12] https://cert-manager.io/docs/concepts/issuer/
[13] https://letsencrypt.org/upcoming-features/
[14] https://smallstep.com/docs/step-cli/reference/ca/provisioner/add/
[15] https://smallstep.com/docs/step-ca/getting-started/
[76] https://docs.digicert.com/en/trust-lifecycle-manager/get-started/trust-architecture-playbook/issuance-pillar/building-a-hierarchy-in-digicert-private-ca.html
[77] https://beefed.ai/en/resilient-internal-pki-ca-design-best-practices
[78] https://services.google.com/fh/files/misc/deploying_public_key_infrastructure_with_cas.pdf
[79] https://www.ssl.com/article/3tier-pki-enterprise-security-offline-root-ceremonies/
[80] https://www.entrust.com/sites/default/files/documentation/whitepapers/offline-ca-best-practices-wp.pdf
[81] https://www.thesslstore.com/blog/setting-up-your-own-certificate-authority/
[82] https://www.nccoe.nist.gov/sites/default/files/legacy-files/tls-serv-cert-mgt-nist-sp1800-16d-final.pdf
[83] https://digitash.com/google/google-configure-cloud-certificate-authority-service-cas-internal-pki/
[84] https://www.qcecuring.com/education/pki/root-ca-vs-intermediate-ca
[85] https://axonshield.com/vault/security/key-management-best-practices/
[86] https://www.devopsschool.com/tutorials/google-cloud-certificate-authority-service-tutorial-architecture-pricing-use-cases-and-hands-on-guide-for-security/
[87] https://evertrust.io/blog/setting-up-an-offline-root-ca-the-ultimate-guide/
[88] https://www.ssl.com/no/Artikkel/3--niv%C3%A5-pki-enterprise-security-offline-root-ceremonies/
[89] https://docs.cloud.google.com/certificate-authority-service/docs/ca-service-overview
[90] https://www.ncsc.gov.uk/collection/in-house-public-key-infrastructure/pki-principles/keep-the-root-ca-offline-and-be-unavailable-for-use
[181] https://www.digicert.com/blog/tls-certificate-lifetimes-will-officially-reduce-to-47-days
[182] https://cabforum.org/2025/04/11/ballot-sc081v3-introduce-schedule-of-reducing-validity-and-data-reuse-periods/
[183] https://cabforum.org/working-groups/server/baseline-requirements/documents/CA-Browser-Forum-TLS-BR-2.2.2.pdf
[184] https://www.manageengine.com/key-manager/ssl-tls-certificate-lifespan-reduced-to-47-days.html
[185] https://tlsradar.com/blog/shrinking-certificate-validity-2026
[186] https://www.thesslstore.com/blog/47-day-ssl-certificate-validity-by-2029/
[187] https://www.ssl.com/article/preparing-for-47-day-ssl-tls-certificates/
[188] https://drj.com/industry_news/ca-browser-forum-passes-ballot-to-reduce-ssl-tls-certificates-to-47-day-maximum-term/
[189] https://postquantum.com/security-pqc/sc081v3-47day-certificate/
[190] https://knowledge.digicert.com/alerts/public-tls-certificates-199-day-validity
[191] https://www.digicert.com/blog/tls-certificate-rule-changes
[192] https://groups.google.com/a/groups.cabforum.org/g/servercert-wg/c/a6A2Wmu0gUw/m/4OadKg-sAAAJ
[193] https://www.encryptionconsulting.com/tls-cert-validity/
[194] https://www.sans.org/newsletters/newsbites/xxvii-29
[195] https://www.sans.org/newsletters/newsbites/xxviii-20
[196] https://spiffe.io/docs/latest/spire-about/use-cases/
[197] https://spiffe.io/docs/latest/spire-about/spire-concepts/
[198] https://spiffe.io/docs/latest/deploying/svids/
[199] https://spiffe.io/docs/latest/microservices/envoy-jwt/readme/
[200] https://developer.hashicorp.com/vault/docs/auth/kubernetes
[201] https://docs.redhat.com/en/documentation/openshift_container_platform/4.19/html/security_and_compliance/zero-trust-workload-identity-manager
[202] https://github.com/spiffe/go-spiffe/blob/main/README.md
[203] https://www.hashicorp.com/en/blog/implementing-workload-identity-with-hashicorp-vault-and-spiffe
[204] https://github.com/hashicorp/vault/blob/main/website/content/docs/agent-and-proxy/autoauth/methods/kubernetes.mdx
[205] https://developer.hashicorp.com/vault/api-docs/auth/kubernetes
[206] https://stribog.com/blog/spiffe-spire-zero-trust-workload-identity-kubernetes-mtls
[207] https://docs.redhat.com/en/documentation/openshift_container_platform/4.20/html/security_and_compliance/zero-trust-workload-identity-manager
[208] https://docs.redhat.com/en/documentation/openshift_container_platform/4.22/html/security_and_compliance/zero-trust-workload-identity-manager
[209] https://docs.redhat.com/en/documentation/openshift_container_platform/4.18/html/security_and_compliance/zero-trust-workload-identity-manager
[210] https://github.com/hashicorp/vault/blob/main/api/auth/kubernetes/kubernetes.go
[31] https://docs.sigstore.dev/about/security/
[32] https://docs.sigstore.dev/cosign/signing/overview/
[33] https://github.com/sigstore/fulcio/blob/main/docs/security-model.md
[34] https://www.systemshardening.com/articles/cicd/sigstore-keyless-signing/
[35] https://deepwiki.com/sigstore/docs/3.2-keyless-signing-workflow
[36] https://docs.sigstore.dev/about/threat-model/
[37] https://github.com/sigstore/architecture-docs
[38] https://docs.sigstore.dev/about/overview/
[39] https://perun.au/insights/sigstore-production/
[40] https://blog.sigstore.dev/privacy-in-sigstore-57cac15af0d0/
[41] https://secopslog.com/courses/supplychain-deep/sc-rekor/
[42] https://docs.sigstore.dev/logging/verify-release/
[43] https://www.stb.id.au/blog/sigstore-sorcery
[44] https://sbomify.com/2024/08/12/what-is-sigstore/
[45] https://eprint.iacr.org/2023/003.pdf
[166] https://developer.hashicorp.com/vault/tutorials/operations/emergency-break-glass-features
[167] https://nhimg.org/faq/how-do-hsms-change-pki-risk-management/
[168] https://nhimg.org/faq/what-breaks-when-secrets-management-is-fragmented-across-multiple-systems/
[169] https://securew2.com/pki/hsm-private-key-protection
[170] https://www.datocms-assets.com/2885/1547495119-guide-adopting-hashicorp-vault.pdf
[171] https://devopsaitoolkit.com/categories/vault/
[172] https://developer.hashicorp.com/vault/docs/configuration/seal/seal-best-practices
[173] https://nhimg.org/articles/secrets-manager-sprawl-exposes-the-limits-of-centralized-oversight/
[174] https://nhimg.org/articles/secrets-management-tools-in-2026-expose-the-real-nhi-gap/
[175] https://www.futurex.com/solutions/advanced-key-lifecycle
[176] https://nhimg.org/articles/devops-secrets-management-is-breaking-under-secret-sprawl/
[177] https://perun.au/insights/hashicorp-vault-production/
[178] https://indico.cern.ch/event/616635/contributions/2732591/attachments/1537085/2408516/secrets_management.pdf
[179] https://nhimg.org/community/nhi-best-practices/owasp-secrets-management-cheat-sheet-are-your-controls-keeping-up/
[180] https://aws.amazon.com/blogs/security/cloudhsm-best-practices-to-maximize-performance-and-avoid-common-configuration-pitfalls/
[241] https://developer.hashicorp.com/vault/docs/secrets/kv/kv-v2
[242] https://developer.hashicorp.com/vault/docs/secrets/kv
[243] https://developer.hashicorp.com/vault/docs/secrets/kv/kv-v2/upgrade
[244] https://developer.hashicorp.com/vault/docs/secrets/transit
[245] https://developer.hashicorp.com/vault/docs/secrets
[246] https://developer.hashicorp.com/vault/docs/secrets/ssh
[247] https://developer.hashicorp.com/vault/docs/secrets/ssh/signed-ssh-certificates
[248] https://developer.hashicorp.com/vault/api-docs/secret/kv/kv-v2
[249] https://developer.hashicorp.com/vault/docs/secrets/kv/kv-v2/cookbook/patch-data
[250] https://blog.stephane-robert.info/docs/securiser/secrets/hashicorp-vault/secrets-kv/
[251] https://developer.hashicorp.com/vault/docs/secrets/gcp
[252] https://github.com/hashicorp/vault/blob/main/website/content/docs/secrets/kv/kv-v2.mdx
[253] https://github.com/hashicorp/vault/blob/main/website/content/docs/secrets/kv/kv-v2/cookbook/delete-data.mdx
[254] https://timesofcloud.com/hashicorp/vault/kv-v2/
[255] https://developer.hashicorp.com/vault/docs/secrets/kv/kv-v2/cookbook/max-versions
[256] https://developer.hashicorp.com/vault/docs/auth/approle
[257] https://developer.hashicorp.com/vault/docs/auth/gcp
[258] https://developer.hashicorp.com/vault/docs/auth/jwt
[259] https://developer.hashicorp.com/vault/docs/auth/azure
[260] https://developer.hashicorp.com/vault/docs/auth/jwt/oidc-providers/azuread
[261] https://www.hashicorp.com/ar/resources/policies-vault
[262] https://developer.hashicorp.com/vault/docs/auth/approle/approle-pattern
[263] https://docs.enclaive.cloud/vault/tutorials/cli/access-control/policies
[264] https://developer.hashicorp.com/vault/docs/auth
[265] https://onboarding.hashicorp.academy/VaultOnboarding/VaultOnboarding_%20Vault%20Policies-v1.pdf
[266] https://developer.hashicorp.com/vault/docs/auth/spiffe
[267] https://notes.kodekloud.com/docs/HashiCorp-Certified-Vault-Associate-Certification/Create-Vault-Policies/Customizing-the-Path/page
[268] https://developer.hashicorp.com/vault/docs/auth/aws
[269] https://mpolinowski.github.io/docs/DevOps/Hashicorp/2021-08-22--hashicorp-vault-policies/2021-08-22/
[270] https://developer.hashicorp.com/vault/docs/auth/jwt/oidc-providers/secureauth
[286] https://notes.kodekloud.com/docs/HashiCorp-Certified-Vault-Associate-Certification/Learning-the-Vault-Architecture/Storage-Backends/page
[287] https://developer.hashicorp.com/vault/docs/enterprise/consistency
[288] https://developer.hashicorp.com/vault/tutorials/enterprise/performance-standbys
[289] https://developer.hashicorp.com/vault/docs/enterprise/performance-standby
[290] https://consul.qubitpi.org/vault/tutorials/raft
[291] https://developer.hashicorp.com/vault/docs/configuration/storage/raft
[292] https://github.com/hashicorp/vault/blob/main/website/content/docs/faq/ssct.mdx
[293] https://developer.hashicorp.com/vault/docs/configuration/storage/consul
[294] https://deepwiki.com/hashicorp/vault-client-go/2.5-replication-consistency
[295] https://developer.hashicorp.com/vault/docs/configuration/storage
[296] https://developer.hashicorp.com/vault/tutorials/day-one-raft/raft-reference-architecture
[297] https://developer.hashicorp.com/vault/docs/v1.15.x/configuration/storage/raft
[298] https://developer.hashicorp.com/vault/docs/internals/integrated-storage
[299] https://developer.hashicorp.com/vault/docs/concepts/storage
[300] https://vault.qubitpi.org/vault/tutorials/day-one-consul
[331] https://www.doppler.com/blog/dynamic-secrets-in-action
[332] https://docs.doppler.com/docs/secrets-rotation
[333] https://www.g2.com/products/doppler-secrets-management-platform/reviews
[334] https://www.doppler.com/platform/secrets-rotation
[335] https://www.doppler.com/blog/rotated-vs-dynamic-secrets
[336] https://www.doppler.com/guides/managing-secrets-at-scale/automating-secrets-management
[337] https://docs.doppler.com/docs/dynamic-secrets
[338] https://www.g2.com/compare/aws-secrets-manager-vs-doppler-secrets-management-platform
[339] https://www.doppler.com/blog/dynamic-secrets-secrets-management-automation
[340] https://www.g2.com/categories/secrets-management-tools
[341] https://www.doppler.com/blog/10-step-secrets-rotation-guide
[342] https://www.doppler.com/platform/secrets-manager
[343] https://docs.doppler.com/docs/secretops-beginners-series-secrets-rotation
[344] https://www.doppler.com/integrations
[345] https://www.g2.com/products/doppler-secrets-management-platform/pricing
[226] https://www.reddit.com/r/aws/comments/fd08ft/aws_secrets_manager_anyone_using_it/
[227] https://www.peerspot.com/questions/what-is-your-experience-regarding-pricing-and-costs-for-aws-secrets-manager
[228] https://news.ycombinator.com/item?id=16757364
[229] https://www.linkedin.com/posts/kasi-suresh-992675177_azure-keyvault-cloudsecurity-activity-7359632258024009729-Qq8q
[230] https://docs.aws.amazon.com/secretsmanager/latest/userguide/rotate-secrets_lambda.html
[231] https://www.g2.com/products/azure-key-vault/reviews/azure-key-vault-review-13367437
[232] https://repost.aws/questions/QUpBU6XmpzT0-W_AvtwaQQsQ/cost-of-secrets-manager
[233] https://github.com/aws-samples/aws-secrets-manager-rotation-lambdas/issues
[234] https://docs.aws.amazon.com/secretsmanager/latest/userguide/troubleshoot_rotation.html
[235] https://docs.cloud.google.com/secret-manager/docs/secret-rotation
[236] https://aws.amazon.com/secrets-manager/pricing/
[237] https://nhimg.org/community/workload-identity-management-forum/google-cloud-secret-manager-what-iam-teams-miss-at-scale/
[238] https://nhimg.org/articles/google-cloud-secret-manager-governance-for-gcp-secrets-at-scale/
[239] https://www.reddit.com/r/aws/comments/f8bqou/aws_secrets_manager_issue/
[240] https://www.akeyless.io/blog/aws-secrets-manager-cost/
[316] https://learn.microsoft.com/en-us/azure/key-vault/
[317] https://docs.aws.amazon.com/secretsmanager/latest/apireference/API_RotateSecret.html
[318] https://docs.aws.amazon.com/secretsmanager/latest/userguide/cloudtrail_log_entries.html
[319] https://docs.cloud.google.com/secret-manager/docs/audit-logging
[320] https://docs.cloud.google.com/secret-manager/regional-secrets/audit-logging-rs
[321] https://learn.microsoft.com/en-us/azure/key-vault/general/developers-guide
[322] https://aws.amazon.com/secrets-manager/
[323] https://docs.aws.amazon.com/secretsmanager/latest/userguide/rotating-secrets.md
[324] https://cloud.google.com/secret-manager/docs/audit-logging?hl=ko
[325] https://cloud.google.com/secret-manager/docs/audit-logging?hl=ja
[326] https://cloud.google.com/secret-manager/docs/audit-logging?hl=fr
[327] https://computingforgeeks.com/aws-secrets-manager-rotation-guide/
[328] https://cloud.google.com/secret-manager/regional-secrets/audit-logging-rs?hl=zh-cn
[329] https://docs.aws.amazon.com/secretsmanager/latest/userguide/rotating-secrets.html
[330] https://tech-insider.org/how-to-set-up-aws-secrets-manager-2026/
[271] https://github.com/getsops/sops
[272] https://getsops.io/docs/reference/
[273] https://getsops.io/
[274] https://github.com/age-sops/sops
[275] https://pkg.go.dev/github.com/getsops/sops/v3
[276] https://getsops.io/docs/usage/identities/age/
[277] https://github.com/getsops/sops/blob/master/README.rst
[278] https://getsops.io/docs/usage/identities/config-file/
[279] https://github.com/getsops/sops/blob/main/README.rst
[280] https://getsops.io/docs/usage/common-operations/
[281] https://nicholas-morris.com/articles/sops
[282] https://how2.sh/posts/how-to-manage-gitops-secrets-with-sops-and-age/
[283] https://github.com/humphd/osd700-winter-2024/wiki/Tech-Stack-%E2%80%90-SOPS-(Secrets-OPerationS)
[284] https://aorith.github.io/posts/secrets-sops/
[285] https://stackharbor.com/en/knowledge-base/gitops-secrets-sealed-sops-external/
[301] https://www.systemshardening.com/articles/kubernetes/external-secrets-operator-hardening/
[302] https://perun.au/insights/sealed-secrets-production
[303] https://how2.sh/posts/how-to-manage-gitops-secrets-with-sops-and-age/
[304] https://perun.au/insights/external-secrets-production/
[305] https://www.akeyless.io/blog/eso-external-secrets-operator-kubernetes/
[306] https://tessl.io/registry/nitinjain999/platform-skills/1.34.0/files/references/secrets.md
[307] https://stackharbor.com/en/knowledge-base/k8s-secrets-sealed-secrets-kubeadm-deploy/
[308] https://github.com/bitnami-labs/sealed-secrets/issues/262
[309] https://github.com/bitnami-labs/sealed-secrets/issues/357
[310] https://best-of-web.builder.io/library/bitnami-labs/sealed-secrets
[311] https://external-secrets.io/v0.5.5/faq/
[312] https://playbook.stakater.com/content/workshop/sealed-secrets/management.html
[313] https://www.reddit.com/r/kubernetes/comments/1rklv79/external_secrets_operator_in_production/
[314] https://github.com/bitnami-labs/sealed-secrets/issues/432
[315] https://iotdigitaltwinplm.com/kubernetes-secrets-management-external-secrets-operator-architecture-2026/
[211] https://developer.hashicorp.com/vault/docs/secrets/aws
[212] https://developer.hashicorp.com/vault/api-docs/secret/aws
[213] https://developer.hashicorp.com/vault/api-docs/secret/ssh
[214] https://oneuptime.com/blog/post/2026-01-26-vault-dynamic-secrets/view
[215] https://oneuptime.com/blog/post/2026-03-20-vault-dynamic-aws-credentials-opentofu/view
[216] https://developer.hashicorp.com/vault/docs/secrets/azure
[217] https://jamali.hashnode.dev/dynamic-credentials-with-hashicorp-vault
[218] https://anasrhimi.tech/blog/vault-dynamic-secrets-aws
[219] https://nedinthecloud.com/2020/08/29/use-hashicorp-vault-aws-engine-with-multiple-accounts/
[220] https://kloudvin.com/article/vault-dynamic-secrets-cicd-short-lived-credentials/
[221] https://developer.hashicorp.com/vault/docs/secrets/pki/setup
[222] https://deepwiki.com/songlining/vault-config-as-code/6.4-aws-secrets-engine-(dynamic-credentials)
[223] https://github.com/hashicorp/vault/blob/main/website/content/docs/secrets/pki/quick-start-root-ca.mdx
[224] https://developer.hashicorp.com/vault/docs/secrets/databases
[225] https://developer.hashicorp.com/vault/docs/secrets/ssh/signed-ssh-certificates
[346] https://developer.hashicorp.com/vault/docs/enterprise/namespaces
[347] https://developer.hashicorp.com/vault/docs/enterprise/namespaces/namespace-structure
[348] https://developer.hashicorp.com/vault/tutorials/enterprise/namespaces
[349] https://developer.hashicorp.com/vault/docs/audit
[350] https://github.com/hashicorp/web-unified-docs/blob/main/content/hcp-docs/content/docs/vault/get-started/manage-namespaces.mdx
[351] https://github.com/hashicorp/vault/blob/main/website/content/docs/audit/index.mdx
[352] https://notes.kodekloud.com/docs/HashiCorp-Certified-Vault-Operations-Professional-2022/Configure-Access-Control/Vault-Namespace/page
[353] https://developer.hashicorp.com/vault/docs/ui/namespaces
[354] https://www.ibm.com/support/pages/node/7264912
[355] https://timesofcloud.com/hashicorp/vault/audit-devices/
[356] https://developer.hashicorp.com/vault/docs/internals/telemetry/metrics/audit
[357] https://developer.hashicorp.com/vault/docs/audit/best-practices
[358] https://factory.ai/open-source-wikis/vault?page=systems/audit.md
[359] https://docs.redhat.com/zh-cn/documentation/red_hat_ceph_storage/4/html/object_gateway_configuration_and_administration_guide/namespaces-for-vault_rgw
[360] https://factory.ai/open-source-wikis/vault?page=features/audit-pipeline.md
[421] https://openbao.org/blog/release-v2-6-0/
[422] https://openbao.org/blog/
[423] https://openbao.org/community/release-notes/2-6-0/
[424] https://openbao.org/docs/commands/namespace/
[425] https://openbao.org/docs/configuration/seal/
[426] https://openbao.org/community/rfcs/namespace-sealing/
[427] https://openbao.org/docs/concepts/seal/
[428] https://openbao.org/docs/next/guides/sealable-namespaces/
[429] https://openbao.org/docs/next/concepts/namespaces/sealable-namespaces/
[430] https://openbao.org/docs/rfcs/namespace-sealing/
[431] https://openbao.org/community/release-notes/
[432] https://openbao.org/docs/api/system/namespaces/
[433] https://openbao.org/docs/next/release-notes/
[434] https://openbao.org/blog/namespaces-announcement/
[435] https://openbao.org/docs/
[361] https://infisical.com/platform/secrets-management
[362] https://infisical.com/
[363] https://www.g2.com/products/infisical/reviews
[364] https://devopsboys.com/blog/infisical-secrets-manager-review-2026
[365] https://infisical.com/docs/documentation/platform/secrets-mgmt/concepts/secrets-delivery
[366] https://infisical.com/docs/integrations/platforms/kubernetes-csi
[367] https://infisical.com/blog/secrets-management-complete-guide
[368] https://kx.cloudingenium.com/en/infisical-open-source-secrets-management-platform-guide
[369] https://infisical.com/docs/documentation/platform/secrets-mgmt/overview
[370] https://infisical.com/docs/documentation/guides/kubernetes-operator
[371] https://infisical.com/docs/integrations/platforms/kubernetes/overview
[372] https://infisical.com/docs/integrations/platforms/kubernetes/infisical-secret-crd
[373] https://www.pkgpulse.com/guides/infisical-vs-doppler-vs-hashicorp-vault-secrets-2026
[374] https://makerstack.co/reviews/infisical-review/
[375] https://infisical.com/blog/how-to-manage-secrets-in-kubernetes
[436] https://learn.microsoft.com/en-us/azure/key-vault/general/service-limits
[437] https://learn.microsoft.com/en-us/azure/key-vault/general/overview-throttling
[438] https://learn.microsoft.com/en-us/azure/azure-resource-manager/management/azure-subscription-service-limits
[439] https://learn.microsoft.com/en-us/azure/key-vault/secrets/tutorial-rotation
[440] https://learn.microsoft.com/en-us/azure/key-vault/secrets/tutorial-rotation-dual
[441] https://learn.microsoft.com/en-us/azure/key-vault/general/whats-new
[442] https://learn.microsoft.com/en-us/answers/questions/2007074/transaction-limits-retrieval-of-secrets-from-azure
[443] https://oneuptime.com/blog/post/2026-02-16-how-to-configure-azure-key-vault-secret-rotation-using-azure-functions-and-event-grid/view
[444] https://learn.microsoft.com/en-us/azure/event-grid/event-schema-key-vault
[445] https://safeguard.sh/resources/blog/best-practices-for-rotating-secrets-stored-in-azure-key-vault
[446] https://learn.microsoft.com/en-us/azure/reliability/reliability-key-vault
[447] https://oneuptime.com/blog/post/2026-03-31-dapr-secret-rotation-azure-key-vault/view
[448] https://www.azurecitadel.com/cmk/keyvaults/
[449] https://techcommunity.microsoft.com/blog/integrationsonazureblog/automate-secret-rotation-in-key-vault/3275149
[450] https://learn.microsoft.com/en-us/answers/questions/224400/azure-key-vault-limits
[376] https://pki.goog/repo/cps/5.22/GTS-CPS.html
[377] https://www.sectigo.com/root-causes/root-causes-283-google-optional-ocsp-proposal-clarified
[378] https://www.sectigo.com/resource-library/root-causes-283-google-optional-ocsp-proposal-clarified
[379] https://axelspire.com/vault/operations/certificate-revocation-crl-ocsp/
[380] https://www.ieee-security.org/TC/W2SP/2012/papers/w2sp12-final9.pdf
[381] https://pki.goog/repo/cps/4.21/GTS-CPS.html
[382] https://www.digicert.com/blog/solving-short-lived-certificates
[383] https://csrc.nist.gov/csrc/media/publications/nistir/7924/draft/documents/draft_nistir_7924.pdf
[384] https://www.sectigo.com/resource-library/root-causes-281-google-proposes-optional-ocsp
[385] https://nhimg.org/faq/what-breaks-when-certificate-revocation-depends-on-crls-or-ocsp-alone/
[386] https://techcommunity.microsoft.com/discussions/security-baselines/your-connection-isnt-private-on-edge-after-hardening-plus-no-home-page/3385891/replies/3391162
[387] https://www.encryptionconsulting.com/ocsp-vs-crl/
[388] https://nvlpubs.nist.gov/nistpubs/specialpublications/nist.sp.800-57pt1r5.pdf
[389] https://nhimg.org/faq/what-is-the-difference-between-short-lived-certificates-and-traditional-certific/
[390] https://www.arxiv.org/pdf/1906.10775
[391] https://www.rfc-editor.org/pdfrfc/rfc8555.txt.pdf
[392] https://www.tech-invite.com/y85/tinv-ietf-rfc-8555-2.html
[393] https://letsencrypt.org/docs/challenge-types/
[394] https://docs.keyfactor.com/ejbca/latest/acme
[395] https://letsencrypt.com/de/docs/challenge-types/
[396] https://cps.letsencrypt.org/es/docs/challenge-types/
[397] https://www.ans.co.uk/docs/domains-and-dns-management/ssl/lets-encrypt/lets-encrypt-how/
[398] https://letsencrypt.com/ja/docs/challenge-types/
[399] https://deepwiki.com/natemcmaster/LettuceEncrypt/2.3-challenge-types
[400] https://community.letsencrypt.org/t/how-can-we-fix-a-bug-in-the-acme-protocol/200027
[401] https://www.rfc-editor.org/rfc/rfc8555.html
[402] https://docs.aws.amazon.com/acm/latest/userguide/acm-acme.html
[403] https://www.machinecert.com/resources/blog/automate-ssl-certificate-renewal-acme
[404] https://hexdocs.pm/ssl_cert/1.0.0/acme_protocol.html
[405] https://www.tech-invite.com/y85/tinv-ietf-rfc-8555-4.html
[406] https://docs.sigstore.dev/cosign/system_config/custom_components/
[407] https://docs.sigstore.dev/logging/overview/
[408] https://github.com/sigstore/scaffolding/blob/main/README.md
[409] https://github.com/sigstore/fulcio
[410] https://medium.com/sigstore/scaffolding-sigstore-e893eb962f22
[411] https://oneuptime.com/blog/post/2026-01-25-sigstore-supply-chain-security/view
[412] https://docs.sigstore.dev/logging/installation/
[413] https://docs.sigstore.dev/certificate_authority/overview/
[414] https://github.com/sigstore/architecture-docs
[415] https://github.com/sigstore/fulcio/blob/main/docs/setup.md
[416] https://blog.sigstore.dev/a-guide-to-running-sigstore-locally-f312dfac0682/
[417] https://github.com/sigstore/fulcio/blob/main/CHANGELOG.md
[418] https://github.com/sigstore/docs/blob/main/content/en/certificate_authority/certificate-issuing-overview.md
[419] https://github.com/sigstore/fulcio/blob/main/fulcio.swagger.json
[420] https://docs.sigstore.dev/cosign/signing/overview/
[481] https://openbao.org/community/release-notes/
[482] https://github.com/openbao/openbao/releases
[483] https://openbao.org/community/release-notes/2-6-0/
[484] https://endoflife.date/openbao
[485] https://newreleases.io/project/github/openbao/openbao/release/v2.6.1
[486] https://openbao.org/docs/release-notes/
[487] https://openssf.org/blog/2026/08/06/announcing-openbao-v2-6/
[488] https://openbao.org/blog/openbao-joins-the-openssf/
[489] https://openbao.org/community/contributing/
[490] https://openbao.org/docs/next/contributing/
[491] https://www.opentechhub.io/openbao/
[492] https://github.com/openbao/openbao
[493] https://www.linkedin.com/posts/upfortify_cve-2026-16142-truebooker-lt-126-activity-7494668577921368064-B9JY
[494] https://openbao.org/blog/cipherboy-ossna-26-talk/
[495] https://github.com/openbao/openbao/activity
[526] https://www.1password.dev/service-accounts/rate-limits
[527] https://www.1password.community/developers-69/service-account-rate-limits-15-minutes-block-no-backoff-duration-shown-23967
[528] https://www.1password.dev/secrets-automation
[529] https://www.1password.dev/service-accounts/get-started
[530] https://www.1password.community/discussions/developers/re-service-account-rate-limits-15-minutes-block-no-backoff-duration-shown/167186
[531] https://startwithidentity.com/vendors/secrets/1password-secrets/
[532] https://toolradar.com/tools/onepassword-secrets
[533] https://releases.1password.com/developers/cli/2.26.0/
[534] https://www.1password.community/discussions/developers/your-client-has-been-rate-limited/85392/replies/85411
[535] https://www.1password.dev/connect/security
[536] https://developer.1password.com/llms-sdks.txt
[537] https://www.1password.dev/service-accounts
[538] https://developer.1password.com/docs/cli/reference/management-commands/service-account/
[539] https://www.reddit.com/r/1Password/comments/1c2jsq7/ratelimit_questions/
[540] https://www.1password.community/developers-69/automated-connect-server-token-rotation-22061
[511] https://docs.cyberark.com/conjur-enterprise/12.1/en/content/deployment/highavailability/certificate-architecture.htm
[512] https://github.com/cyberark/kubernetes-conjur-deploy/blob/master/README.md
[513] https://docs.cyberark.com/conjur-enterprise/12.2/en/content/integrations/k8s-ocp/k8s-follower.htm
[514] https://docs.cyberark.com/conjur-enterprise/latest/en/content/integrations/k8s-ocp/k8s-architecture.htm
[515] https://github.com/cyberark/kubernetes-conjur-deploy
[516] https://developer.cyberark.com/blog/how-we-architected-the-conjur-service-to-handle-infinite-traffic/
[517] https://docs.cyberark.com/conjur-enterprise/13.3/en/content/deployment/dap/dap-followers-lp.htm
[518] https://github.com/cyberark/conjur
[519] https://docs.cyberark.com/conjur-enterprise/12.4/en/content/deployment/highavailability/certificate-architecture.htm
[520] https://docs.cyberark.com/conjur-enterprise/latest/en/content/deployment/highavailability/high_availability_overview.html?TocPath=Setup%7C_____1
[521] https://www.g2.com/products/cyberark-conjur/reviews
[522] https://github.com/cyberark/conjur/blob/master/design/authenticators/AUTHENTICATORS.md
[523] https://www.zendikt.com/product/cyberark-conjur
[524] https://docs.cyberark.com/conjur-enterprise/latest/en/content/integrations/k8s-ocp/k8s-follower-lp.htm
[525] https://docs.cyberark.com/conjur-enterprise/12.2/en/content/deployment/highavailability/cluster-guidelines.htm
[496] https://www.cncf.io/projects/external-secrets/
[497] https://envmanager.com/blog/external-secrets-operator
[498] https://bex.co/blog/2026/07/12/sops-cncf-sandbox-age-encryption-self-hosted-secrets
[499] https://github.com/external-secrets/external-secrets
[500] https://github.com/bitnami-labs/sealed-secrets/issues/1785
[501] https://blog.container-solutions.com/external-secrets-operator-accepted-into-the-cncf-sandbox
[502] https://www.cncf.io/blog/2026/06/09/solving-secret-sprawl-in-multi-account-kubernetes-with-external-secrets-operator/
[503] https://github.com/cncf/toc/issues
[504] https://psirt.com/advisory/BIT-sealed-secrets-2026-22728
[505] https://www.reddit.com/r/kubernetes/comments/1qkrwmv/external_secrets_operator_in_its_next_release/
[506] https://codingprotocols.com/blog/external-secrets-operator-kubernetes
[507] https://github.com/getsops/sops
[508] https://oneuptime.com/blog/post/2026-02-26-argocd-health-checks-sealed-secrets/view
[509] https://cloudnative-pg.github.io/docs/1.26/cncf-projects/external-secrets/
[510] https://github.com/cncf/toc/issues/1819
[541] https://developer.hashicorp.com/vault/docs/secrets/transit
[542] https://developer.hashicorp.com/vault/tutorials/encryption-as-a-service/eaas-transit
[543] https://developer.hashicorp.com/vault/tutorials/encryption-as-a-service/eaas-transit-rewrap
[544] https://github.com/hashicorp/vault/blob/main/website/content/docs/configuration/seal/transit-best-practices.mdx
[545] https://gist.github.com/ruanbekker/e8f18a929bed843c7af4552eb70c727f
[546] https://notes.kodekloud.com/docs/HashiCorp-Certified-Vault-Associate-Certification/Compare-and-Configure-Secrets-Engines/Encrypting-Data-with-the-Transit-Secrets-Engine/page
[547] https://docs.spring.io/spring-vault/docs/current/api/org/springframework/vault/core/VaultTransitTemplate.html
[548] https://dev.to/mattiasfjellstrom/vault-associate-certification-part-10-explain-encryption-as-a-service-4dk4
[549] https://stackoverflow.com/questions/61286584/hashicorp-vault-should-you-store-customer-provided-secrets-in-kv
[550] https://dev.to/admantium/hashicorp-vault-transit-secrets-engine-2kam
[551] https://oneuptime.com/blog/post/2026-01-26-vault-transit-secrets/view
[552] https://oneuptime.com/blog/post/2026-02-09-vault-transit-encryption-service/view
[553] https://developer.hashicorp.com/vault/api-docs/secret/transit
[554] https://timesofcloud.com/hashicorp/vault/encryption-as-service/
[555] https://github.com/hashicorp/vault-guides/blob/master/encryption/vault-transit-rewrap/README.md
[466] https://developer.hashicorp.com/vault/docs/secrets/databases
[467] https://developer.hashicorp.com/vault/api-docs/secret/databases
[468] https://developer.hashicorp.com/vault/docs/secrets/databases/mysql-maria
[469] https://nicheelab.com/en/articles/vault/database-secrets/
[470] https://docs.devnetexperttraining.com/static-docs/HashiCorp-Vault/docs/plugin-portal/
[471] https://notes.kodekloud.com/docs/HashiCorp-Certified-Vault-Operations-Professional-2022/Create-a-working-Vault-server-configuration-given-a-scenario/Database-Secrets-Engine/page
[472] https://developer.hashicorp.com/vault/tutorials/db-credentials/database-root-rotation
[473] https://developer.hashicorp.com/vault/docs/secrets/databases/postgresql
[474] https://developer.hashicorp.com/vault/docs/secrets/aws
[475] https://developer.hashicorp.com/vault/docs/secrets/databases/oracle
[476] https://it.vt.edu/content/dam/it_vt_edu/dcss/2022/fall/dcss_vault.pdf
[477] https://deepwiki.com/hashicorp/vault-client-typescript/6.4-database-secrets-engine
[478] https://developer.hashicorp.com/vault/tutorials/db-credentials/database-secrets
[479] https://discuss.hashicorp.com/t/how-database-root-password-rotation-works/30979
[480] https://developer.hashicorp.com/vault/tutorials/get-started/understand-static-dynamic-secrets
[451] https://www.globenewswire.com/news-release/2023/08/10/2723189/0/en/HashiCorp-adopts-the-Business-Source-License-for-future-releases-of-its-products.html
[452] https://www.hashicorp.com/en/blog/hashicorp-adopts-business-source-license
[453] https://newsroom.ibm.com/campaign?item=2153
[454] https://newsroom.ibm.com/2025-02-27-ibm-completes-acquisition-of-hashicorp,-creates-comprehensive,-end-to-end-hybrid-cloud-platform
[455] https://newsroom.ibm.com/mergers-and-acquisitions
[456] https://www.hashicorp.com/en/license-faq
[457] https://www.hashicorp.com/en/bsl
[458] https://www.hashicorp.com/en/press
[459] https://www.theregister.com/software/2025/02/28/ibm-likes-hashicorp-finally-puts-a-64b-ring-on-it/857825
[460] https://siliconangle.com/2025/02/27/ibm-completes-6-4b-hashicorp-acquisition-following-regulatory-approvals/
[461] https://finance.yahoo.com/news/ibm-closes-6-4b-hashicorp-142049324.html
[462] https://newsroom.ibm.com/mergers-and-acquisitions?item=33785
[463] https://www.hashicorp.com/en/blog/hashicorp-officially-joins-the-ibm-family
[464] https://techcrunch.com/2025/02/25/ibms-6-4b-hashicorp-acquisition-cleared-by-uk/
[465] https://vivianvoss.net/blog/the-terms-you-did-not-sign
[556] https://openbao.org/community/release-notes/2-6-0/
[557] https://lwn.net/Articles/1090871/
[558] https://lwn.net/Articles/1090870/
[559] https://github.com/openbao/openbao/releases
[560] https://endoflife.date/openbao
[561] https://openssf.org/blog/2026/08/06/announcing-openbao-v2-6/
[562] https://www.johnburns.io/post/test-openbao-policy-boundaries-before-deployment/
[563] https://openbao.org/community/release-notes/
[564] https://www.linkedin.com/posts/activity-7495707689264799744-oRPg
[565] https://github.com/openbao/openbao
[566] https://library.mikesailab.com/bookmarks/security-toolkit/2026-08-26/
[567] https://github.com/openbao/openbao/blob/main/api/client.go
[568] https://github.com/openbao/openbao/activity
[569] https://openbao.org/docs/next/release-notes/
[570] https://openbao.org/docs/commands/version-history/
