# Transparency logs and blockchain anchoring

> Web-grounded research gathered 2026-09-02 via `perplexity_research`. The full source
> response is summarised here rather than vendored — the material is dense and the
> curated form below carries the citations that matter.
>
> Context for **[25-identity-attestation-and-transparency](../../features/25-identity-attestation-and-transparency.md)**.

## The executive finding

Three different things get conflated. They are not the same:

| | What it gives |
|---|---|
| **Merkle transparency log** | Compact evidence that an item is in a particular *signed log state*, and that one signed state extends another. **Tamper-evident, not tamper-proof.** |
| **Public blockchain** | A replicated consensus mechanism selecting a canonical, globally ordered history among **mutually distrustful writers**. Tamper-*resistant* — economically hard to rewrite, not mathematically impossible. |
| **Periodic anchoring** | Evidence that survives compromise or disappearance of the log operator. |

And the strategic conclusion:

> For most online software, certificate and package ecosystems, **a well-operated
> transparency log with a mandatory witness quorum is faster, cheaper, more private and
> semantically easier to monitor** than blockchain anchoring.

## The attack that matters: split view

This is the single most important thing to understand, because it is not intuitive.

**A transparency log does not intrinsically prevent equivocation.** Each victim-specific
fork can have valid signatures, valid inclusion proofs, and internally valid consistency
proofs. RFC 9162 states directly that an equivocating log can circumvent ordinary
auditing and must therefore still be treated as trusted absent additional mechanisms.

Rekor's own monitoring documentation says the same: logs are **tamper-evident, not
tamper-proof**. The operator can erase storage, sign a new history, or serve forks —
detection requires *someone* to retain and compare old checkpoints.

So "we publish to a Merkle log" is not, by itself, a security property.

## What actually closes it

| Mechanism | Effect | Limitation |
|---|---|---|
| **Client gossip** | Clients exchange signed checkpoints and request consistency proofs between them. Conflicting same-size roots are evidence of equivocation. | Depends on information crossing the attacker's partition. Difficult privacy and deployment properties. |
| **Monitors / mirrors** | Download all entries, reconstruct roots, check successive consistency, inspect content. | Costs **O(new entries)** even though individual proofs are O(log n). A monitor cannot expose a fork it never sees. |
| **Asynchronous witnesses** | Independently follow the log and publish accepted checkpoints. | Clients must actually *enforce* witness statements. Optional witnessing becomes security theatre. |
| **Synchronous quorum witnessing** | The log obtains **M-of-N cosignatures** from witnesses that verified consistency *before* returning a checkpoint. Clients reject checkpoints lacking quorum. C2SP defines the cosignature format. | Needs witness diversity and quorum intersection — at least one honest witness shared between accepted quorums. Adds latency. |
| **Blockchain anchor** | Detects histories inconsistent with the anchor. | **Only at anchor granularity.** Does not monitor entries or stop forks *between* anchors. |

**Synchronous quorum witnessing is the primary defence.** Anchoring is a different,
slower, complementary control.

## The industry moved to tiled logs

Worth adopting rather than reinventing:

- **Static CT API** (C2SP) keeps RFC 6962-compatible submission and SCT cryptography but
  replaces dynamic proof endpoints with a mutable signed checkpoint plus **immutable,
  CDN-cacheable tiles**. A verifier downloads tiles and computes proofs locally. Hash
  tiles hold 256 hashes — a full SHA-256 tile is 8,192 bytes.
- Let's Encrypt introduced **Sunlight** in March 2024, drawing on **Go sumdb's** tiled
  publication model. Chrome began admitting static-CT logs in 2025; from Chrome 144,
  effective **15 April 2026**, certificates could satisfy Chrome CT policy using
  static-API SCTs alone. Let's Encrypt's RFC 6962 logs went read-only 30 November 2025
  and shut down 28 February 2026.
- **Rekor v2** moved to tiles (Trillian Tessera) for the same reason.

The transition changes **cost and delivery, not the trust theorem**. Clients still need a
remembered checkpoint, gossip, or witnesses.

## Two facts about Rekor that affect liquid directly

1. **Rekor v2 is explicitly not a timestamp authority.** It omits v1's Signed Entry
   Timestamp, and the spec says signers should obtain an **RFC 3161 timestamp
   separately**. Anyone assuming the log provides authoritative time is wrong.
2. **Status is more nuanced than "v2 is out."** V2 reached GA, but in June 2026 Sigstore
   said the public `sigstore.dev` service would continue using **Rekor v1 by default for
   the foreseeable future**. And the v2 launch did **not** have witnessing enabled,
   pending a public witness network. The design should not be mistaken for a deployed
   guarantee.

## Monitoring is the expensive part

Cryptographic verification is cheap — one inclusion path is logarithmic. **Finding every
unauthorised use of your identity requires reading, parsing and indexing every relevant
entry.**

One documented deployment measured a Rekor v1 identity scan at roughly **50× slower than
log growth**. V2's 256-entry bulk tiles improved scanning by about two orders of
magnitude, and bounded shards prevent every new monitor from replaying one
indefinitely-growing tree.

That is the operational reason for tiles. Not a new cryptographic primitive.

## Go's checksum database — and its recent CVE

`sum.golang.org` is the best-deployed example because verification is **automatic in the
standard `go` command** rather than an optional audit tool. Each installation retains its
latest signed tree root and requires new roots to extend it; an inconsistency is a hard
build failure.

But in August 2026, Go fixed **GO-2026-6179 / CVE-2026-56865**: a malicious `GOPROXY`
could forge up to two tiles and persist module content not detectable by evaluating the
log. A related issue allowed a malicious `GOSUMDB` to return content not actually in the
log. Fixed in Go 1.25.13, 1.26.6, and `x/mod` 0.40.0.

The architecture is sound; the lesson is that **"clients verify every proof correctly" is
a critical and fallible implementation assumption.**

## Anchoring, concretely

**OpenTimestamps** is the established mechanism. The client hashes the file and submits a
commitment to calendar servers; calendars aggregate many submissions into a Merkle tree
and periodically place the root in a Bitcoin transaction (conventionally `OP_RETURN`).
The initial `.ots` proof links the digest to a pending commitment; after mining,
`ots upgrade` obtains the path through the calendar tree and block, and `ots verify`
recomputes it.

Calendars can censor, delay or disappear **before** upgrade — but once the complete proof
is held, they need not be trusted at all.

What a verified anchor establishes: `H(D)` was committed under root `R`; `R` was committed
in transaction `tx`; `tx` was in block `B`; therefore the commitment existed **no later
than that block's time interval**. It is proof of *existence*, not of authorship,
correctness, ownership or truth.

**Costs and residual dependencies:**

| Cost | Consequence |
|---|---|
| Confirmation latency | A calendar receipt is not yet a Bitcoin timestamp. Hours, depending on batching and fees. |
| Fees | Batching amortises one transaction across many records, but someone must fund and fee-manage anchors continually. |
| Chain dependency | Long-term verification needs preserved headers and a chain that remains secure and interpretable. |
| **Anchor interval** | A daily anchor leaves **up to a day of operator-controlled ordering and equivocation** before the next root. |
| Approximate time | "Before this confirmed block," not "at 12:00:00 UTC." |
| Proof retention | A root alone is useless without the artifact, Merkle path, transaction reference and chain data. |
| No content monitoring | Anchoring freezes a commitment. It says nothing about whether the entry was authorised. |

## Long-term validity is a process, not an artifact

Neither a one-time anchor nor a one-time RFC 3161 token is "valid for decades":

- **Timestamp the signature value, not the document** — that is what proves the signature
  existed while the certificate was valid.
- **Preserve contemporaneous revocation evidence.** Fetching today's OCSP decades later
  proves nothing about then.
- **Renew before expiry.** Add a new archive timestamp covering the old one *and* its
  validation evidence, before the old protection weakens.
- **Rehash before the hash breaks** — with all prior evidence, starting a new chain.
- **RFC 4998 Evidence Record Syntax** uses reduced Merkle trees so one TSA timestamp
  protects many objects, each keeping a compact path. It recommends redundant records
  under different hash algorithms and different TSAs.

## The honest criticism

> The strongest criticism of "blockchain for timestamping" is not that anchoring fails;
> it is that projects often use an expensive consensus system to solve only a
> **single-writer integrity problem.**

Also valid: "immutable" is overstated; the chain authenticates only a hash, not the
submitter or the truth of the data; block timestamps are not precise trusted UTC; a
proprietary chain controlled by the same organisation as the log adds nothing over a
signed database; and public deterministic hashes can act as a **confirmation oracle** for
guessed documents — salted commitments are preferable.

## Implications for liquid

1. **Witnessing is the default, not anchoring.** liquid's transparency log
   ([09](../../features/09-artifact-service.md)) must require an **M-of-N witness
   quorum cosignature before a checkpoint is accepted**, using the C2SP cosignature
   format. Optional witnessing is security theatre, and the Rekor v2 launch shipping
   without it is the cautionary example.

2. **Adopt a tiled log.** Static-CT/sumdb style: signed checkpoint plus immutable
   CDN-cacheable tiles, proofs computed client-side. It is where CT, Rekor and Go all
   converged, and it makes monitoring affordable — which is the actual bottleneck.

3. **Anchoring is an escalation, correctly scoped.** Enable it where the required
   property is *operator-independent, durable proof of existence-before-T* — regulated
   records, legal evidence, long retention. State the anchor interval honestly: it
   bounds the window of possible operator equivocation.

4. **Do not claim the log provides authoritative time.** Rekor v2 explicitly does not.
   If liquid needs trusted UTC, that is **RFC 3161**, separately.

5. **Budget for monitoring, and make it cheap.** A log nobody monitors provides no
   security. Bounded shards plus bulk tiles are what make identity monitoring feasible.

6. **Verify every proof, correctly.** The Go CVE shows a partial verifier is a bypass.
   Tile-parent verification is not optional.

7. **Plan long-term validity as a process.** Timestamp signatures, retain revocation
   evidence, renew before expiry, migrate hashes ahead of breakage. Relevant because
   liquid's history is meant to outlive the keys that signed it.

8. **Positioning: do not say "blockchain."** liquid is a single-writer integrity problem
   with an operator — precisely the case the criticism targets. "Append-only signed
   transparency log with witness quorum and optional public anchoring" is both accurate
   and, in front of a security-literate buyer, stronger.
