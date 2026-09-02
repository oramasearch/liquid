# Identity, attestation and transparency

> Standard: [TAAP/1](../standards/taap-1-spec-draft.md) ·
> Research: [26-transparency-logs-and-anchoring](../research/26-transparency-logs-and-anchoring/) ·
> [13-supply-chain-security](../research/13-supply-chain-security/) ·
> [23-secrets-service-and-pki](../research/23-secrets-service-and-pki/) ·
> [25-multiplayer-and-agent-collaboration](../research/25-multiplayer-and-agent-collaboration/)
>
> **Every drip is signed by an identified party — human or agent. On by default.**

## TAAP/1 is liquid's identity layer

Every actor that writes to a liquid repository — a person, an agent, a build worker, a
CI job — is a **TAAP identity**: a key-derived identifier of the form

```
taap:1:ed25519:JBSWY3DPEHPK3PXPJBSWY3DPEHPK3PXP...
```

where the fingerprint is `SHA-512/256` over the canonical public key object, base32
encoded.

Four properties of TAAP make it the right fit, and each maps to something liquid already
needs:

| TAAP property | Why liquid needs it |
|---|---|
| **Self-certifying identifiers** | An attacker cannot substitute their key for a known identifier — the identifier would change. Attribution survives without a naming authority. |
| **Transport-agnostic** | liquid works offline ([axiom 8](README.md)). An identity system that requires an HTTP round trip to a browser cannot sign a drip on a plane. TAAP assumes the transport provides *nothing*. |
| **Attestations by third parties** | "Authority A asserts X about Subject S" — signed by A, verifiable by anyone. This is exactly what liquid needs to say *which agent, which model, which authority* ([24](24-multiplayer-and-agents.md)). |
| **Delegation with scopes and revocation** | An agent acts on a human's behalf with **less** authority than the human holds ([19](19-identity.md)). |

Note what TAAP already gets right that liquid would otherwise have to invent:
**domain-separated signatures** (`TAAP/1 <type>` + `0x00` prefix, so a signature over one
object type cannot be replayed as another), **no floating point anywhere** (because
float serialization is not deterministic and therefore breaks canonicalization),
**algorithm agility with verifier-authoritative accept-lists** (the fix for the
`alg: none` class of JWT bugs), and **truncation forbidden as identity** (because
attackers grind keys to match the first and last characters users actually compare).

## Signed drips

A drip ([22](22-change-capture.md)) carries a TAAP signature over its canonical form.

```
drip {
  parent_drip_hash        ← the chain
  actor                   ← taap:1:ed25519:...
  content_hashes          ← the chunks written
  created_at
  signature               ← TAAP signature object
}
```

The `signature_purpose` field states intent inside the signed material, so a signature
made to authenticate cannot be repurposed as a signature of authorship.

**On by default.** Signing is not a mode you enable; an unsigned drip is the exception,
and liquid records that it was unsigned rather than silently accepting it.

**For agents, the signature carries the delegation chain**: which agent, running which
model, dispatched by which principal, under which authority — as TAAP attestations
rather than as a commit trailer someone can disable
([24](24-multiplayer-and-agents.md)).

## What the hash chain actually gives you — precisely

Drips form a hash chain, every link is signed, and the whole thing is published to a
Merkle transparency log ([09](09-artifact-service.md)). Periodically, the log's root is
**anchored** to a public chain.

This is worth naming accurately, because the accurate name is *stronger* positioning
than the loose one.

**What you get:**

| Property | Mechanism |
|---|---|
| **Append-only, tamper-evident history** | Hash chain plus Merkle log. Any rewrite is detectable by anyone holding an earlier root. |
| **Attributable authorship** | Every drip signed by an identified party. |
| **Inclusion proofs** | "This drip is in the log" — verifiable without the whole log. |
| **Consistency proofs** | "This log is an append-only extension of the one I saw before." |
| **Independent timestamping** | Anchoring proves *existence before time T* without trusting the log operator. |

**What you do not get, and should not claim:**

- **No consensus.** There is no distributed agreement about which history is canonical.
  A blockchain's defining feature is a consensus mechanism among mutually distrusting
  writers; liquid has an operator and a log.
- **No Byzantine fault tolerance** among untrusting parties.
- **Tamper-*evident*, not tamper-*proof*.** History can still be rewritten. What the log
  guarantees is that rewriting is *detectable*, not impossible. Rekor's own documentation
  says exactly this about itself.
- **No protection from split view — by default.** This is the non-obvious one. A log can
  serve each victim a *different, internally consistent* history: valid signatures, valid
  inclusion proofs, valid consistency proofs, different content. RFC 9162 states that an
  equivocating log circumvents ordinary auditing. **Publishing to a Merkle log is not, by
  itself, a security property.** What closes it is below.

**So the honest description is: an append-only, signed, cryptographically timestamped
transparency log with optional public anchoring.** That is precisely what Certificate
Transparency and the Go checksum database are — and in an enterprise security
conversation those are *more* credible references than "blockchain," not less. Pitching
it as a blockchain invites "what is your consensus mechanism?", and the answer is that
it does not need one.

You get every property Steve asked for. The accurate name is the better one.

## Witnessing is the primary defence — not anchoring

The instinct is that anchoring to a chain is the strong control. **It is not.** Anchoring
only detects inconsistency *at anchor granularity* — a daily anchor leaves up to a day of
operator-controlled ordering and equivocation before the next root.

What actually closes split view is a **witness quorum**:

> liquid's log obtains **M-of-N cosignatures** from independent witnesses that have
> verified consistency, *before* returning a checkpoint. Clients reject any checkpoint
> lacking quorum.

Using the C2SP cosignature format. **Mandatory, not optional** — optional witnessing is
security theatre, and Rekor v2 shipping without an enabled witness network is the
cautionary example.

Security requires witness *diversity* and quorum *intersection*: at least one honest
witness shared between any two accepted quorums. Witnesses under one administration are
not witnesses.

### The log is tiled

Signed checkpoint plus immutable, CDN-cacheable tiles; proofs computed client-side. This
is where Certificate Transparency (Static CT API, Let's Encrypt's Sunlight), Rekor v2 and
the Go checksum database all converged.

The reason is **monitoring cost**, which is the real bottleneck: verifying one inclusion
proof is logarithmic, but finding every unauthorised use of an identity means reading
every entry. A Rekor v1 identity scan was measured at ~50× slower than log growth; bulk
tiles improved it by about two orders of magnitude.

**A log nobody can afford to monitor provides no security.**

### Verify every proof, completely

Go's `sum.golang.org` — the best-deployed example of this architecture — shipped
**CVE-2026-56865** in August 2026: a malicious proxy could forge up to two tiles because
tiles were not verified against their parents. The architecture was sound; the verifier
was partial. Partial verification is a bypass.

## Anchoring

**Enabled by default, disableable.** An escalation on top of witnessing, for when the
required property is *operator-independent* durable proof.

Periodically — hourly by default — liquid commits the log's current Merkle root to a
public chain via OpenTimestamps-style aggregation, and retains the proof. One transaction
timestamps every drip since the last anchor, so cost amortises across the batch.

| Configuration | Behaviour |
|---|---|
| **Air-gapped** | Signing and hash chaining only, no network. TAAP's transport-agnostic design makes this a supported configuration rather than a degraded one. |
| **Witnessed** (default) | Published to lake's tiled log with a mandatory witness quorum. Closes split view. |
| **Anchored** | Merkle roots additionally committed to a public chain on a schedule. Survives compromise or disappearance of lake itself. |

State the trade honestly: anchoring proves *existence before a confirmed block* without
trusting lake or its witnesses. It costs confirmation latency (hours, not seconds), fees
that someone must fund continually, and a dependency on that chain remaining secure and
interpretable. And it proves existence only — **not authorship, correctness, ownership or
truth.**

For most repositories the witnessed log is sufficient. For regulated records, legal
evidence and long retention, anchoring is the difference between "our vendor says so" and
"anyone can check."

### Time is a separate problem

**The log does not provide authoritative time.** Rekor v2 explicitly dropped its v1
timestamp and tells signers to obtain an RFC 3161 timestamp separately. A block timestamp
is a consensus-constrained estimate, not precise UTC.

Where liquid needs trusted UTC — regulatory deadlines, legal ordering — that is an
**RFC 3161 TSA**, in addition to the log and any anchor.

## Reconciling TAAP identity with lake's PKI

These are **two different identity models** and the relationship needs to be explicit,
because getting it wrong produces a system with two incompatible notions of "who."

| | TAAP | lake's CA ([20](20-secrets-service-and-pki.md)) |
|---|---|---|
| Identity is | a long-lived public key | a short-lived certificate bound to an OIDC identity |
| Works offline | yes | no — requires an issuer |
| Naming authority | none | lake |
| Lifetime | years, with auditable rotation | minutes |

They compose rather than compete:

- **TAAP identity is the durable one.** It is what a drip is signed by, and what remains
  attributable decades later.
- **lake attests TAAP identities.** An attestation binding `taap:1:ed25519:...` to an org
  member, a role, or an agent's authority — issued by lake, verifiable by anyone, and
  revocable.
- **Short-lived certificates are issued *to* a TAAP key** for online operations where
  ephemerality is the point: publishing artifacts, mTLS between services, build workers.
- **Offline signing uses the TAAP key directly**, with lake's attestation verified
  whenever the verifier next has connectivity.

So: TAAP answers *who*, durably and offline. lake's PKI answers *what are they currently
authorised to do*, freshly and revocably.

## TAAP open questions liquid answers

liquid is a concrete driver for several of the spec's [open questions](../standards/taap-1-spec-draft.md).
Feedback to the protocol owner:

| Question | liquid's answer |
|---|---|
| **22.3 — non-human subjects** | **Required.** Agents, build workers and build artifacts must all be Subjects. This is not an edge case for liquid; it is the main case. |
| **22.5 — delegation chains** | **Required, attenuation-only with depth limits.** human → agent → subagent is the normal topology, and an agent must never hold more than its dispatcher. The confused-deputy risk is real and the depth limit is the mitigation. |
| **22.8 — ledger dependency** | **Optional, and liquid supplies one.** lake's transparency log is exactly the log TAAP treats as optional. The protocol should keep working with purely local trust stores — liquid needs the air-gapped configuration above. |
| **22.2 — identifier stability across rotation** | **Prefer stability** (the current draft's choice). A drip signed in 2026 must remain attributable after a 2029 key rotation; breaking every stored reference is not acceptable in a version-control system. |
| **22.9 — bearer mode** | **liquid does not need it.** Every liquid client can hold a key. Removing bearer tokens simplifies the security story at no cost here. |
| **22.1 — primary environment** | For liquid: enterprise services plus offline/air-gapped. `cjson` should stay non-optional — auditability of the signed material matters more than density. |
| **22.6 — recovery** | Matters commercially for lake, and is the most common real-world failure of key-based identity. It also interacts badly with end-to-end encryption ([11](11-secrets-and-encryption.md)): if lake cannot read the data, lake cannot restore access to it. |
| **22.10 — naming** | Worth resolving before liquid ships anything depending on the `taap:` scheme. |

## Open questions

- **Signing cost on the drip path.** Every save produces a signature. Ed25519 is fast,
  but the [performance budget](22-change-capture.md) is single-digit milliseconds and
  signing must not blow it. Batch signing of coalesced drips is the likely answer, and it
  is unspecified.
- **Key custody for agents.** An agent needs a key it can sign with, which is a secret
  it holds. Short-lived keys minted per session ([20](20-secrets-service-and-pki.md)),
  attested to a durable agent identity, is the probable shape.
- **Which chain to anchor to**, and whether to support several. Bitcoin has the strongest
  independence argument and the worst latency and cost.
- **Anchoring cadence** and whether it should vary by repository policy.
- **Long-term validity is a process, not an artifact**, and needs specifying. Neither a
  one-time anchor nor a one-time RFC 3161 token stays valid for decades. It requires
  timestamping the *signature value* rather than the document, retaining contemporaneous
  revocation evidence, renewing archive timestamps before the old protection weakens, and
  rehashing all prior evidence before the hash algorithm breaks. RFC 4998 Evidence Record
  Syntax is the relevant standard. This matters because liquid's history is meant to
  outlive the keys that signed it.
- **TAAP's own maturity.** It is a draft with unresolved questions, and liquid would be
  its first significant implementation. That is an opportunity and a risk, and the risk
  is that the protocol changes under liquid.
