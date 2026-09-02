# TAAP/1: Transport-Agnostic Authorization and Attribution Protocol

**Document status:** Draft (Request for Comments)
**Version:** 1.0.0-draft.1
**Date:** 2026-08-05
**Category:** Standards Track (proposed)
**Editors:** (fill in)

---

## Abstract

TAAP/1 defines a message-based protocol for (a) authenticating parties identified by
long-lived public keys, (b) issuing and verifying **signed attribute attestations**
(for example, "University U asserts that Alice holds a BS in Computer Science"), and
(c) granting, using, and revoking **delegated access** to protected resources on behalf
of a subject.

TAAP/1 does **not** depend on HTTP, URLs, browsers, redirects, or any specific transport.
It is defined purely as a set of signed messages with a canonical encoding, and may be
carried over TCP, UDP, TLS streams, Unix sockets, Bluetooth, MQTT, NFC, QR codes,
SMTP/IMAP email, or physical media.

---

## Status of This Document

This is a working draft intended for review and iteration. Sections marked
**[OPEN QUESTION]** require input from the protocol owner before the draft can be
considered complete. Nothing in this document is final.

---

## Table of Contents

1. Introduction
2. Terminology and Conventions
3. Design Goals and Non-Goals
4. Architecture and Roles
5. Identity Model
6. Canonical Encoding and Signing
7. Message Envelope
8. Core Message Types
9. Authentication (Challenge–Response)
10. Attribute Attestations (Credentials)
11. Delegated Access and Access Grants
12. Access Tokens
13. Resource Access
14. Revocation
15. Discovery and Key Resolution
16. Transport Bindings
17. Error Handling
18. Security Considerations
19. Privacy Considerations
20. Design Rationale (Consolidated)
21. Comparison to OAuth 2.x / OIDC / W3C VC
22. Open Questions for the Protocol Owner
23. IANA-Style Registries (Local Registries)
24. Appendix A: Worked Example
25. Appendix B: Reserved Field Name Index

---

## 1. Introduction

### 1.1. Problem Statement

Existing delegated-authorization technology is overwhelmingly HTTP-shaped. OAuth 2.x is
explicitly scoped to HTTP: its endpoints are URLs, its flows use redirects and status
codes, and OAuth 2.1 states that use over non-HTTP protocols is out of scope. This makes
it awkward or impossible to use directly in:

- constrained or offline environments (IoT, industrial buses, air-gapped systems),
- peer-to-peer systems with no stable server endpoints,
- store-and-forward channels (email, message queues, sneakernet),
- human-mediated channels (QR codes, printed credentials, voice-read codes).

Separately, OAuth has no first-class notion of **attributed attributes**: statements
signed by an authority about a subject. OpenID Connect adds identity claims, but those
claims are asserted by the same party that issues tokens, not by arbitrary third-party
authorities such as universities, licensing boards, or employers.

TAAP/1 addresses both gaps at once.

### 1.2. What TAAP/1 Provides

1. **Key-based identity.** Every party is a public key (plus optional metadata).
2. **Authentication.** A signed challenge–response proof of key possession.
3. **Attestation.** Signed, independently verifiable statements about a subject
   ("attributed attributes").
4. **Delegation.** A subject can authorize a client to act on their behalf, with scopes.
5. **Tokens.** Short-lived, verifiable capabilities usable over any transport.
6. **Revocation.** Every issued artifact — token, grant, and attestation — is revocable.

### 1.3. Design Philosophy

TAAP/1 assumes that the transport provides *nothing*: no confidentiality, no integrity,
no ordering, no reliability, no session state, no clock synchronization guarantees. Every
security property is therefore established **inside** the messages. This is the single
most important design decision in this document; most other choices follow from it.

---

## 2. Terminology and Conventions

### 2.1. Requirement Levels

The key words **MUST**, **MUST NOT**, **REQUIRED**, **SHALL**, **SHALL NOT**, **SHOULD**,
**SHOULD NOT**, **RECOMMENDED**, **MAY**, and **OPTIONAL** are to be interpreted as in
BCP 14 (RFC 2119 / RFC 8174) when, and only when, they appear in all capitals.

### 2.2. Roles

| Role | Symbol | Description |
|---|---|---|
| Subject | S | The party an attestation is about, or on whose behalf access is delegated. Usually a human or an organization. |
| Client | C | Software requesting access on behalf of a Subject. Analogous to an OAuth client. |
| Authority | A | An entity that issues attribute attestations (e.g. a university). |
| Grant Authority | G | An entity that issues access tokens after Subject consent. Analogous to an OAuth authorization server. |
| Resource Holder | R | An entity that holds protected resources and enforces access. Analogous to an OAuth resource server. |
| Verifier | V | Any party that validates a proof, attestation, or token. R and G are both Verifiers. |

**Rationale (roles):** OAuth conflates "the thing that authenticates you" and "the thing
that says facts about you." TAAP/1 separates **Authority** (asserts attributes) from
**Grant Authority** (issues access). This lets a resource decide "only issue access to
someone with a degree from Authority X" without either party trusting the other's
infrastructure.

### 2.3. Other Terms

- **Artifact** — any signed TAAP/1 object: an attestation, an access grant, or a token.
- **Fingerprint** — a hash of a public key, used as a stable identifier.
- **Canonical form** — the deterministic byte serialization used for signing (Section 6).

---

## 3. Design Goals and Non-Goals

### 3.1. Goals

**G1. Transport independence.** The protocol MUST be usable over any bidirectional or
even unidirectional channel.
*Reasoning:* This is the founding requirement. It forces self-contained, self-authenticating
messages, which turns out to also improve auditability and offline verification.

**G2. Explicit, readable field names.** No abbreviated field names.
*Reasoning:* JWT's `iss`/`sub`/`aud`/`exp`/`jti` were chosen when tokens had to fit in URL
query strings and HTTP headers. TAAP/1 has no such constraint, and abbreviations are a
recurring source of implementation bugs and misreadings during audits. Bytes are cheap;
misparsed security-critical fields are not.

**G3. Full, uncompressed key material.** Public keys are represented in full, and
human-facing identifiers are never silently truncated.
*Reasoning:* Truncated fingerprints in UIs have repeatedly enabled impersonation attacks.
Compressed elliptic-curve points are cryptographically fine but require curve arithmetic
to decompress and invite implementation divergence. TAAP/1 favors explicitness over
compactness.

**G4. Universal revocability.** Every artifact MUST have a stable identifier and a defined
revocation path.
*Reasoning:* Anything issued must be withdrawable. A degree can be rescinded; a token can
leak; a grant can be regretted.

**G5. Offline verifiability where possible.** A Verifier SHOULD be able to validate an
artifact's signature and structure without contacting any other party.
*Reasoning:* Required by G1. Revocation checking is the one place where freshness may
require contact; Section 14 addresses this with explicit freshness policy.

**G6. Selective disclosure.** A Subject SHOULD be able to present a subset of attested
attributes.
*Reasoning:* A bar that needs "over 21" should not receive a full birth date and address.

**G7. Auditability.** Any security decision MUST be reconstructible from the retained
messages alone.

### 3.2. Non-Goals

- **NG1.** TAAP/1 does not define a transport, discovery network, or storage system.
- **NG2.** TAAP/1 does not define a consent UI, only the messages around consent.
- **NG3.** TAAP/1 does not require, but does not preclude, a blockchain or distributed
  ledger for key and revocation distribution.
- **NG4.** TAAP/1 does not attempt wire compatibility with OAuth 2.x or JWT. Section 21
  discusses bridging.
- **NG5.** TAAP/1 does not define account recovery policy (see Open Questions).

---

## 4. Architecture and Roles

### 4.1. Layering

```
+--------------------------------------------------------------+
| Layer 4: Policy         (who may access what, under which     |
|                          attested attributes)                 |
+--------------------------------------------------------------+
| Layer 3: Artifacts      (attestations, access grants, tokens) |
+--------------------------------------------------------------+
| Layer 2: Messages       (envelope, signatures, replay defense)|
+--------------------------------------------------------------+
| Layer 1: Encoding       (canonical CBOR / canonical JSON)     |
+--------------------------------------------------------------+
| Layer 0: Transport      (TCP, BLE, MQTT, SMTP, QR, ...)       |
+--------------------------------------------------------------+
```

**Reasoning:** Strict layering means a transport binding (Section 16) can be written
without touching cryptography, and a new artifact type can be added without changing the
envelope. Layer 0 is deliberately outside the trust boundary.

### 4.2. Trust Relationships

- V trusts A to assert attributes within A's declared domain.
- V trusts G to have obtained genuine Subject consent.
- No party is required to trust the transport.
- No party is required to trust a global root. Trust is per-Verifier policy (Section 15.4).

**Reasoning:** A single global root of trust is both a political problem and a single point
of compromise. TAAP/1 pushes trust decisions to the Verifier, which is the only party that
bears the consequences of a bad decision.

---

## 5. Identity Model

### 5.1. Identifiers

A TAAP/1 identity is a **key-derived identifier** of the form:

```
taap:1:<algorithm>:<fingerprint_base32>
```

Example:

```
taap:1:ed25519:JBSWY3DPEHPK3PXPJBSWY3DPEHPK3PXPJBSWY3DPEHPK3PXPJBSW
```

Where:

- `algorithm` is a registered signature algorithm identifier (Section 23.1).
- `fingerprint_base32` is the unpadded uppercase RFC 4648 base32 encoding of
  `SHA-512/256(canonical_public_key_object)`.

**Reasoning (why key-derived):** Self-certifying identifiers eliminate a whole class of
attacks. If the identifier is derived from the key, an attacker cannot substitute their key
for a known identifier — the identifier would change. This is what lets TAAP/1 work offline
and without a naming authority.

**Reasoning (why base32, not base64):** Base32 is case-insensitive and avoids characters
that are ambiguous when read aloud, printed, or OCR'd. Since G1 admits QR codes and printed
media, transcription robustness matters more than density.

**Reasoning (why SHA-512/256 rather than SHA-256):** It has the same output length but
resists length-extension by construction and is faster on 64-bit hardware. This is a mild
preference, not a security-critical choice.

### 5.2. Identifiers MUST NOT Be Truncated

Implementations **MUST NOT** display or transmit a truncated identifier as if it were the
identifier. A user interface **MAY** show an abbreviated form for layout purposes only if:

1. the full value is reachable without leaving the current view, and
2. the abbreviated form is visually marked as abbreviated (for example with an ellipsis),
   and
3. no security decision is offered to the user based solely on the abbreviated form.

**Reasoning:** Users compare the first and last few characters of fingerprints. Attackers
grind keys to match those characters. Forbidding truncation-as-identity closes this.

### 5.3. Public Key Object

Keys are structured objects with named fields, never opaque blobs.

```json
{
  "key_algorithm": "ed25519",
  "key_usage": ["signing"],
  "public_key_bytes_base64": "3s1WQ0K2v...==",
  "created_at": "2026-01-04T09:00:00Z",
  "key_label": "alice-primary-2026"
}
```

For elliptic curves with distinct coordinates, the uncompressed form is used and the
coordinates are named explicitly:

```json
{
  "key_algorithm": "p256",
  "key_usage": ["signing"],
  "public_key_x_base64": "f83OJ3D2xF1...",
  "public_key_y_base64": "x_FEzRu9m36...",
  "point_encoding": "uncompressed",
  "created_at": "2026-01-04T09:00:00Z",
  "key_label": "kiosk-verifier-01"
}
```

For RSA:

```json
{
  "key_algorithm": "rsa-pss-sha512",
  "key_usage": ["signing"],
  "modulus_base64": "0MvQ...",
  "public_exponent_base64": "AQAB",
  "modulus_bit_length": 4096,
  "created_at": "2026-01-04T09:00:00Z",
  "key_label": "registrar-archival"
}
```

**Reasoning (named coordinates, uncompressed points):** Per G3. A compressed point saves
32 bytes and costs a square root computation plus a sign-bit convention that has historically
been implemented inconsistently. Naming `public_key_x_base64` and `public_key_y_base64`
makes the object self-describing and diffable in logs.

**Reasoning (`key_usage`):** Key separation. A key used for signing attestations MUST NOT be
the same key used for, say, transport encryption. Mixing usages has broken real protocols.

### 5.4. Identity Documents

An **identity document** binds one identifier to one or more keys and optional metadata.

```json
{
  "artifact_type": "identity_document",
  "protocol_version": "TAAP/1",
  "identifier": "taap:1:ed25519:JBSWY3DP...",
  "keys": [ { "...": "public key objects" } ],
  "display_name": "Alice Nakamura",
  "controller": "taap:1:ed25519:JBSWY3DP...",
  "previous_document_hash_base64": null,
  "created_at": "2026-01-04T09:00:00Z",
  "updated_at": "2026-01-04T09:00:00Z"
}
```

Identity documents are **self-signed** by a key they contain (Section 7).

**Reasoning (`previous_document_hash_base64`):** It forms a hash chain, so key rotation is
auditable and a Verifier holding an old document can detect that a newer one exists and
verify the lineage. Without this, rotation and compromise are indistinguishable.

### 5.5. Key Rotation

To rotate, the controller publishes a new identity document that:

1. references the previous document by hash,
2. is signed by **both** an old key and a new key (dual signature), unless the old key is
   declared compromised, and
3. sets `updated_at` to a strictly later time.

**Reasoning (dual signature):** It proves continuity of control. An attacker with only the
new key cannot forge lineage; an attacker with only the old key cannot silently redirect the
identity to a key they control without also signing with it, which is detectable and
attributable.

**Note:** The identifier is derived from a key, so rotation raises the question of whether
the identifier changes. TAAP/1 keeps the **original** identifier stable and treats the
document chain as the authority on the current key set. This is flagged in
**[OPEN QUESTION 22.2]** because it trades self-certification for stability.

---

## 6. Canonical Encoding and Signing

### 6.1. Encodings

TAAP/1 defines two interchangeable encodings:

| Encoding | Identifier | Use case |
|---|---|---|
| Canonical JSON | `cjson` | Human inspection, email, logs, debugging |
| Canonical CBOR | `ccbor` | Constrained links, binary transports, QR codes |

Both **MUST** produce identical semantic content. Signatures are computed over the
canonical form of whichever encoding is in use, and the encoding identifier is carried in
the envelope so a Verifier never has to guess.

**Reasoning (two encodings):** G1 spans both a printed QR code and an SMTP message. One
encoding cannot serve both well. Carrying the encoding identifier inside the signed envelope
prevents encoding-confusion attacks.

### 6.2. Canonical JSON Rules

1. Object keys sorted by Unicode code point, ascending.
2. No insignificant whitespace.
3. UTF-8 output, no BOM.
4. Strings use the shortest valid escape sequences; only characters requiring escape are
   escaped.
5. No floating-point numbers anywhere in TAAP/1 (Section 6.4).
6. Absent optional fields are omitted, never emitted as `null`, except where a `null` is
   explicitly meaningful (e.g. `previous_document_hash_base64`).

### 6.3. Canonical CBOR Rules

Canonical CBOR follows RFC 8949 Section 4.2.1 deterministic encoding: shortest-form
integers, definite-length items, and map keys sorted by their encoded bytes.

### 6.4. No Floating Point

Numeric values in TAAP/1 **MUST** be integers or strings. Timestamps are strings
(Section 6.5). Durations are integer seconds.

**Reasoning:** Floating-point serialization is not deterministic across languages and
runtimes, which breaks canonicalization and therefore breaks signatures. This has bitten
JSON-signing systems repeatedly.

### 6.5. Timestamps

All timestamps **MUST** be RFC 3339 UTC strings with second precision and a literal `Z`:
`2026-08-05T19:42:00Z`.

**Reasoning (strings, not epoch integers):** Readability during audits, per G2, and no
32-bit rollover or unit ambiguity. The cost is a slightly larger encoding and a parse step.

### 6.6. Signature Computation

Given an artifact or message body `B`:

1. Construct `B` **without** any `signatures` field.
2. Serialize `B` to the canonical form of the declared encoding.
3. Compute `signing_input = domain_separator || 0x00 || canonical_bytes(B)`.
4. Sign `signing_input` with the private key.

The `domain_separator` is the ASCII string:

```
TAAP/1 <artifact_type_or_message_type>
```

**Reasoning (domain separation):** Without it, a signature over one object type could be
replayed as a signature over a different object type that happens to serialize similarly.
Domain separation is cheap and eliminates cross-protocol and cross-type signature reuse.
The `0x00` byte guarantees the separator cannot be confused with the start of the payload.

### 6.7. Signature Object

```json
{
  "signature_algorithm": "ed25519",
  "signing_key_identifier": "taap:1:ed25519:JBSWY3DP...",
  "signing_key_label": "alice-primary-2026",
  "signature_purpose": "authentication_response",
  "signed_at": "2026-08-05T19:42:00Z",
  "signature_bytes_base64": "MEUCIQDf..."
}
```

**Reasoning (`signature_purpose`):** It states intent in the signed material itself, so a
signature made for authentication cannot be repurposed as a signature of consent. This is
the same motivation as the domain separator, applied at a finer granularity.

### 6.8. Algorithm Agility

Every signed object carries its algorithm. Verifiers **MUST** reject algorithms not in their
configured accept-list, and **MUST NOT** infer the algorithm from key length or from the
signature itself.

**Reasoning:** The `alg: none` class of JWT vulnerabilities came from letting the attacker
choose the verification algorithm. TAAP/1 requires the *Verifier's* policy to be
authoritative, with the message's declared algorithm used only to select among already
permitted options.

---

## 7. Message Envelope

Every TAAP/1 message on the wire is an envelope.

```json
{
  "protocol_version": "TAAP/1",
  "encoding": "cjson",
  "message_type": "authentication_challenge",
  "message_identifier": "01J9Z8QF7T4V2C6M0X3B5N7K9P",
  "conversation_identifier": "01J9Z8QF7T4V2C6M0X3B5N7K90",
  "sender_identifier": "taap:1:ed25519:VERIFIERKEY...",
  "recipient_identifier": "taap:1:ed25519:JBSWY3DP...",
  "created_at": "2026-08-05T19:42:00Z",
  "expires_at": "2026-08-05T19:47:00Z",
  "message_body": { "...": "type-specific" },
  "signatures": [ { "...": "signature objects" } ]
}
```

### 7.1. Field Semantics

| Field | Required | Meaning |
|---|---|---|
| `protocol_version` | yes | Exactly `TAAP/1` for this document. |
| `encoding` | yes | `cjson` or `ccbor`. Must match actual encoding. |
| `message_type` | yes | Registered type (Section 23.2). |
| `message_identifier` | yes | Globally unique, sortable, 128-bit. |
| `conversation_identifier` | no | Groups related messages; absent for one-shot messages. |
| `sender_identifier` | yes | TAAP identifier of the sender. |
| `recipient_identifier` | no | Omitted for broadcast or when the recipient is unknown. |
| `created_at` | yes | Sender's clock at creation. |
| `expires_at` | no | After this instant the message MUST be rejected. |
| `message_body` | yes | Type-specific payload. |
| `signatures` | yes | At least one signature over the envelope minus `signatures`. |

### 7.2. Why the Envelope Is Signed as a Whole

The signature covers `message_type`, `recipient_identifier`, `conversation_identifier`, and
`expires_at` as well as the body.

**Reasoning:** If only the body were signed, an attacker could re-label a message as a
different type, redirect it to a different recipient, splice it into a different
conversation, or extend its lifetime. Binding all routing and lifetime metadata into the
signature makes the message meaningful only in the context its author intended.

### 7.3. Message Identifiers

`message_identifier` **MUST** be 128 bits of uniqueness rendered as Crockford base32, and
**SHOULD** be lexicographically sortable by creation time (ULID-style: 48-bit timestamp
plus 80 bits of randomness).

**Reasoning:** Sortability makes replay caches and audit logs cheap to prune — a Verifier can
discard cache entries older than its clock-skew window by prefix comparison alone. Eighty
bits of randomness makes collision within a window negligible.

### 7.4. No Transport-Derived Trust

A Verifier **MUST NOT** grant any trust based on the channel a message arrived on: not the
source address, not TLS client authentication, not the sending email address, not the
Bluetooth pairing.

**Reasoning:** Direct consequence of G1. If channel properties could confer trust,
implementations would come to depend on them and the protocol would silently stop being
transport-agnostic. Channel security may still be used for confidentiality and DoS
resistance — it just cannot be used for authorization.

---

## 8. Core Message Types

| `message_type` | Direction | Purpose |
|---|---|---|
| `authentication_challenge` | V → S/C | Begin proof of key possession. |
| `authentication_response` | S/C → V | Signed proof. |
| `authentication_result` | V → S/C | Accept or reject. |
| `attestation_request` | S → A | Ask an Authority to attest attributes. |
| `attestation_issue` | A → S | Deliver a signed attestation. |
| `attestation_presentation` | S/C → V | Present attestation(s), possibly partial. |
| `attestation_status_query` | V → A | Ask whether an attestation is still valid. |
| `attestation_status_response` | A → V | Answer, signed. |
| `access_request` | C → G | Request delegated access. |
| `consent_request` | G → S | Ask the Subject to approve. |
| `consent_response` | S → G | Approve or deny, signed by the Subject. |
| `access_grant_issue` | G → C | Deliver grant plus first token. |
| `token_refresh_request` | C → G | Exchange a grant for a fresh token. |
| `token_issue` | G → C | Deliver a token. |
| `resource_request` | C → R | Use a token to act on a resource. |
| `resource_response` | R → C | Result. |
| `revocation_request` | any → G/A | Ask to revoke an artifact. |
| `revocation_result` | G/A → any | Confirm revocation. |
| `revocation_notice` | G/A → R | Push revocation to enforcement points. |
| `revocation_list_query` | V → G/A | Pull current revocation state. |
| `revocation_list_response` | G/A → V | Signed revocation list. |
| `error` | any → any | Structured failure (Section 17). |

**Reasoning (explicit type per step rather than a generic request/response):** Each step has
different security semantics and different replay rules. Naming them separately lets the
domain separator and `signature_purpose` do real work, and makes policy engines and audit
logs readable without reconstructing state.

---

## 9. Authentication (Challenge–Response)

### 9.1. Flow

```
V                                             S
|                                             |
|-- authentication_challenge ---------------->|
|                                             |
|<-- authentication_response ------------------|
|                                             |
|-- authentication_result ------------------->|
```

### 9.2. `authentication_challenge` Body

```json
{
  "challenge_nonce_base64": "9f3c1b7e5a2d4c8f0e6b3a9d7c5f1e8b",
  "challenge_expires_at": "2026-08-05T19:47:00Z",
  "verifier_identifier": "taap:1:ed25519:VERIFIERKEY...",
  "requested_subject_identifier": "taap:1:ed25519:JBSWY3DP...",
  "acceptable_signature_algorithms": ["ed25519", "p256"],
  "context_binding": {
    "purpose_description": "Sign in to the workshop door controller",
    "resource_holder_identifier": "taap:1:ed25519:DOORCTRL...",
    "channel_binding_base64": null
  }
}
```

### 9.3. `authentication_response` Body

```json
{
  "challenge_nonce_base64": "9f3c1b7e5a2d4c8f0e6b3a9d7c5f1e8b",
  "verifier_identifier": "taap:1:ed25519:VERIFIERKEY...",
  "subject_identifier": "taap:1:ed25519:JBSWY3DP...",
  "responder_created_at": "2026-08-05T19:42:03Z",
  "identity_document_hash_base64": "Qm9vZ2ll..."
}
```

The envelope signature over this body, with `signature_purpose` set to
`authentication_response`, is the proof.

### 9.4. Nonce Requirements

- `challenge_nonce_base64` **MUST** contain at least 128 bits from a CSPRNG.
- A Verifier **MUST** reject a response whose nonce it did not issue, has already accepted,
  or has expired.
- A Verifier **MUST** retain accepted nonces at least until `challenge_expires_at` plus its
  configured clock-skew allowance.

**Reasoning (Verifier-chosen nonce):** Only the Verifier can guarantee freshness for itself.
A Subject-chosen nonce would let a Subject replay an old signature. Requiring the nonce to
be echoed inside the signed body binds the proof to that specific challenge.

**Reasoning (`verifier_identifier` echoed in the response):** Prevents a relay attack where
a malicious Verifier forwards a challenge from a real Verifier and reuses the resulting
proof elsewhere. Because the Subject signs *whom they are proving to*, the proof is useless
to a third party.

### 9.5. `identity_document_hash_base64`

The responder states which version of its identity document it is asserting.

**Reasoning:** It lets the Verifier detect a stale cached document and fetch the newer one,
and it prevents a downgrade attack where a Verifier is tricked into validating against a
retired key.

### 9.6. Optional Channel Binding

`channel_binding_base64` **MAY** carry a hash of underlying channel material (for example a
TLS exporter value). If present in the challenge, the responder **MUST** echo it.

**Reasoning:** Where a secure channel does exist, binding to it prevents man-in-the-middle
relay of an otherwise valid proof. This is optional precisely because G1 forbids *requiring*
any channel property.

---

## 10. Attribute Attestations (Credentials)

### 10.1. Purpose

An attestation is a signed statement: *Authority A asserts that Subject S has attributes X
as of time T*. This is the "signed attribution" requirement.

### 10.2. Attestation Object

```json
{
  "artifact_type": "attestation",
  "protocol_version": "TAAP/1",
  "attestation_identifier": "01J9ZB4KQ7Y8X2M5N0P3R6T9V1",
  "authority_identifier": "taap:1:ed25519:UNIVERSITYKEY...",
  "authority_display_name": "Example State University, Registrar",
  "subject_identifier": "taap:1:ed25519:JBSWY3DP...",
  "attestation_profile": "academic_degree/1",
  "asserted_attributes": {
    "degree_level": "bachelor",
    "degree_title": "Bachelor of Science",
    "field_of_study": "Computer Science",
    "conferral_date": "2024-06-15",
    "institution_legal_name": "Example State University",
    "honors": "cum_laude"
  },
  "evidence_references": [
    {
      "evidence_type": "internal_record",
      "evidence_locator": "registrar/degree/2024/CS/44817",
      "evidence_digest_base64": "3q2+7w=="
    }
  ],
  "issued_at": "2024-07-01T10:00:00Z",
  "valid_from": "2024-07-01T10:00:00Z",
  "expires_at": null,
  "status_policy": {
    "status_check_recommended": true,
    "status_authority_identifier": "taap:1:ed25519:UNIVERSITYKEY...",
    "maximum_status_age_seconds": 2592000
  },
  "attribute_disclosure_commitments": {
    "commitment_algorithm": "sha512-256-salted",
    "commitments": {
      "degree_level": "8f1c...",
      "degree_title": "44ab...",
      "field_of_study": "9d20...",
      "conferral_date": "77e5...",
      "institution_legal_name": "12bc...",
      "honors": "aa03..."
    }
  },
  "signatures": [ { "...": "authority signature" } ]
}
```

### 10.3. Field Reasoning

**`attestation_profile`** — a versioned name for the attribute schema. Verifiers key their
policy off the profile, not off guessing which keys are present.
*Reasoning:* Without a profile identifier, every Verifier invents its own interpretation of
`field_of_study`, and adding a field becomes a breaking change.

**`asserted_attributes` as a nested object** — keeps Authority-asserted facts clearly
separated from protocol metadata.
*Reasoning:* A flat object invites collisions between a protocol field and an attribute name,
and an attacker who can influence attribute names could shadow protocol semantics.

**`evidence_references`** — points to the Authority's own basis for the claim, with a digest
but not necessarily the content.
*Reasoning:* Supports audit and dispute resolution without forcing the Authority to leak
underlying records. The digest lets the record be verified later if it is disclosed.

**`expires_at: null`** — permitted. A conferred degree does not expire.
*Reasoning:* Forcing expiry on inherently permanent facts leads implementers to set absurd
dates. Instead, permanence is expressed honestly and freshness is handled by
`status_policy`, which is where the real question ("has this been rescinded?") belongs.

**`status_policy`** — the Authority states how strongly it recommends a status check and how
stale a status answer may be.
*Reasoning:* Only the issuer knows the volatility of its own claims. A degree changes almost
never; a "currently licensed to practice" claim changes often. Encoding this in the artifact
lets a Verifier apply proportionate rigor instead of a single global rule.

**`attribute_disclosure_commitments`** — per-attribute salted hashes enabling selective
disclosure (Section 10.4).

### 10.4. Selective Disclosure

The Authority computes, for each attribute:

```
commitment = SHA-512/256( "TAAP/1 attribute" || 0x00
                        || attribute_name || 0x00
                        || attribute_value_canonical || 0x00
                        || per_attribute_salt )
```

The Authority delivers the salts to the Subject alongside the attestation. The Subject may
then present the signed attestation with `asserted_attributes` reduced to only the
attributes being disclosed, plus the salts for exactly those attributes.

A Verifier:

1. verifies the Authority signature over the attestation (commitments are inside the signed
   body, so they cannot be altered),
2. recomputes commitments for each disclosed attribute and compares,
3. treats undisclosed attributes as unknown, not absent.

**Reasoning (salted per-attribute hashes rather than a Merkle tree or zero-knowledge proofs):**
Salted hashes are implementable correctly by an average engineer in an afternoon, require no
pairing-friendly curves or trusted setup, and work in constrained environments. Salting is
mandatory because unsalted commitments over low-entropy values (`degree_level: "bachelor"`)
are trivially brute-forced.
**Limitation, stated plainly:** this scheme reveals *how many* attributes exist and their
*names*, and it cannot prove predicates ("over 21") without revealing the value. If predicate
proofs are required, a BBS+-style signature scheme is the correct upgrade path, at
significant complexity cost. Flagged as **[OPEN QUESTION 22.4]**.

### 10.5. Subject Consent to Attestation

An Authority **SHOULD NOT** issue an attestation about a Subject without either an
`attestation_request` signed by that Subject or a documented lawful basis recorded in
`evidence_references`.

**Reasoning:** Attestations are inherently about someone. Unsolicited signed claims about
third parties are a defamation and privacy hazard, and a protocol that makes them frictionless
will see them abused.

---

## 11. Delegated Access and Access Grants

### 11.1. Flow

```
C                     G                     S
|                     |                     |
|-- access_request -->|                     |
|                     |-- consent_request ->|
|                     |                     |
|                     |<- consent_response -|
|<- access_grant_issue|                     |
```

Before `consent_request`, G **MUST** authenticate S per Section 9.

### 11.2. `access_request` Body

```json
{
  "client_identifier": "taap:1:ed25519:CLIENTKEY...",
  "client_display_name": "Transcript Importer",
  "subject_identifier": "taap:1:ed25519:JBSWY3DP...",
  "resource_holder_identifier": "taap:1:ed25519:RESOURCEKEY...",
  "requested_scopes": [
    {
      "scope_name": "read_transcripts",
      "scope_description": "Read your academic transcripts",
      "scope_is_required": true
    },
    {
      "scope_name": "write_notes",
      "scope_description": "Add notes to your record",
      "scope_is_required": false
    }
  ],
  "requested_grant_lifetime_seconds": 2592000,
  "requested_token_lifetime_seconds": 900,
  "client_attestation_presentations": [],
  "request_nonce_base64": "b41d9f..."
}
```

**Reasoning (`scope_is_required`):** Lets a Subject grant a subset without the Client failing
outright, which reduces the incentive for Clients to demand everything. Consent quality
improves when partial consent is a first-class outcome.

**Reasoning (`scope_description` supplied by the Client but shown as Client-supplied):** G
**MUST NOT** present Client-supplied text as if it were G's own. Otherwise a Client can write
"Read nothing important" next to a dangerous scope. G **SHOULD** prefer its own registered
description for known scopes.

### 11.3. `consent_response` Body

```json
{
  "consent_decision": "approved_with_modifications",
  "access_request_message_identifier": "01J9ZB...",
  "request_nonce_base64": "b41d9f...",
  "client_identifier": "taap:1:ed25519:CLIENTKEY...",
  "resource_holder_identifier": "taap:1:ed25519:RESOURCEKEY...",
  "approved_scopes": ["read_transcripts"],
  "denied_scopes": ["write_notes"],
  "approved_grant_expires_at": "2026-09-04T19:42:00Z",
  "consent_recorded_at": "2026-08-05T19:43:11Z",
  "consent_presentation_digest_base64": "5c8a..."
}
```

`consent_decision` is one of `approved`, `approved_with_modifications`, or `denied`.

**Reasoning (Subject signs the consent, not just G recording it):** This is a deliberate
departure from OAuth, where consent is an internal database row at the authorization server
and the user has no cryptographic artifact proving what they agreed to. Here, the consent
response is a signed, non-repudiable record naming the exact Client, Resource Holder, scopes,
and expiry. It protects the Subject (G cannot fabricate consent) and it protects G (the
Subject cannot plausibly deny it).

**Reasoning (`consent_presentation_digest_base64`):** A hash of the exact text and scope list
shown to the Subject at decision time. This makes "what was I actually shown?" answerable
years later, which is the crux of most consent disputes.

### 11.4. Access Grant Object

```json
{
  "artifact_type": "access_grant",
  "protocol_version": "TAAP/1",
  "grant_identifier": "01J9ZBQ0M4T7X1C3N6P9R2V5Y8",
  "grant_authority_identifier": "taap:1:ed25519:GRANTKEY...",
  "subject_identifier": "taap:1:ed25519:JBSWY3DP...",
  "client_identifier": "taap:1:ed25519:CLIENTKEY...",
  "resource_holder_identifier": "taap:1:ed25519:RESOURCEKEY...",
  "approved_scopes": ["read_transcripts"],
  "consent_message_identifier": "01J9ZBH...",
  "consent_response_digest_base64": "7f3a...",
  "issued_at": "2026-08-05T19:43:12Z",
  "expires_at": "2026-09-04T19:42:00Z",
  "maximum_token_lifetime_seconds": 900,
  "grant_binding": {
    "binding_type": "client_key_proof_required",
    "bound_client_key_identifier": "taap:1:ed25519:CLIENTKEY..."
  },
  "status_policy": {
    "status_check_recommended": true,
    "status_authority_identifier": "taap:1:ed25519:GRANTKEY...",
    "maximum_status_age_seconds": 300
  },
  "signatures": [ { "...": "grant authority signature" } ]
}
```

The grant is the long-lived artifact — the analogue of an OAuth refresh token — but it is a
**structured, signed, inspectable object** rather than an opaque string.

**Reasoning (structured grant):** A Subject can be shown a plain-language list of everything
they have granted, derived directly from the artifacts, without querying a vendor's internal
tables. Transparency is a design goal, not a feature bolted on later.

**Reasoning (`consent_response_digest_base64` inside the grant):** Cryptographically chains
the grant to the specific consent that authorized it. A grant whose consent cannot be
produced is self-evidently suspect.

**Reasoning (`grant_binding` defaulting to key-proof):** Grants are sender-constrained. See
Section 12.3.

---

## 12. Access Tokens

### 12.1. Why Tokens Exist at All

A token is a **short-lived, narrowly scoped capability** derived from a grant. It exists so
that the Resource Holder can make an authorization decision cheaply and locally, without
contacting the Grant Authority on every request and without ever seeing the Subject's keys or
the full consent record.

**Reasoning:** This is exactly the property that makes OAuth useful, and it is worth keeping.
The separation also limits blast radius: a leaked token expires in minutes, whereas a leaked
grant is revocable and detectable.

### 12.2. Access Token Object

```json
{
  "artifact_type": "access_token",
  "protocol_version": "TAAP/1",
  "token_identifier": "01J9ZBQ9F2K5M8P1R4T7V0X3Z6",
  "grant_identifier": "01J9ZBQ0M4T7X1C3N6P9R2V5Y8",
  "grant_authority_identifier": "taap:1:ed25519:GRANTKEY...",
  "subject_identifier": "taap:1:ed25519:JBSWY3DP...",
  "client_identifier": "taap:1:ed25519:CLIENTKEY...",
  "intended_resource_holder_identifier": "taap:1:ed25519:RESOURCEKEY...",
  "authorized_scopes": ["read_transcripts"],
  "issued_at": "2026-08-05T19:43:12Z",
  "not_valid_before": "2026-08-05T19:43:12Z",
  "expires_at": "2026-08-05T19:58:12Z",
  "token_binding": {
    "binding_type": "client_key_proof_required",
    "bound_client_key_identifier": "taap:1:ed25519:CLIENTKEY..."
  },
  "attested_attribute_summary": [
    {
      "attestation_identifier": "01J9ZB4KQ7Y8X2M5N0P3R6T9V1",
      "authority_identifier": "taap:1:ed25519:UNIVERSITYKEY...",
      "attestation_profile": "academic_degree/1",
      "disclosed_attribute_names": ["degree_level", "field_of_study"]
    }
  ],
  "single_use_required": false,
  "signatures": [ { "...": "grant authority signature" } ]
}
```

Note the field names: `intended_resource_holder_identifier` rather than `aud`,
`expires_at` rather than `exp`, `token_identifier` rather than `jti`,
`subject_identifier` rather than `sub`, `grant_authority_identifier` rather than `iss`.

**Reasoning (long names):** Per G2. The compactness that motivated `iss`/`sub`/`aud`/`exp`
was an artifact of shipping tokens in URLs and HTTP headers. TAAP/1 has no such constraint,
and every security review of a token is easier when the fields say what they mean.
`ccbor` recovers most of the size difference for constrained links anyway.

**Reasoning (`intended_resource_holder_identifier` singular and mandatory):** Multi-audience
tokens are a persistent source of confused-deputy bugs, where a token accepted by one service
is replayed against another. One token, one intended recipient.

**Reasoning (`attested_attribute_summary`):** Records which attributes the authorization
decision relied on. The Resource Holder can enforce attribute-conditioned policy without
re-verifying the full attestation chain, and an auditor can see *why* access was granted.

### 12.3. Sender-Constrained by Default

`binding_type` is `client_key_proof_required` unless explicitly configured otherwise. A
Client using such a token **MUST** accompany it with a fresh signature over the resource
request (Section 13).

Bearer tokens (`binding_type: "bearer"`) are **NOT RECOMMENDED** and, if supported, **MUST**
have a lifetime of at most 300 seconds.

**Reasoning:** This is the single biggest security improvement over classic OAuth. A bearer
token is a password: whoever holds it wins. Because TAAP/1 already requires every party to
have a signing key, requiring proof-of-possession costs almost nothing and eliminates the
entire class of attacks based on stolen tokens in logs, proxies, referrers, and error
reports. Bearer mode exists only as a grudging concession for environments where the Client
genuinely cannot sign per-request.

### 12.4. Short Lifetimes

`expires_at` **SHOULD** be within 900 seconds of `issued_at` and **MUST** be within
`maximum_token_lifetime_seconds` of the referenced grant.

**Reasoning:** Short lifetimes are the practical answer to revocation latency for
self-contained artifacts. They bound the damage from a leak and reduce how aggressively
Resource Holders must poll revocation state.

### 12.5. Token Refresh

`token_refresh_request` presents the grant plus a fresh proof-of-possession by the bound
Client key. G verifies the grant is unexpired and unrevoked, then issues a new token.

G **SHOULD** treat an unusual refresh pattern — sudden geographic or transport change, or
refresh of a grant already reported compromised — as grounds for requiring re-consent.

---

## 13. Resource Access

### 13.1. `resource_request` Body

```json
{
  "access_token": { "...": "full access token object" },
  "requested_operation": "read_transcripts",
  "operation_parameters": {
    "academic_year": "2024"
  },
  "request_nonce_base64": "cc71a2...",
  "client_created_at": "2026-08-05T19:44:02Z"
}
```

The envelope signature by the bound Client key, with `signature_purpose` set to
`resource_request`, is the proof-of-possession.

### 13.2. Verifier Obligations

A Resource Holder **MUST**, in order:

1. Verify the envelope signature and confirm the signing key matches
   `token_binding.bound_client_key_identifier`.
2. Verify `message_identifier` has not been seen (replay cache).
3. Verify the token signature against a trusted Grant Authority key.
4. Verify `intended_resource_holder_identifier` equals its own identifier.
5. Verify `not_valid_before` ≤ now ≤ `expires_at`, within clock-skew allowance.
6. Verify `requested_operation` is covered by `authorized_scopes`.
7. Apply revocation freshness policy (Section 14.5).
8. Apply local policy, including any attribute conditions.

Failure at any step **MUST** produce an `error` message and **MUST NOT** partially perform
the operation.

**Reasoning (ordered, mandatory checklist):** Under-specified verification is where
authorization protocols actually fail in the field. Ordering matters: cheap cryptographic and
replay checks precede expensive network-dependent ones, which limits DoS amplification.

### 13.3. Clock Skew

Verifiers **MUST** have a configured `clock_skew_allowance_seconds`, default 60, applied
symmetrically. Verifiers **MUST NOT** accept artifacts more than that far in the future.

**Reasoning:** G1 means TAAP/1 cannot assume NTP. Making skew an explicit, bounded,
configured parameter is better than every implementation inventing its own tolerance.

---

## 14. Revocation

Requirement G4: everything is revocable. TAAP/1 defines one revocation model applied
uniformly to tokens, grants, and attestations.

### 14.1. `revocation_request` Body

```json
{
  "revocation_target_type": "access_grant",
  "revocation_target_identifier": "01J9ZBQ0M4T7X1C3N6P9R2V5Y8",
  "revocation_cascade": "revoke_derived_artifacts",
  "revocation_reason_code": "subject_withdrew_consent",
  "revocation_reason_description": "Removed in account settings",
  "requested_effective_at": "2026-08-05T20:00:00Z",
  "requester_role": "subject"
}
```

`revocation_target_type` is one of `access_token`, `access_grant`, `attestation`, or
`identity_key`.

### 14.2. Who May Revoke

| Target | Permitted requesters |
|---|---|
| `access_token` | Subject, Client, Grant Authority |
| `access_grant` | Subject, Client, Grant Authority |
| `attestation` | Issuing Authority; Subject MAY request suppression |
| `identity_key` | Controller of the identity document |

**Reasoning (Subject can always revoke a grant):** The Subject is the party whose resources
are at stake. Requiring them to ask the Client to relinquish access — a Client that may be
hostile or defunct — would be absurd. This is a known weak point in real-world OAuth
deployments.

**Reasoning (only the Authority can revoke an attestation, but the Subject can request
suppression):** An Authority's statement is the Authority's own speech; a Subject cannot
unilaterally unsay it. But a Subject can stop presenting it and can ask the Authority to mark
it withdrawn, which matters when the underlying fact is contested or was issued in error.

### 14.3. Cascade Semantics

`revocation_cascade` is one of:

- `target_only` — revoke exactly the named artifact.
- `revoke_derived_artifacts` — also revoke everything derived from it (tokens from a grant).
- `revoke_all_for_client` — revoke every grant and token for that Client and Subject pair.
- `revoke_all_for_subject` — revoke everything issued for that Subject.

Revoking a grant **MUST** default to `revoke_derived_artifacts`.

**Reasoning:** "I revoked the app's access but it kept working for fifteen minutes" is a
failure of user expectation, not a subtlety. Cascading by default aligns the mechanism with
what the person clicking the button means. `revoke_all_for_subject` exists for the device-lost
and account-compromised cases, where speed matters more than precision.

### 14.4. Revocation List Object

```json
{
  "artifact_type": "revocation_list",
  "protocol_version": "TAAP/1",
  "revocation_list_identifier": "01J9ZC1V...",
  "issuing_authority_identifier": "taap:1:ed25519:GRANTKEY...",
  "list_sequence_number": 4417,
  "generated_at": "2026-08-05T19:45:00Z",
  "next_update_expected_at": "2026-08-05T19:50:00Z",
  "covers_artifact_types": ["access_token", "access_grant"],
  "covers_issued_from": "2026-01-01T00:00:00Z",
  "revoked_entries": [
    {
      "revocation_target_type": "access_grant",
      "revocation_target_identifier": "01J9ZBQ0M4T...",
      "revoked_at": "2026-08-05T19:44:30Z",
      "revocation_reason_code": "subject_withdrew_consent"
    }
  ],
  "signatures": [ { "...": "issuer signature" } ]
}
```

**Reasoning (`list_sequence_number` and `next_update_expected_at`):** Together they let a
Verifier detect both rollback (an attacker replaying an older list) and staleness (the
Verifier's copy is past its expected refresh). A signed list without a sequence number is
replayable, which silently undoes revocation.

**Reasoning (`covers_artifact_types` and `covers_issued_from`):** A Verifier must know the
list's scope. Otherwise "not in the list" is ambiguous between "not revoked" and "not covered
by this list," and implementations will guess the convenient answer.

### 14.5. Freshness Policy and Fail-Closed Behavior

Each Verifier configures, per artifact type:

- `maximum_revocation_list_age_seconds`
- `behavior_when_status_unavailable`: `deny`, `allow_with_short_lifetime`, or
  `allow_and_log`

For any operation with material side effects, `behavior_when_status_unavailable` **MUST** be
`deny`.

**Reasoning:** This is where transport independence and revocability genuinely conflict. An
offline Verifier cannot know current revocation state. Rather than pretending otherwise,
TAAP/1 makes the tradeoff explicit and configurable, and forces the safe choice for
consequential actions. A door lock reading a badge offline may reasonably `allow_and_log`;
a system wiring money may not.

### 14.6. Push and Pull

Revocation may be delivered by push (`revocation_notice`) or pull (`revocation_list_query`).
Verifiers **MUST** support pull; push is **RECOMMENDED** where the transport allows it.

**Reasoning:** Push is faster but unreliable across arbitrary transports; pull always works
if connectivity exists at all. Requiring pull guarantees a floor of correctness.

### 14.7. Short Lifetimes as Complementary Defense

Because tokens are short-lived (Section 12.4), the practical exposure window after revocation
is bounded even for a fully offline Verifier.

---

## 15. Discovery and Key Resolution

### 15.1. Resolution Is Pluggable

TAAP/1 does not mandate how a Verifier obtains an identity document. Conforming methods
include:

1. **Direct presentation** — the party sends its identity document in-band.
2. **Local trust store** — pre-provisioned, as with pinned keys.
3. **Directory service** — any queryable store, over any transport.
4. **Transparency log** — an append-only log of identity documents.
5. **Out-of-band** — QR code, printed card, verbal fingerprint exchange.

**Reasoning:** Naming and discovery are where most decentralized identity efforts bog down in
governance. Because identifiers are key-derived, TAAP/1 does not *need* a trusted directory
for correctness — only for convenience and freshness. Leaving resolution pluggable keeps the
core protocol shippable.

### 15.2. Direct Presentation Is Always Sufficient for Signature Verification

Since the identifier is a hash of the key, a Verifier can validate that a presented document
matches a claimed identifier with no external lookup.

### 15.3. Resolution Cannot Confer Authority

Obtaining a key from a directory establishes only *which key* corresponds to an identifier.
Whether that identifier is *authorized* for anything is a separate policy decision.

**Reasoning:** Conflating resolution with authorization means whoever runs the directory
controls access. Keeping them separate preserves per-Verifier trust (Section 4.2).

### 15.4. Trust Policy Object

Verifiers **SHOULD** express trust as data rather than code:

```json
{
  "artifact_type": "trust_policy",
  "policy_identifier": "registrar-verifier-policy-v3",
  "trusted_attestation_authorities": [
    {
      "authority_identifier": "taap:1:ed25519:UNIVERSITYKEY...",
      "trusted_for_profiles": ["academic_degree/1", "academic_enrollment/1"],
      "trusted_from": "2026-01-01T00:00:00Z",
      "trusted_until": null
    }
  ],
  "trusted_grant_authorities": [
    { "authority_identifier": "taap:1:ed25519:GRANTKEY..." }
  ],
  "acceptable_signature_algorithms": ["ed25519", "p256"],
  "clock_skew_allowance_seconds": 60,
  "maximum_revocation_list_age_seconds": 300,
  "behavior_when_status_unavailable": "deny"
}
```

**Reasoning (`trusted_for_profiles`):** Scoped trust. A university is authoritative about its
own degrees and not about, say, medical licensure. Unscoped trust roots are how one
compromised issuer becomes a universal forgery capability.

---

## 16. Transport Bindings

A binding specifies framing, direction, and size limits. It **MUST NOT** add security
semantics.

### 16.1. Stream Binding (TCP, TLS, Unix sockets)

Length-prefixed frames: 4-byte big-endian unsigned length, then that many bytes of encoded
envelope. Maximum frame size is negotiated or defaults to 1 MiB.

### 16.2. Datagram Binding (UDP, MQTT, message queues)

One envelope per datagram or message. Envelopes exceeding the path MTU **MUST** use the
stream binding instead; TAAP/1 defines no fragmentation.

**Reasoning:** Application-layer fragmentation of security-critical messages invites
reassembly attacks. Refusing to define it is safer than defining it badly.

### 16.3. Store-and-Forward Binding (Email, Files)

- Media type `application/taap+json` or `application/taap+cbor`.
- One envelope per part; multiple parts allowed in one message.
- `expires_at` **SHOULD** be at least 3600 seconds to tolerate queuing delay.
- Interactive challenge–response over email is **NOT RECOMMENDED** because round-trip latency
  exceeds sane nonce lifetimes; attestation delivery and revocation notices are appropriate.

**Reasoning:** This binding is what "can it work over email?" actually looks like. It works
well for one-shot, long-lived artifacts and poorly for tight interactive handshakes. Saying so
explicitly prevents implementers from building an unusable sign-in flow.

### 16.4. Visual Binding (QR Codes, Printed Media)

- `ccbor` encoding **REQUIRED**.
- Compression (Deflate) applied after canonicalization, before encoding, and **MUST NOT**
  change signed bytes.
- Multi-code sequences carry `sequence_index` and `sequence_total` outside the signed
  envelope.

**Reasoning:** Compressing *after* canonicalization keeps the signed byte stream unambiguous.
Compressing before would make signature verification dependent on the compressor's behavior.

### 16.5. Unidirectional Binding

Where no return channel exists, only self-contained artifacts may be conveyed
(`attestation_issue`, `revocation_list_response`, `token_issue`). Challenge–response requires
bidirectionality.

---

## 17. Error Handling

```json
{
  "error_code": "token_expired",
  "error_description": "Token expired at 2026-08-05T19:58:12Z",
  "failed_message_identifier": "01J9ZBQ9F...",
  "error_occurred_at": "2026-08-05T20:01:00Z",
  "retry_advice": {
    "retry_is_useful": true,
    "retry_after_seconds": 0,
    "suggested_remedy": "refresh_token"
  }
}
```

Registered codes include `signature_invalid`, `signature_algorithm_not_accepted`,
`identity_document_unknown`, `identity_document_stale`, `nonce_unknown`, `nonce_replayed`,
`message_expired`, `token_expired`, `token_not_yet_valid`, `token_binding_proof_missing`,
`token_binding_proof_invalid`, `wrong_resource_holder`, `scope_not_authorized`,
`artifact_revoked`, `revocation_status_unavailable`, `attestation_authority_not_trusted`,
`attestation_profile_unsupported`, `attribute_commitment_mismatch`, `consent_required`,
`consent_denied`, `grant_expired`, `encoding_invalid`, `protocol_version_unsupported`,
`rate_limited`, `internal_error`.

Error messages **MUST NOT** reveal whether an identifier exists when that would leak
information to an unauthenticated party.

**Reasoning (`retry_advice`):** Clients otherwise implement retry heuristics by trial and
error, producing either infinite loops against permanent failures or premature abandonment of
transient ones. Telling the Client what to do is cheap and improves whole-system behavior.

**Reasoning (distinct codes for `artifact_revoked` vs `revocation_status_unavailable`):**
These demand different Client behavior — one is permanent, the other is retryable — and
collapsing them leads Clients to retry forever against revoked artifacts.

---

## 18. Security Considerations

### 18.1. Threat Model

Assumed adversary capabilities: read, drop, reorder, replay, and inject arbitrary messages on
any transport; register their own identities; operate a malicious Client, Verifier, or
directory; and compromise a single key.

Out of scope: compromise of a Subject's device at the moment of consent; compromise of an
Authority's signing key (mitigated only by rotation and revocation); coercion of a Subject.

### 18.2. Replay

Countermeasures: Verifier-chosen nonces (9.4), `message_identifier` uniqueness with a replay
cache (7.3), `expires_at` on messages and tokens, `signature_purpose` and domain separation
(6.6, 6.7), and `list_sequence_number` on revocation lists (14.4).

### 18.3. Relay and Confused Deputy

Countermeasures: `verifier_identifier` echoed in the signed authentication response (9.4),
single mandatory `intended_resource_holder_identifier` (12.2), optional channel binding (9.6),
and per-request proof-of-possession (12.3).

### 18.4. Token Theft

Sender-constrained tokens by default, short lifetimes, and cascading revocation. A token
alone is insufficient without the bound key.

### 18.5. Signature Substitution and Algorithm Confusion

Verifier-side algorithm accept-lists (6.8), algorithm declared inside signed material,
explicit `key_usage` separation (5.3), and refusal to infer algorithm from key or signature
shape.

### 18.6. Denial of Service

Verification is cheap-first ordered (13.2); frame size limits (16.1); replay cache pruning by
sortable identifier (7.3); rate limiting signalled by `rate_limited`. Note that
signature verification on every request is a real cost — Verifiers **SHOULD** rate-limit
before verifying where possible.

### 18.7. Key Compromise

An identity document declaring `key_compromised` **MUST** cause Verifiers to reject
signatures made by that key after the declared compromise time, and **SHOULD** trigger review
of artifacts signed before it. Attestations signed by a compromised Authority key **SHOULD**
be treated as suspect pending re-issuance.

### 18.8. Downgrade

`protocol_version` is inside the signed envelope; Verifiers **MUST** reject unexpected
versions rather than negotiating downward. Bearer mode **MUST NOT** be selectable by the
requesting party — only by Verifier policy.

### 18.9. Cryptographic Recommendations

Default signature algorithm: **Ed25519**. Alternatives: ECDSA on P-256 for hardware and
compliance constraints; RSA-PSS with SHA-512 and at least 3072-bit moduli for legacy
interoperation. Hash: SHA-512/256. Randomness: CSPRNG only, at least 128 bits for nonces and
salts.

**Reasoning (Ed25519 default):** Fixed small key and signature sizes, no per-signature
randomness requirement (deterministic, so a bad RNG at signing time does not leak the key),
no point-format ambiguity, and widely available constant-time implementations. Its main
drawbacks are weaker hardware-token and FIPS coverage than P-256, which is why P-256 remains
a first-class alternative.

### 18.10. Post-Quantum Readiness

Algorithm identifiers are strings and every signed object declares its own algorithm, so
adding ML-DSA or SLH-DSA requires only registry additions. Verifiers **MAY** require multiple
signatures with different algorithms on a single artifact, enabling hybrid transition.

**Reasoning:** Long-lived attestations (a degree, valid for decades) will outlive current
signature assumptions. Building agility and multi-signature in from the start is far cheaper
than retrofitting.

---

## 19. Privacy Considerations

### 19.1. Correlation

A stable, key-derived identifier is inherently correlatable across Verifiers.

Mitigations: per-relationship identities (a Subject **MAY** hold many identities), selective
disclosure (10.4), and minimal `attested_attribute_summary` content.

**Stated limitation:** TAAP/1 does not provide unlinkability. Two Verifiers who collude can
determine they saw the same Subject if the same identifier was used. Full unlinkability
requires anonymous-credential schemes; see **[OPEN QUESTION 22.4]**.

### 19.2. Verifier Phone-Home

Status checks can reveal to an Authority when and where a Subject presents an attestation.

Mitigations: cached revocation lists rather than per-artifact queries, list-based status so
the Authority learns nothing about which entry was of interest, and generous
`maximum_status_age_seconds` for slow-changing claims.

**Reasoning:** Per-artifact online status checks are a surveillance channel. List-based
distribution is more bandwidth but far less revealing, which is why 14.6 requires pull
support.

### 19.3. Data Minimization

Verifiers **SHOULD** request the narrowest profile and fewest attributes sufficient, and
**SHOULD NOT** retain disclosed attributes longer than needed for the decision and its audit
record.

### 19.4. Consent Records Are Personal Data

Signed consent responses are strong evidence and also sensitive. They **SHOULD** be retained
under a stated policy and **SHOULD** be available to the Subject.

---

## 20. Design Rationale (Consolidated)

| Decision | Alternative rejected | Reasoning |
|---|---|---|
| Transport-agnostic signed messages | HTTP-bound endpoints | Founding requirement G1; also enables offline and store-and-forward use. |
| Full field names (`subject_identifier`) | JWT-style `sub`/`iss`/`aud` | Abbreviations existed for URL and header compactness, a constraint TAAP/1 does not have. Readability reduces audit and implementation error. |
| Uncompressed, named key coordinates | Compressed points, opaque blobs | Self-describing, no decompression convention to get wrong, no truncation ambiguity. |
| Key-derived identifiers | Registry-assigned names, URLs | Self-certifying; no naming authority; works offline; resists key substitution. |
| Base32 fingerprints | Base64, hex | Case-insensitive and transcription-robust for print, QR, and voice. |
| Two canonical encodings | One encoding | JSON for humans and email, CBOR for constrained and visual transports. |
| No floating point | Allow JSON numbers generally | Float serialization is non-deterministic, breaking canonical signing. |
| RFC 3339 string timestamps | Epoch integers | Readable in audit; no unit or rollover ambiguity. |
| Signed envelope covering routing metadata | Sign body only | Prevents type relabeling, recipient redirection, and lifetime extension. |
| Domain separation plus `signature_purpose` | Plain signature over payload | Prevents cross-type and cross-purpose signature reuse. |
| Sender-constrained tokens by default | Bearer tokens | Removes the "token is a password" failure mode; nearly free since all parties already have keys. |
| Short token lifetime, long grant lifetime | Long-lived tokens | Bounds leak damage; concentrates revocation on one inspectable artifact. |
| Structured, signed grant | Opaque refresh token | Subject can see and audit exactly what was granted. |
| Subject-signed consent | Server-side consent record only | Non-repudiable in both directions; gives the Subject cryptographic evidence. |
| Authority separate from Grant Authority | OIDC-style claims from the token issuer | Third-party attribution (a university, a licensing board) needs an independent signer. |
| Salted per-attribute commitments | Unsalted hashes; BBS+; full disclosure | Implementable and constrained-device friendly; salting defeats brute force. BBS+ deferred as an upgrade. |
| `status_policy` set by the issuer | One global freshness rule | Only the issuer knows how volatile its claim is. |
| Cascade revocation by default | Revoke target only | Matches user intent when they revoke an app's access. |
| Explicit fail-closed configuration | Implicit fail-open | Makes the offline-versus-revocation tradeoff visible and forces safety for side-effecting operations. |
| Per-Verifier scoped trust policy | Global trust root | Avoids a single compromise becoming universal forgery; avoids governance deadlock. |
| Ordered mandatory verification checklist | Prose guidance | Verification gaps are the most common real-world failure; ordering also limits DoS cost. |

---

## 21. Comparison to OAuth 2.x, OIDC, and W3C VC

| Aspect | OAuth 2.x / OIDC | W3C Verifiable Credentials | TAAP/1 |
|---|---|---|---|
| Transport | HTTP required | Transport-neutral data model, HTTP-centric in practice | Any transport, explicitly bound |
| Identity | Account at an issuer | DIDs, method-dependent | Key-derived identifier, self-certifying |
| Field naming | Abbreviated (`iss`, `sub`, `exp`) | Mixed, JSON-LD vocabulary | Fully spelled out |
| Third-party attribution | Not modelled | Core feature | Core feature, with issuer-set status policy |
| Token possession | Bearer by default | N/A | Sender-constrained by default |
| Consent artifact | Server-side record | N/A | Subject-signed, non-repudiable |
| Revocation | RFC 7009 endpoint, token-focused | Status lists per credential | Uniform model for tokens, grants, and attestations |
| Selective disclosure | Not applicable | Supported via SD-JWT, BBS+ | Salted per-attribute commitments |
| Offline verification | Generally no | Yes for signatures | Yes for signatures; revocation freshness explicit |

### 21.1. Bridging

An HTTP binding for TAAP/1 is straightforward: carry envelopes as request and response bodies
with the TAAP media types. A gateway may also translate TAAP grants into OAuth tokens for
legacy resource servers, at the cost of losing sender constraint and the signed consent
chain. Such a gateway **MUST** be treated as a trusted component and **SHOULD** log the
downgrade.

---

## 22. Open Questions for the Protocol Owner

These need your input before the draft stabilizes.

**22.1. Primary environment.** Which target matters most — constrained IoT, peer-to-peer
applications, enterprise services, offline or air-gapped, or store-and-forward? This changes
encoding defaults, lifetime defaults, and whether `cjson` should be optional.

**22.2. Identifier stability across rotation.** Section 5.5 keeps the original identifier
stable after key rotation, which sacrifices strict self-certification (a Verifier must fetch
the document chain to learn the current key). The alternative — the identifier changes with
the key — preserves self-certification but breaks every stored reference. Which do you prefer?

**22.3. Attestations about organizations and machines.** Should the Subject of an attestation
be allowed to be a non-human (a device, a build artifact, a corporate entity)? Supporting this
well probably means additional profiles and possibly delegation chains.

**22.4. Advanced privacy.** Do you need predicate proofs ("over 21" without the birth date)
and unlinkability across Verifiers? If yes, plan for BBS+ or similar anonymous credentials,
which is a substantial complexity and implementation-maturity cost. If no, the current salted
commitment scheme is much simpler.

**22.5. Delegation chains.** Should a Client be able to sub-delegate to another Client
(A grants to B, B grants a subset to C)? This is powerful and a common source of confused
deputy bugs; it needs explicit depth limits and attenuation-only rules if included.

**22.6. Recovery.** What happens when a Subject loses all keys? Options include social
recovery with threshold signatures, an Authority-mediated re-binding of attestations to a new
identity, or explicitly out of scope. This is the most common real-world failure of key-based
identity systems.

**22.7. Attestation portability.** If a Subject rotates identity, should existing attestations
automatically follow? Doing so requires either re-issuance by each Authority or a signed
linkage artifact, each with different trust implications.

**22.8. Ledger dependency.** Are you willing to depend on a distributed ledger or transparency
log for key and revocation distribution, or must the protocol work with purely local trust
stores? Currently the draft assumes the latter and treats logs as optional.

**22.9. Bearer mode.** Should bearer tokens be permitted at all, or removed entirely for a
stricter, simpler security story at the cost of excluding some Clients?

**22.10. Naming.** "TAAP" is a placeholder. Also, the `taap:1:...` identifier scheme should be
confirmed — reusing `did:` would gain ecosystem tooling but inherits DID method governance.

---

## 23. Local Registries

### 23.1. Signature Algorithm Identifiers

| Identifier | Algorithm | Status |
|---|---|---|
| `ed25519` | Ed25519 (RFC 8032) | REQUIRED |
| `p256` | ECDSA P-256 with SHA-256 | RECOMMENDED |
| `p384` | ECDSA P-384 with SHA-384 | OPTIONAL |
| `rsa-pss-sha512` | RSA-PSS, SHA-512, ≥3072-bit | OPTIONAL |
| `ml-dsa-65` | ML-DSA-65 | RESERVED |
| `slh-dsa-sha2-128s` | SLH-DSA | RESERVED |

### 23.2. Message Types

As enumerated in Section 8. New types require a registry entry with name, direction, body
schema, replay rules, and required `signature_purpose`.

### 23.3. Attestation Profiles

Initial profiles: `academic_degree/1`, `academic_enrollment/1`, `professional_license/1`,
`employment/1`, `organization_membership/1`, `age_threshold/1`.

Each profile registration **MUST** specify attribute names, value types and formats, required
versus optional attributes, and expected volatility to guide `status_policy`.

### 23.4. Scope Names

Scopes are Resource-Holder-defined. Registration **SHOULD** include a canonical
human-readable description, whether the scope permits side effects, and whether
`behavior_when_status_unavailable` must be `deny`.

### 23.5. Revocation Reason Codes

`subject_withdrew_consent`, `client_no_longer_needed`, `key_compromised`,
`artifact_issued_in_error`, `underlying_fact_no_longer_true`, `superseded_by_reissuance`,
`policy_violation`, `administrative_action`, `unspecified`.

---

## 24. Appendix A: Worked Example

**Scenario.** Alice holds a degree attestation from Example State University. A hiring
platform's Client wants to read her verified degree from her personal credential store, which
acts as Resource Holder. Everything runs over a Bluetooth Low Energy link between her phone
and a kiosk — no HTTP anywhere.

1. **Kiosk authenticates Alice.** Kiosk sends `authentication_challenge` with a 128-bit
   nonce, its own `verifier_identifier`, and a five-minute expiry. Alice's phone returns
   `authentication_response` echoing the nonce and the kiosk's identifier, signed with
   `signature_purpose: "authentication_response"`. The kiosk verifies against the identity
   document Alice presents, confirming the document hash matches her identifier.

2. **Client requests access.** The kiosk's Client sends `access_request` for scope
   `read_verified_degree`, requesting a 900-second token lifetime and a 24-hour grant.

3. **Alice consents.** Her phone, acting as Grant Authority for her own store, displays the
   Client name, the resource, and the scope. She approves. Her phone emits a
   `consent_response` signed with `signature_purpose: "consent_response"`, including a digest
   of exactly the text shown.

4. **Grant and token issued.** Her phone issues an `access_grant` referencing the consent
   digest, and an `access_token` bound to the Client's key with a 900-second lifetime and
   `intended_resource_holder_identifier` set to her store.

5. **Client reads the resource.** The Client sends `resource_request` carrying the token and
   signing the envelope with its bound key. Her store runs the Section 13.2 checklist and
   returns the degree attestation with only `degree_level` and `field_of_study` disclosed,
   plus salts for those two attributes.

6. **Kiosk verifies attribution.** It checks the university's signature over the attestation,
   recomputes the two commitments, confirms the university is trusted for
   `academic_degree/1` in its trust policy, and applies `status_policy`: the list it cached
   nine days ago is within the 30-day `maximum_status_age_seconds`, so no network is needed.

7. **Alice revokes.** Walking away, she taps "revoke." Her phone records a revocation of the
   grant with `revocation_cascade: "revoke_derived_artifacts"`. The token would have expired
   within fifteen minutes anyway; the grant is dead immediately.

Note what did not happen: no redirect, no URL, no browser, no bearer token, no full birth
date, no shared secret, and no dependence on the kiosk being online.

---

## 25. Appendix B: Reserved Field Name Index

Envelope: `protocol_version`, `encoding`, `message_type`, `message_identifier`,
`conversation_identifier`, `sender_identifier`, `recipient_identifier`, `created_at`,
`expires_at`, `message_body`, `signatures`.

Signature: `signature_algorithm`, `signing_key_identifier`, `signing_key_label`,
`signature_purpose`, `signed_at`, `signature_bytes_base64`.

Keys and identity: `key_algorithm`, `key_usage`, `key_label`, `public_key_bytes_base64`,
`public_key_x_base64`, `public_key_y_base64`, `point_encoding`, `modulus_base64`,
`public_exponent_base64`, `modulus_bit_length`, `identifier`, `keys`, `display_name`,
`controller`, `previous_document_hash_base64`, `created_at`, `updated_at`.

Attestation: `attestation_identifier`, `authority_identifier`, `authority_display_name`,
`subject_identifier`, `attestation_profile`, `asserted_attributes`, `evidence_references`,
`issued_at`, `valid_from`, `expires_at`, `status_policy`,
`attribute_disclosure_commitments`.

Grant and token: `grant_identifier`, `grant_authority_identifier`, `client_identifier`,
`resource_holder_identifier`, `intended_resource_holder_identifier`, `approved_scopes`,
`authorized_scopes`, `consent_message_identifier`, `consent_response_digest_base64`,
`maximum_token_lifetime_seconds`, `grant_binding`, `token_identifier`, `token_binding`,
`not_valid_before`, `attested_attribute_summary`, `single_use_required`.

Revocation: `revocation_target_type`, `revocation_target_identifier`, `revocation_cascade`,
`revocation_reason_code`, `revocation_reason_description`, `requested_effective_at`,
`requester_role`, `revocation_list_identifier`, `issuing_authority_identifier`,
`list_sequence_number`, `generated_at`, `next_update_expected_at`, `covers_artifact_types`,
`covers_issued_from`, `revoked_entries`, `revoked_at`.

Errors: `error_code`, `error_description`, `failed_message_identifier`,
`error_occurred_at`, `retry_advice`, `retry_is_useful`, `retry_after_seconds`,
`suggested_remedy`.

No field name in this protocol is abbreviated. There is no `iss`, `sub`, `aud`, `exp`,
`nbf`, `iat`, or `jti`.

---

## References

- BCP 14 / RFC 2119 / RFC 8174 — Requirement level key words
- RFC 3339 — Date and time on the Internet
- RFC 4648 — Base16, Base32, Base64 encodings
- RFC 6749, RFC 6750 — OAuth 2.0 framework and bearer token usage
- RFC 7009 — OAuth 2.0 token revocation
- RFC 7662 — OAuth 2.0 token introspection
- RFC 8032 — EdDSA (Ed25519)
- RFC 8949 — CBOR, including deterministic encoding
- RFC 9449 — OAuth 2.0 Demonstrating Proof of Possession (DPoP)
- draft-ietf-oauth-v2-1 — OAuth 2.1
- W3C Verifiable Credentials Data Model
- W3C Decentralized Identifiers (DIDs)
- FIPS 186-5, FIPS 204, FIPS 205 — Digital signature standards
