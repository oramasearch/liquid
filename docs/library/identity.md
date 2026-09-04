# TAAP/1 identity and signing

**What.** libliquid creates a local RFC 8032 Ed25519 device identity and signs every stored drip
and change. An actor identifier is `taap:1:ed25519:<fingerprint>`, using unpadded uppercase RFC
4648 base32 of SHA-512/256 over the canonical-CBOR public-key object.

## Exact signed bytes

For body `B`, liquid signs `ASCII("TAAP/1 " + artifact_type) || 0x00 || canonical_cbor(B)`.
`B` has no `signatures` field (and a liquid change has no `sig` field). Artifact types are `drip`,
`change`, and `identity_document`. The canonical signature object has the §6.7 fields and its
`signature_purpose` must equal the artifact type.

Identity documents are canonical CBOR with `artifact_type = "identity_document"`, `encoding =
"ccbor"`, and a `signatures` array. A predecessor hash is SHA-512/256 of the predecessor's stored
canonical CBOR bytes, signatures included, encoded as base64.

## Local custody and verification

Identities live at `$LIQUID_HOME/identity` or `~/.liquid/identity`. `<fingerprint>.key` holds the
encrypted private key, `<fingerprint>.pub` the public key, and `<fingerprint>.identity.cbor` the
self-signed document. The owner-only local wrapping-key provider and private key are never logged;
`signer` wipes its secret material on destruction.

`verify(actor, artifact_type, canonical_body, signature)` is synchronous. It returns `true` for a
valid signature, `false` for an unsigned record, and `errc::unauthorized` for invalid signed data.
`verify_document` checks the self-signatures. Drips and changes use the same verification rules.

## Measurement and limits

On the development machine, 1,000 canonical drip-body signatures averaged **198,234 ns** each,
measured by `Identity.SigningCostMeasurement` in the Release build. Batching is deferred to #49.
This issue supports actor subjects and Ed25519 only; object subjects await
liquid-bridge #1, and delegation, session keys, and remote or hardware custody are later work.
