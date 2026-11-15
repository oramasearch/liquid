# TAAP/1 identity and signing

**What.** libliquid creates a TAAP device identity and signs every stored drip and change with a
software Ed25519 key, a FIDO2 credential, or PIV slot `9c`. An actor identifier begins
`taap:1:<algorithm>:` and uses unpadded uppercase RFC 4648 base32 of SHA-512/256 over the
canonical-CBOR public-key object. Rotation preserves the original TAAP identifier while the
current key's algorithm, fingerprint, and custody may change.

## Before signing up for lake

Run this command, then paste its output into lake's TAAP/1 identity document field:

```sh
liquid identity show --document
```

The command works before creating a repository and creates a software identity on first use.
It writes only the complete signed public document as unpadded base64url canonical CBOR, with no
whitespace, newline, prose, or ANSI escapes, and exits 0. The private signing key stays local:
software keys remain on the client, and FIDO2 and PIV keys remain on their tokens. Exporting an
existing identity does not ask the token to sign again or change the identity.

`--document --json` is refused with a clear usage error on stderr and exit 2. `--document` applies
only to `show`; using it with `rotate` also returns exit 2. `--chain` prints the whole chain instead
of the current document, for `liquid identity trust` on another machine (see
[below](#how-a-verifier-obtains-and-trusts-another-persons-chain)). Ordinary text and JSON summaries retain
their existing behavior. After a rotation, export returns the current successor document.

The output matches [lake's sign-up identity contract](https://github.com/oramasearch/lake/blob/main/docs/api/identity.md):
base64url wraps the complete canonical record, including its self-signature. The CBOR fields include
`protocol_version = "TAAP/1"`, `artifact_type = "identity_document"`, `encoding = "ccbor"`,
`identifier`, `keys`, and `signatures`. Lake decodes, re-encodes, and verifies that record
using the same libliquid public codecs and `verify_document`.

For inspection with a standard-base64 decoder, translate the URL alphabet and restore padding:

```sh
liquid identity show --document | python3 -c 'import base64,sys; s=sys.stdin.buffer.read(); sys.stdout.buffer.write(base64.urlsafe_b64decode(s + b"=" * (-len(s) % 4)))' | xxd
```

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

`signer::document()` returns a copy of the public signed document retained when the signer is
created or opened, and updated after successful rotation. It performs no I/O or signing and returns
`errc::invalid_argument` for an empty signer. The CLI passes this value to
`encode_identity_document`; it has no second CBOR encoder or direct access to key files.

The codecs do not verify trust. Call `verify_document` for one decoded document and `verify_chain`
for an ordered initial-document and rotation-document chain. `cli-identity-codec` runs `liquid
identity show` and `liquid identity rotate`, then round-trips and verifies their exact stored files
through these public functions. It also exercises first-use and post-rotation `--document` output,
verifies byte equality with the stored document, checks the base64url alphabet and absence of all
whitespace, and proves refused flag combinations exit 2 before creating an identity.
`IdentityDocument.*` captures the CLI handler output for software, FIDO2 EdDSA, FIDO2 ES256, and PIV.
It independently decodes base64url and verifies the canonical record and self-signature. Tests
assert that neither stdout nor the decoded CBOR contains raw, base64, base64url, or hexadecimal
private key material, including the Ed25519 seed; hardware cases use the existing protocol doubles.
The identity unit test also decodes a complete canonical signature
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
path is updated to the newest document for local compatibility. `verify_chain` checks that the
initial document's identifier is derived from its own key, then stored-byte links, stable
identifiers, timestamps, declarations, and the required signatures, and returns the current key
set. `verify(actor, ...)` resolves a non-current signing key through the stored chain.

A verifier that holds only an old document can validate that document and signatures made with its
key, but cannot identify a successor or its current key without receiving or resolving the chain.
When presented a normal successor, it can prove old-key continuity. A compromise successor proves
that the new key signed the declaration, but cannot prove continuity of control through the omitted
old-key signature, so it is accepted only by the decision described below.

## How a verifier obtains and trusts another person's chain

A signature proves only that some key signed. Attribution needs one more fact: that the key belongs
to the identity the record names. libliquid binds a name to a key in exactly one way, through that
identity's document chain (liquid#383).

- **The root of trust is the identifier itself.** A TAAP identifier is the fingerprint of the
  identity's initial key, so the initial document proves its own name: `verify_chain` and
  `verify_document` refuse an initial document whose `taap_id` and `fingerprint` are not derived
  from its own key (`identity_chain_identifier_not_derived_from_key`). Every later key is bound by a
  rotation signed by the key it replaces.
- **`verify(actor, artifact_type, body, signature)` reads the chain from this machine's identity
  store** (`$LIQUID_HOME/identity/<fingerprint>/documents/`), keyed by the name in `actor.taap_id`.
  The store holds the local identity's own chain and every chain accepted by
  `trust_identity_chain`. The actor's `public_key`, `fingerprint` and `key_fingerprint` are not
  trusted. **With no stored chain the result is the unknown-signer error, never `true`.** Its code
  is `errc::unauthorized`, so a caller that refuses every error stays closed;
  `is_unknown_signer(error)` lets a caller say "unknown signer" rather than "bad signature". A
  stored chain that does not contain the signing key gives `signing key is not in the identity
  chain`. The name is validated as `taap:1:<algorithm>:<52 base32 characters>` before it is used as
  a path.
- **`verify(actor, chain, artifact_type, body, signature)` verifies against a chain the caller
  supplies**, for example one carried with a request. It requires the chain to start at the
  initial document, to name the same identity as `actor.taap_id`, and every rotation to carry the
  replaced key's signature.
- **`trust_identity_chain(chain, options)` stores a verified chain.** A chain already stored may only
  be extended: a chain whose documents differ from the stored ones is a fork and is refused with
  `errc::conflict`, and a shorter prefix changes nothing. Rotations beyond what is already stored
  must be signed by the key they replace; a rotation that only declares the old key compromised is
  signed by its new key alone, so anyone could write one for any public initial document. It is
  accepted only with `accept_compromise_rotation`, an out-of-band decision that sync never makes.
  The local identity's own compromise rotations are already in its store.
- **`load_identity_chain(taap_id)` returns a stored chain**, and `encode_identity_chain` /
  `decode_identity_chain` carry one as a canonical CBOR array of complete documents, oldest first.
- **`verify_key_signature(key, ...)`** checks a signature against a key and nothing else. It is for a
  key the caller itself bound to a name, such as a session key it minted and stored; it is never a
  substitute for a chain.

### Every decision that trusts an identity requires a verified chain

Steve decided on 2026-11-14 (liquid#429, liquid#452): every decision that trusts an identity
requires a verified identity chain, and a signature verified only against the key a record carries
never satisfies one. The TAAP fingerprint is computed from the key *and* the identity document's
metadata (`created_at`, `key_label`, custody), which a record's actor does not carry, so the record
key alone can never be bound to the `taap_id` it claims. The decisions, and the chain-required
check each one uses:

| Decision | Where | Check |
|---|---|---|
| Approved and attestation issuers | `src/attestation/registry.cpp` | `verify_attestation`: `verified` is set only through a chain |
| The admission issuer pin | `src/admission/review.cpp` | `verify_attestation`; an unchained issuer is `attestation.unknown_signer` |
| Approved builders and policy counts | `src/build/policy.cpp` | `verify_attestation` |
| Distinct release signers | `src/identity/threshold.cpp` | `verify_attestation`; an unchained signer is listed in `unknown_signers` and not counted |
| Build provenance | `src/build/provenance.cpp` | `verify` and `verify_attestation` |
| Authors of imported drips and changes | `src/repository/repository_impl.cpp` (`import_synced_*`) | `verify`; an unchained author is refused |

A signature with no chain is reported, never trusted: **`check_signature(actor, ...)`** returns a
`signature_check` whose `identity_verified` is set only when a chain binds the name to the signing
key, exactly as `verify` requires. With no stored chain it falls back to the record's own key and
reports `signature_valid` with `identity_verified == false`: the record was *signed by an unverified
key* (`unverified_signer_label`). It is for display only. A known chain that lacks the signing key,
or a bad signature, is still an error.

`repository::drips()` and `repository::log()` set `drip::identity_verified` and
`change::identity_verified` this way, and `liquid log` (with or without `--drips`, text and JSON)
shows an unverified author as `signed by an unverified key`, never as the name the record claims.

**History signed before liquid#448** by an identity whose chain was never kept (for example by an
earlier lake pod's per-pod identity) is shown as signed by an unverified key and cannot satisfy any
policy. It is not migrated: Steve decided on 2026-10-23 that repository data so far is test data
and the affected repositories may be deleted (liquid#452).

A chain reaches a verifier in one of three ways:

| Path | How |
|---|---|
| **Sync** | Every exported drip and change author's stored chain travels as an `identity` frame ahead of the records it signs ([sync.md](sync.md#metadata-stream)). The receiver verifies it, stores it with `trust_identity_chain`, and only then checks the signatures. A sender relays every chain it holds, so a chain reaches a third machine; the repository also keeps each author's chain in its metadata, so a restarted process with a new home still sends it. An author whose chain the sender does not hold is sent without one, and the receiver refuses that author's records as an unknown signer. |
| **By hand** | `liquid identity show --chain` prints the whole chain as unpadded base64url; `liquid identity trust <chain>` (or `-` for standard input) verifies and stores it on another machine. This is how a release verifier, an admission server, a worker scheduler, or a build-policy checker learns the identities it should accept. |
| **With the request** | A service that receives a chain with each request, such as lake's TAAP sign-in, passes it to `verify(actor, chain, ...)` or `attestation_verification_options::issuer_chain`. |

Trusting a chain answers *who signed*, not *whether that signer may do this*. Who may issue a release
policy or a hot-update patch is a separate allow-list (liquid#384, liquid#388); build policy and
admission policy already name the identities they accept.

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
P-256 entirely in software. This additional signature-object reading is sent as protocol feedback, because TAAP §6.7 currently specifies only a direct signature byte string.

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
valid signature by a key in the named identity's stored chain, `false` for an unsigned record,
`errc::unauthorized` for invalid signed data, and the unknown-signer error (also
`errc::unauthorized`) when no chain for the name is stored. `verify_document` checks the
self-signature, and for an initial document that its identifier is derived from its key. Drips,
changes, results, and attestations use the same verification rules.

Verification against tests: `Identity.SignerNameWithoutAChainDoesNotVerify` signs with one key while
naming another identity and gets an unknown signer, then shows that trusting the named identity's
real chain does not make the other key verify. `Identity.InitialDocumentMustBeNamedAfterItsOwnKey`,
`Identity.ChainTravelsAndIsTrustedOnlyByExtension`, `Identity.ForkedChainIsRefused`,
`Identity.CompromiseOnlyRotationIsNotTrustedFromAnotherMachine`, and `Identity.SignerNameIsNotAPath`
cover the rules above; `Sync.drip_signed_by_one_key_naming_another_identity_is_refused` and
`Sync.author_identity_chain_travels_with_its_drips` cover sync. For liquid#429,
`Identity.SignatureWithoutAChainIsReportedAsAnUnverifiedKey` covers `check_signature`,
`Sync.change_signed_by_one_key_naming_another_identity_is_refused` covers a forged change author on
import, `Sync.history_without_its_authors_chain_is_listed_as_unverified` covers the listing, and
`ReleaseApproval.self_signed_approvals_naming_listed_signers_do_not_count` covers the release
threshold.

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
the protocol change that specifies them; delegation and session keys remain separate work.
