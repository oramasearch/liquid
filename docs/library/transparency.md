# Transparency-log verification

`liquid::transparency` verifies a Lake transparency-log response before a caller relies on
it. The public API is coroutine-native: `log_source` permits an application or test to provide
the checkpoint, tiles, and proofs, while `http_log_source` obtains the Lake HTTP representation
with liquid's Capy and Corosio client. No Lake library is linked.

## Verification rules

The client validates the checkpoint's SHA-256 of its compact canonical JSON body, then its
Ed25519 Lake signature over `TAAP/1 lake_checkpoint/1`, a NUL byte, and the CBOR checkpoint
body. It accepts only configured witnesses with valid signatures over that canonical JSON body.
Duplicate witness keys never count twice; a checkpoint below the configured distinct-witness
threshold fails as `checkpoint witness threshold not met`.

Entry tiles use `LAKE-TILE-1\n` followed by nonzero big-endian lengths and canonical TAAP
attestation bytes. Hash tiles use `LAKE-HASH-TILE-1\n` followed by 32-byte hashes. Every tile
hash is checked, each leaf is recomputed as `SHA256(0x00 || entry)`, and each parent is
recomputed as `SHA256(0x01 || left || right)`. The client fetches and compares all available
parent tiles before independently calculating the RFC 6962-style split-tree root. A valid leaf
tile paired with an altered parent tile is therefore refused.

An inclusion proof must name the selected complete checkpoint, leaf index, tree size, and root.
It is calculated against the exact canonical TAAP bytes supplied by the caller. A consistency
proof must name both the stored root and the new complete checkpoint and establish append-only
extension before trust advances. An identity scan validates all tiles to the current checkpoint,
requires canonical TAAP encoding for every entry, and returns every entry whose issuer or text
attribute names the requested identity.

## Trusted checkpoint state and bootstrap

The most recent accepted `(sequence, tree_size, root_hash, checkpoint_hash)` is stored as a
canonical-CBOR repository metadata record under a SHA-256-derived log name. It is not a second
database. On an existing record, `verify_consistency` obtains and validates Lake's proof before
overwriting the record. `verify_inclusion` refuses a different checkpoint unless that extension
has first been proven.

For first use, an operator should independently pin the intended checkpoint and call the API
with `bootstrap_policy::pinned`; callers that instead select `accept_with_warning` receive
`checkpoint_trust::bootstrap_warning = true`, which must be recorded or shown to the user.
The CLI uses the warning policy so that a first verification is visible in its output.

## CLI

`liquid log verify <log-url> <attestation-base64url>` verifies exact inclusion and records the
checkpoint. `liquid log consistency <log-url>` advances an existing trusted record only through
a proof. `liquid log scan <log-url> <identity>` returns canonical attestations naming that
identity. Existing `liquid log [--json]` history output remains unchanged. `liquid verify --log
<log-url>` additionally requires inclusion for every agreeing reproducibility attestation.

## Failure meaning

| Failure | Meaning |
|---|---|
| `checkpoint log signature is invalid` | The log identity did not sign the checkpoint. |
| `checkpoint witness threshold not met` | Too few distinct configured witnesses signed it. |
| `tile hash does not match its bytes` | A received tile was modified or substituted. |
| `parent hash tile does not match children` | A complete-subtree parent is inconsistent with verified children. |
| `inclusion proof does not verify` | The attestation, index, path, or root was forged or changed. |
| `consistency proof does not verify` | The claimed extension is not append-only. |
| `attestation is not canonical TAAP` | A log entry cannot be decoded and re-encoded identically. |

## Conformance fixture

`tests/fixtures/transparency/fixture.json` is copied from Lake commit `cb60ed4` under
`tests/fixtures/transparency/`. It is generated in Lake by
`tests/fixtures/transparency/regenerate.py`; rerun that script on Lake main and copy the
result when Lake intentionally changes the wire format. The liquid tests exercise its signed
checkpoint, entry and hash tiles, inclusion paths, and consistency path, then mutate the tiles
and proof paths to cover missing, reordered, substituted, and parent-only corruption.
