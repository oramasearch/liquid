# Standards

External standards liquid depends on that do not yet have a home of their own.

| File | What it is |
|---|---|
| [`taap-1-spec-draft.md`](taap-1-spec-draft.md) | **TAAP/1** — Transport-Agnostic Authorization and Attribution Protocol. Draft `1.0.0-draft.1`, dated 2026-08-05. The identity, attestation and delegation protocol liquid uses to identify every party that commits a drip. |
| [`taap-origin-conversation.md`](taap-origin-conversation.md) | The research conversation the spec came out of — kept for provenance, not as normative content. |

## Why these are here

**TAAP/1 currently exists only as a draft in one person's `Downloads` folder.** A
protocol that liquid's entire identity and attestation story depends on cannot live
outside version control, so it is vendored here.

**This is a holding position, not the destination.** TAAP is a protocol with a scope
much wider than liquid — it is transport-agnostic by design and is meant to be usable
over email, QR codes and NFC. It should get its own repository so it can be versioned,
reviewed and eventually published independently of this project. When it does, this
directory becomes a pinned reference rather than a copy.

## Status

TAAP/1 is a **draft with open questions**, several of which liquid is a concrete driver
for. See [`../features/25-identity-attestation-and-transparency.md`](../features/25-identity-attestation-and-transparency.md)
for which ones liquid answers and how.

Do not treat the vendored copy as authoritative once the spec has its own repository.
Record the commit or version this copy corresponds to when that happens.
