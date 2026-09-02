# Identity and auth

> Research: [13-supply-chain-security](../research/13-supply-chain-security/) ·
> [21-consumer-sync-and-web](../research/21-consumer-sync-and-web/) ·
> [14-secrets-and-encryption](../research/14-secrets-and-encryption/)
>
> Clerk-quality authentication, provided by lake.

## Why this is not a bolt-on

Identity looks like table stakes — sign-in, orgs, teams, permissions. In liquid it is
structural, because **the identity system is the trust root for the entire chain of
custody**.

The connection is direct. Keyless signing works by binding a short-lived certificate
to an **OIDC identity** ([10](10-supply-chain.md)). If lake is an OIDC issuer, then:

```
a person or workload authenticates to lake
        ↓  OIDC identity
short-lived signing certificate
        ↓
signed change · signed action · signed artifact · signed deployment
        ↓
verifiable provenance, end to end
```

Every existing stack federates this across three or four vendors — an identity
provider, a CI system, a signing service and a registry — and every **trust boundary**
between them is a place the chain can be broken: each handoff re-asserts an identity
that the next system takes on faith. lake issues the identity, executes the build, signs
the result and serves the artifact, so there is no inter-vendor handoff to attack.

That is worth stating plainly: **auth is not a feature lake adds to look complete. It
is the thing that makes the security story true.**

## The three principal types

| Principal | Examples | Authenticated by |
|---|---|---|
| **People** | Developers, and everyone using the Drive surface | Password + MFA, passkeys/WebAuthn, OAuth social, enterprise SSO |
| **Workloads** | Build workers, deployed applications, agents | Short-lived workload identity, never a long-lived key |
| **Organisations** | Teams, projects, customers | Membership and role, not a credential of their own |

Long-lived static tokens are the failure mode to avoid — they leak, they are
committed, they never expire, and they cannot be attributed. lake issues short-lived
credentials by default and treats a static token as an explicit, audited exception.

## What lake provides for its own users

Standard, and it has to be genuinely good or nobody reaches the interesting parts:

- Email/password with MFA, **passkeys and WebAuthn**, OAuth social providers.
- Enterprise **SSO — SAML and OIDC** — plus SCIM provisioning.
- Organisations, teams, roles, invitations, and delegated administration.
- Sessions, device management, and revocation that actually takes effect.
- Audit log of authentication and authorisation events.

## Auth as a service for applications on lake

Applications deployed to lake ([13](13-deployment-runtime.md)) get authentication for
*their* users, the way Clerk provides it — sign-in flows and components, user and
organisation management, JWT issuance and verification, webhooks on user lifecycle
events.

The argument for this is the same as the argument against buildpacks: an application
deployed on lake already has an identity, a deployment, secrets and a database. Making
it federate to a separate identity vendor for its own users is an integration tax with
no benefit.

**This is the largest scope question in the document**, and it is flagged as such
below — building a credible Clerk competitor is a substantial product on its own.

## Authorisation

Three permission surfaces that must not become three permission systems:

| Surface | Model |
|---|---|
| **Repositories** | Org/team/role, branch protection, path-scoped rules |
| **Drive** | Per-folder ACLs, share links with scope and role, external collaborators |
| **Platform** | Environment-scoped deploy rights, secret access, production gates |

They resolve against one policy engine over one object graph. A path in a repository
and a folder in the Drive are the same object with the same ACL, viewed differently.

**Checked at commit, not at open.** A device may hold cached plaintext after its
access is revoked; authorisation is re-evaluated when a write is published
([15](15-consumer-sync-and-web.md)).

**Designed against the documented confusions** — research on non-technical users shows
they rarely inspect effective permissions and cannot predict how moves and links affect
collaborators. At the moment of sharing, lake shows who can access, what they can do,
whether access is inherited, and whether the recipient gets a live object, a link or an
independent copy.

## Identity and secrets

Identity is the KEK custody layer ([11](11-secrets-and-encryption.md)). Envelope
encryption needs something to decide who may unwrap a data key, and that is an
authorisation decision.

- Repository and Drive subtree keys wrap to lake identities.
- Deployed workloads mint secrets at deploy time via workload identity, rather than
  holding long-lived credentials.
- Hardware tokens (PIV, FIDO2 `hmac-secret`) and TPM sealing remain available for
  principals that need key material never to leave a device.

## Identity and signing

- **Changes** are signed by the authoring identity ([02](02-version-control.md)).
- **Actions** are attested by the executing worker's workload identity
  ([06](06-distributed-execution.md)).
- **Artifacts** are signed at publication ([09](09-artifact-service.md)).
- **Cache writes** are authenticated, because a writable shared cache is a
  fleet-wide code-execution primitive ([05](05-caching.md)).
- **Deployments** record which identity promoted what, to where.

One signing layer, one identity, five uses.

## Federation, not capture

lake must be usable as an identity provider **and** must accept external ones. An
organisation that already runs Okta, Entra or Google Workspace should federate rather
than migrate, and a liquid repository hosted elsewhere should still verify signatures
made against lake identities.

Trust is a graph, not a hierarchy: verification policy states *which issuers are
acceptable for which claims*, and lake is one issuer among several rather than the
only one that works.

## Open questions

- **How far to take auth-as-a-service.** Providing lake's own auth is necessary.
  Providing Clerk-equivalent auth for customer applications is a full product with its
  own roadmap, compliance surface and support burden. Whether that ships alongside the
  platform or after it is undecided, and it should be an explicit decision rather than
  a drift.
- **Compliance scope.** SOC 2, GDPR data residency, and — if lake holds regulated
  data for scientific or clinical users — considerably more. This constrains
  architecture and is cheaper to design for than to retrofit.
- **Offline authorisation.** liquid works fully offline; authorisation cannot always
  be re-checked. What a client may do with cached credentials and for how long needs
  specifying.
- **Revocation reality.** Revoking access prevents future reads; it does not recall
  what was already cloned or decrypted. The product must say so rather than implying
  otherwise.
- **Account recovery**, which is where consumer identity systems actually fail, and
  which interacts badly with end-to-end encryption
  ([11](11-secrets-and-encryption.md)): if lake cannot read the data, lake cannot
  restore access to it.
