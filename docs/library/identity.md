# TAAP/1 identity and signing

**What.** libliquid creates a TAAP device identity and signs every stored drip and change with a
software Ed25519 key, a FIDO2 credential, or PIV slot `9c`. An actor identifier begins
`taap:1:<algorithm>:` and uses unpadded uppercase RFC 4648 base32 of SHA-512/256 over the
canonical-CBOR public-key object. Rotation preserves the original TAAP identifier while the
current key's algorithm, fingerprint, and custody may change.

## Exact signed bytes

For body `B`, liquid signs `ASCII("TAAP/1 " + artifact_type) || 0x00 || canonical_cbor(B)`.
`B` has no `signatures` field (and a liquid change has no `sig` field). Artifact types are `drip`,
`change`, and `identity_document`. The canonical signature object has the §6.7 fields and its
`signature_purpose` must equal the artifact type.

Identity documents are canonical CBOR with `artifact_type = "identity_document"`, `encoding =
"ccbor"`, and a `signatures` array. A predecessor hash is SHA-512/256 of the predecessor's stored
canonical CBOR bytes, signatures included, encoded as base64.

## Public wire codecs

`encode_identity_document(document)` returns the complete canonical-CBOR record, including its
signatures. `decode_identity_document(bytes)` accepts `std::span<const std::byte>`, rejects malformed
or non-canonical input, and returns `result<identity_document>`. Both the initial document and every
rotation successor use this same public record type and codec. `decode_signature(bytes)` decodes a
canonical TAAP/1 signature object into `result<signature>` and rejects unsupported algorithms or
invalid signature lengths or algorithm-specific fields.

The codecs do not verify trust. Call `verify_document` for one decoded document and `verify_chain`
for an ordered initial-document and rotation-document chain. `cli-identity-codec` runs `liquid
identity show` and `liquid identity rotate`, then round-trips and verifies their exact stored files
through these public functions. The identity unit test also decodes a complete canonical signature
object through `decode_signature`.

## Rotation

`signer::rotate(compromise_declaration?)` creates the successor document and moves the local
signer to its new Ed25519 key. Its subject identifier and identifier fingerprint remain those of
the first document; `key_fingerprint` identifies the particular public key. The successor has the
predecessor's stored-byte hash, a strictly later `updated_at`, and signatures over its body without
the `signatures` field. It normally carries both the previous and new key signatures. The local
private key is overwritten only after the successor is signed, leaving only the active private key
on disk.

`compromised_keys`, when non-empty, is part of that signed body. Each entry is
`{ key_fingerprint_base32, declared_at, reason? }`: the fingerprint is unpadded uppercase base32
of SHA-512/256 over the key's canonical public-key object, `declared_at` is an RFC 3339 UTC time,
and `reason` is optional. A successor may omit the old-key signature only when its own
`compromised_keys` names the predecessor key. A signature from that key at or after `declared_at`
is invalid; one made before it is valid but the resulting `verified_identity` reports compromised
history.

Documents are retained as canonical CBOR at
`$LIQUID_HOME/identity/<identity-fingerprint>/documents/NNN.cbor`; the legacy current-document
path is updated to the newest document for local compatibility. `verify_chain` checks stored-byte
links, stable identifiers, timestamps, declarations, and the required signatures, then returns the
current key set. `verify(actor, ...)` resolves a non-current signing key through that local chain.

A verifier that holds only an old document can validate that document and signatures made with its
key, but cannot identify a successor or its current key without receiving or resolving the chain.
When presented a normal successor, it can prove old-key continuity. A compromise successor proves
that the new key signed the declaration, but cannot prove continuity of control through the omitted
old-key signature; that claim remains subject to verifier policy.

`signer::rotate_to(custody, compromise?)` selects `software`, `fido2`, or `piv:9c`. The successor
document includes the custody name, changes `key_algorithm` and `key_fingerprint`, and otherwise
obeys the same dual-signature and compromise rules. `liquid identity rotate --to <custody>` exposes
the operation; `liquid sign <drip|change|artifact> <digest>` uses whichever backend is current.

## Hardware backends and algorithms

The private backend interface lives in `src/identity/keys/`. Software signing remains RFC 8032
Ed25519. PIV selects the standard application over the OS PC/SC service, generates a P-256 key in
digital-signature slot `9c`, and uses the command and TLV forms in
[NIST SP 800-73pt2-5](https://doi.org/10.6028/NIST.SP.800-73pt2-5); signatures are tagged
`ecdsa-p256`.
FIDO2 uses CTAP2 `authenticatorMakeCredential` over USB HID, requests the `hmac-secret` extension,
prefers COSE EdDSA `-8`, and falls back to ES256 `-7`. Its signature tags are `fido2-eddsa` and
`fido2-es256`.

For FIDO2, liquid sets `clientDataHash = SHA-256(TAAP signed bytes)`. The authenticator assertion
signs `authenticatorData || SHA-256(clientDataHash)`. The TAAP signature object therefore adds
`authenticator_data_base64` and `client_data_hash_base64`. Verification rebuilds the TAAP signed
bytes, checks the carried client hash, reconstructs the assertion input, and verifies Ed25519 or
P-256 entirely in software. This additional signature-object reading is recorded as protocol
feedback on [liquid-bridge #8](https://github.com/oramasearch/liquid-bridge/issues/8#issuecomment-5562172800),
because TAAP §6.7 currently specifies only a direct signature byte string.

Device calls are blocking OS operations and run through `runtime::offload`. CTAP2 and PIV command
layers are owned here; no libfido2 or OpenSC types cross the backend boundary. In-process doubles
consume the same CTAP2 CBOR and PIV APDUs used by devices. Verification uses software vectors and
performs no device I/O.

## Local custody and verification

Identities live at `$LIQUID_HOME/identity` or `~/.liquid/identity`. `<fingerprint>.key` holds the
encrypted active private key, `<fingerprint>.pub` the active public key, and
`<fingerprint>.identity.cbor` the current document. The owner-only local wrapping-key provider and
software private key are never logged; `signer` wipes its secret material on destruction.
Hardware `.key` records contain only the public credential descriptor needed to find the token key
again.

`verify(actor, artifact_type, canonical_body, signature)` is synchronous. It returns `true` for a
valid signature, `false` for an unsigned record, and `errc::unauthorized` for invalid signed data.
`verify_document` checks the self-signature. Drips and changes use the same verification rules.

## CRDT commit metadata

Each CRDT transaction for a drip passes the operation message and its Unix millisecond timestamp to
automerge-cpp through `CommitOptions`. This makes the metadata available to CRDT history, compact
checkpoints, and peers without making it an attribution authority. The TAAP signature input is
unchanged: liquid still signs the same domain-separated canonical drip body described above, and
the automerge change hash is not signed in its place. Journal replay supplies the recorded message
and timestamp again so the reconstructed automerge change hash is identical.

## Measurement and limits

On the development machine, 1,000 canonical drip-body signatures averaged **198,234 ns** each,
measured by `Identity.SigningCostMeasurement` in the Release build. Batching is deferred to #49.
Actor subjects support Ed25519, P-256, and the two FIDO2 assertion forms. Object subjects await
liquid-bridge #1; delegation and session keys remain separate work.
