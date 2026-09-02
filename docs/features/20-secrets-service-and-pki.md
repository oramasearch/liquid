# Secrets service and PKI

> Research: [14-secrets-and-encryption](../research/14-secrets-and-encryption/) ·
> [13-supply-chain-security](../research/13-supply-chain-security/)
>
> Companion to [11-secrets-and-encryption](11-secrets-and-encryption.md), which covers
> encrypted data **at rest in a repository**. This document covers the **runtime
> service**: minting secrets and issuing certificates.

## The distinction that matters

Two different problems, routinely conflated:

| | [11 — repository encryption](11-secrets-and-encryption.md) | **20 — this document** |
|---|---|---|
| **What** | Encrypted data stored and versioned in a repository | Secrets and certificates minted on demand |
| **Lifetime** | Permanent; history retains ciphertext forever | Short-lived; leased and revocable |
| **Right for** | Declarative config, bootstrap material, recovery data, private assets | Database credentials, cloud IAM, API tokens, TLS and mTLS |
| **Wrong for** | Live production credentials | Anything that must be versioned and reviewed |

Doc 11 says live production credentials should be *referenced* from the repository and
*minted at deploy time*. **This is the thing that mints them.** That closes the loop.

## The PKI serves TAAP

Stated up front to avoid two competing notions of identity: **[TAAP/1](../standards/taap-1-spec-draft.md)
is liquid's security model** ([25](25-identity-attestation-and-transparency.md)). The
certificates described here are **short-lived credentials issued to TAAP keys** for
online operations — they do not establish a separate identity, and a TAAP identity
remains valid and verifiable when every certificate has expired.

## Why lake is unusually well placed to do this

A secrets service needs three things it normally has to acquire: an identity system to
authenticate callers, an authorisation model to decide who may unwrap what, and an
audit trail. lake has all three already ([19](19-identity.md)).

And the PKI half is not a new subsystem at all. **Sigstore's Fulcio is a certificate
authority** — it issues short-lived certificates bound to an OIDC identity. lake is
already an OIDC issuer and already issues short-lived signing certificates for
provenance ([10](10-supply-chain.md)).

```
lake identity (OIDC)
   ├─→ short-lived signing certs   → signed changes, actions, artifacts
   ├─→ short-lived TLS/mTLS certs  → service identity on the platform
   └─→ short-lived credentials     → databases, cloud IAM, APIs
```

One CA, one identity, three products. The signing story and the PKI story are the same
machinery pointed at different certificate profiles.

## Dynamic secrets

The single biggest security improvement over static secrets, and the design centre of
this service.

Rather than storing a database password, lake **holds the capability to create one**.
An application asks for database access; lake creates a credential scoped to that
application with a short TTL, returns it, and revokes it when the lease expires.

Consequences: nothing long-lived to leak; every credential is attributable to a
principal and a moment; revocation is real rather than aspirational; and a compromised
credential expires on its own.

The operational hazards are known and must be designed for, not discovered:

- **Lease storms** — mass expiry causing synchronised renewal traffic. Jittered TTLs
  and staggered renewal.
- **Revocation at scale** — revoking thousands of leases against a backend that
  rate-limits.
- **Connection pooling with rotating credentials** — long-lived pools outlive the
  credential that opened them. Applications need a renewal path, and lake must
  document it rather than assume it.

## Static secrets

Still needed, and supported: versioned key-value storage with history, soft delete and
metadata, in the manner of Vault's KV v2.

But the product opinion is explicit: **static secrets are the fallback, dynamic
secrets are the default.** The UI should make the better choice the easier one.

## Encryption as a service

Encrypt, decrypt, sign, verify and derive keys **without the key ever leaving lake** —
Vault's transit model.

This is what lets an application encrypt user data without ever holding the key
material, and it shares the envelope-encryption machinery already specified for
repository encryption ([11](11-secrets-and-encryption.md)). Key rotation rewraps data
keys rather than re-encrypting data.

## PKI

**Private CA, properly operated.** Offline root, online intermediates, HSM- or
KMS-backed keys, documented key ceremony, and published policy. Getting this right is
mostly operational discipline rather than code, and the discipline is the product.

**Certificate profiles:**

| Profile | Use |
|---|---|
| **Workload identity** | X.509 and JWT SVIDs in the SPIFFE model — service identity and mTLS on the platform ([13](13-deployment-runtime.md)) |
| **Signing** | Short-lived, OIDC-bound, Fulcio-style ([10](10-supply-chain.md)) |
| **TLS server** | Internal services; ACME endpoint for automated issuance and renewal |
| **Client / device** | Human and device authentication |
| **SSH** | SSH certificates rather than distributed public keys |

**Short lifetimes over revocation infrastructure.** The industry has moved this way
for good reason, and the CA/Browser Forum has been steadily reducing maximum public
certificate lifetimes. A certificate valid for an hour makes CRL and OCSP largely
moot. lake defaults to short lifetimes with automated renewal, and treats long-lived
certificates as an exception requiring justification.

**ACME support**, so existing tooling — cert-manager, the whole Let's Encrypt
ecosystem — works against lake unchanged.

## Authentication for callers

Workloads authenticate by **attestation**, not by a bootstrap secret:

- Kubernetes service account tokens on the platform.
- OIDC/JWT federation from other providers.
- Cloud instance identity.
- Platform-native workload identity for anything lake deployed, which is the common
  case and needs no configuration.

The bootstrapping problem — "what secret lets you fetch secrets?" — is solved by
attestation rather than by a first secret, because lake already knows what it
deployed.

## Audit

Every issuance, access, renewal and revocation is recorded: which principal, which
secret or certificate, when, from where, and under which policy.

Certificate issuance additionally lands in a **transparency log**
([09](09-artifact-service.md)) — the same append-only Merkle structure used for
artifacts, so unauthorised issuance is detectable rather than merely prohibited.

## Break-glass

An emergency path that is **auditable, time-boxed and loud**. Emergency access is a
legitimate requirement; silent emergency access is not. Root key recovery uses
threshold shares (Shamir) with a documented, *rehearsed* quorum procedure.

Recovery must be tested. An untested recovery procedure is not a recovery procedure.

## The honest limits

- **A secrets service is a very high-value target.** Its compromise is total. This
  raises the security bar for lake as a whole, not just for this component.
- **Availability becomes critical.** If applications mint credentials at runtime,
  lake being down stops them from starting. Caching, graceful degradation and a
  documented failure mode are required, not optional.
- **Encryption at rest is not zero-knowledge.** If lake can unwrap keys, lake can
  read data. Say so plainly; offer client-held keys where customers require true
  zero-knowledge, and be explicit about the features that then stop working
  ([11](11-secrets-and-encryption.md)).
- **Revocation does not recall.** Revoking access prevents future issuance; it does
  not retrieve what was already read.

## Open questions

- **Vault-compatible API, or a clean one?** Compatibility buys immediate ecosystem
  access — External Secrets Operator, existing client libraries — at the cost of
  inheriting a large, awkward surface. This is a significant early decision.
- **HSM strategy.** Cloud KMS is easier; a real HSM is what regulated customers ask
  for. Which comes first depends on the target customer.
- **Whether to run a public-facing CA at all**, which would require WebTrust audit and
  CA/Browser Forum compliance — a categorically different undertaking from a private
  CA. Almost certainly out of scope, and worth writing down as out of scope.
- **Namespacing and multi-tenancy** for the secrets engine, which is where Vault's
  complexity mostly lives.
- **Scope sequencing.** This is a full product competing with Vault/OpenBao. It
  shares infrastructure with the rest of lake, which makes it cheaper than building it
  standalone — but not cheap.
