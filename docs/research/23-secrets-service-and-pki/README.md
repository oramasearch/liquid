# Secrets service and PKI

> Evidence: [`sources.md`](sources.md)
>
> Context for **[20-secrets-service-and-pki](../../features/20-secrets-service-and-pki.md)**.
> Companion to [14-secrets-and-encryption](../14-secrets-and-encryption/), which covers
> encrypted data at rest in a repository rather than a runtime service.

## The bottom line

Vault remains the broadest general-purpose platform when an organisation needs dynamic
credentials, PKI, encryption-as-a-service, multi-cloud authentication, leasing and
centralised policy. **Its main cost is operational complexity.** Cloud secret managers
and developer-focused products are easier but mostly emphasise *storage and rotation*
rather than universal just-in-time credential brokering.

And one structural recommendation worth taking seriously: **SPIFFE/SPIRE is usually a
better identity plane than treating Vault authentication itself as workload identity.**
The secrets service should *consume* an identity, not *be* the identity system.

## Vault's architecture, and what to learn from it

The pieces that matter:

- **Barrier and seal/unseal.** All storage is encrypted behind a barrier; the master
  key is protected by Shamir shares or auto-unseal against a KMS/HSM.
- **Secrets engines** — KV v1/v2, database dynamic secrets, **transit**
  (encryption-as-a-service), PKI, SSH, and cloud IAM engines.
- **Auth methods** — AppRole, Kubernetes, JWT/OIDC, cloud instance identity.
- **Leases** — every dynamic secret has a TTL and can be revoked, individually or by
  prefix. This is the core idea.
- **Policies** — path-based HCL ACLs; **namespaces** for multi-tenancy.
- **Audit devices**, and replication for scale.

Namespaces and policy are where Vault's complexity actually lives, and any
reimplementation should expect that.

## OpenBao: the licensing fork

Relevant because it changes what "adopt Vault" means:

- HashiCorp moved Vault from MPL-2.0 to **BUSL 1.1 in August 2023** — source-available,
  restricts competing offerings, each release converting to MPL after four years.
- **IBM completed its HashiCorp acquisition on 27 February 2025.**
- **OpenBao** forked the last MPL lineage and stays **MPL-2.0**. As of September 2026 it
  is an **OpenSSF Sandbox project under the Linux Foundation**, governed by a public
  Technical Steering Committee (ControlPlane, SAP/Liquid Reply, Adfinis, Wallix,
  IOTech, GitLab) rather than one vendor.
- Current release **OpenBao 2.6.2 (18 August 2026)**. It has diverged rather than merely
  rebasing — namespaces are in the open distribution, plus per-namespace Shamir seals
  with separate barrier material, external `kms` auto-unseal plugins, distroless
  images, and authenticated root-generation endpoints.

The guidance: choose OpenBao when OSI licensing and shared governance matter; choose
Vault when first-party enterprise support, HCP and mature multi-region/HSM features
outweigh BUSL and vendor control.

**For liquid this matters twice over** — as a possible component, and as a warning
about what happens to infrastructure people depend on when its licence changes.

## Dynamic secrets are the point

The single biggest security win over static secrets: a stolen credential has a
**bounded lifetime and blast radius**.

Rather than storing a password, the service holds the capability to *create* one —
minting a database credential, cloud IAM credential or SSH certificate scoped to a
caller with a short TTL, then revoking it at lease end.

The operational hazards are real and must be designed for: lease storms from
synchronised expiry, revocation at scale against rate-limited backends, and connection
pools that outlive the credential that opened them.

## SOPS is not a runtime secrets service

Worth stating because the two are constantly conflated. SOPS does Git-friendly
structured encryption with per-document data keys wrapped by KMS, Vault, age or PGP,
leaving structure reviewable and MAC-authenticating the document.

It has **no leases, no revocation, no per-read audit, no dynamic credentials and no
target rotation.** Decryption keys must reach CI/CD or a GitOps controller, and
plaintext still lands in Kubernetes Secrets.

This is precisely the boundary between
[14-secrets-and-encryption](../14-secrets-and-encryption/) and this document, and
liquid must keep the two clearly separated in its own product.

## PKI: the industry has moved to short lifetimes

**Public TLS maximum validity is now on a phased reduction** under CA/Browser Forum
Ballot SC-081v3:

| Issued between | Maximum validity |
|---|---:|
| 15 Mar 2026 – 14 Mar 2027 | **200 days** |
| 15 Mar 2027 – 14 Mar 2029 | **100 days** |
| On or after 15 Mar 2029 | **47 days** |

Let's Encrypt still defaults to 90 days as of September 2026, with an opt-in 45-day
profile in 2026, a **64-day default from 10 February 2027**, and a **45-day default
from 16 February 2028**.

Private PKI is not bound by the browser baseline — but as the research puts it, using
year-long workload certificates *"increasingly contradicts the security and automation
model."*

### Revocation versus expiry

An honest comparison, and short lifetimes are not a free win:

| Mechanism | Strength | Weakness |
|---|---|---|
| **CRL** | Signed, cacheable, mirrorable; no per-client privacy leak | Lists grow; periodic updates; stale copies; publication failure affects many certificates |
| **OCSP** | Per-certificate, potentially fresher; stapling avoids client-to-CA queries | Availability and latency dependency; direct queries leak service relationships; clients often soft-fail |
| **Short lifetime** | Fails closed at `notAfter` with no client action | **Requires highly reliable automated issuance — a CA outage quickly becomes a fleet outage.** Compromise remains useful until expiry |
| **Authorisation-layer revocation** | Remove the SPIFFE registration, mesh policy or account even while the certificate is valid | Only works where every relying service performs the check |

Short lifetimes do **not** prevent use during the remaining validity window; very
sensitive identities still need immediate revocation or authorisation-plane
disablement. RFC 9608 provides a "no revocation available" extension for profiles where
expiry genuinely replaces revocation.

### CA structure

Offline (or administratively disabled) root used only to sign intermediates; online
intermediates doing the issuing; HSM- or KMS-backed keys; documented key ceremony. The
root's lifetime can be long precisely because its operational exposure is near zero.

## SPIFFE/SPIRE

The standard for workload identity: X.509 and JWT **SVIDs**, issued after *attesting*
the workload rather than after it presents a bootstrap secret. This solves the
"what secret lets you fetch secrets?" problem structurally.

The research's recommendation is architectural: use SPIFFE/SPIRE as the identity plane
and let the secrets service consume that identity, rather than making the secrets
service's own auth methods the identity system.

## Fulcio: the CA that already exists in liquid's design

Sigstore's **Fulcio issues short-lived certificates bound to an OIDC identity**, with
issuance recorded in **Rekor's transparency log**. Private and self-hosted deployments
are supported.

This is the observation that collapses two of liquid's subsystems into one: the keyless
signing described in [13-supply-chain-security](../13-supply-chain-security/) *is* a
certificate authority. lake is already an OIDC issuer
([19-identity](../../features/19-identity.md)), so the signing CA and the PKI service
are the same machinery with different certificate profiles.

## The hard operational problems

These are where secrets services actually fail, and they are mostly not code:

- **Key ceremonies and root custody** — Shamir quorum procedures that must be
  *rehearsed*, not merely documented.
- **HSM and cloud KMS integration** — easy with KMS, and regulated customers ask for
  HSM.
- **Rotation without downtime.**
- **Secret sprawl** — the thing the service is meant to fix, which it can also cause.
- **Auditing who accessed what**, per read.
- **Break-glass** — legitimate, and it must be time-boxed and loud.

## Implications for liquid

1. **The PKI and the signing CA are one component.** Fulcio-style OIDC-bound
   short-lived certificates already underpin liquid's provenance
   ([13](../13-supply-chain-security/)). Adding TLS, mTLS, SSH and client profiles is a
   configuration surface on an existing CA, not a new subsystem. This is the strongest
   argument that the secrets/PKI scope is cheaper for lake than for anyone else.

2. **Consume workload identity; do not reinvent it.** Follow SPIFFE/SPIRE semantics for
   service identity. Because lake deploys the workload, it can attest it directly —
   which is the bootstrapping problem solved by construction.

3. **Dynamic secrets are the default; static storage is the fallback.** Design the
   product so the safer choice is the easier one, and design explicitly for lease
   storms, bulk revocation and connection-pool renewal.

4. **Default to short certificate lifetimes with automated renewal**, in line with the
   200 → 100 → 47 day trajectory — and accept the corollary: **a CA outage becomes a
   fleet outage**. High availability for issuance is a hard requirement, not a
   nice-to-have, and it must be stated in the feature spec.

5. **Support ACME**, so cert-manager and the entire Let's Encrypt tooling ecosystem
   work against lake unchanged.

6. **Log issuance to the transparency log.** liquid already runs one for artifacts
   ([12-artifact-distribution](../12-artifact-distribution/)); Rekor's precedent shows
   the same structure makes unauthorised certificate issuance *detectable* rather than
   merely prohibited.

7. **Keep the two secrets stories separate in the product.** Repository encryption
   ([14](../14-secrets-and-encryption/)) versions ciphertext forever; the secrets
   service mints leased, revocable credentials. SOPS's limitations are the cautionary
   example of what happens when users expect one to do the other's job.

8. **Take the licensing lesson.** BUSL, an acquisition and a Linux Foundation fork
   happened to the incumbent in three years. liquid's own licence and governance are a
   product decision that customers will evaluate, not an afterthought.

9. **Availability is now a security property.** A secrets service is a very
   high-value target whose compromise is total, and whose *unavailability* stops
   applications from starting. Both must be designed for.
