# Encrypted store objects

**Status: implemented for liquid #7's store-level scope, #51's pluggable KEK custody, #53's
data-key rotation and readability reporting, and #52's JSON/`.env` leaf scope.** The local store encrypts whole
objects inside a trust domain, versions the domain data key, retains held versions for historical
reads, and reports the versions used by repository revisions. Repository path rules and structured
leaf-value encryption are described in [leaf-encryption.md](leaf-encryption.md).

## What and why

Set `store_options::keys` to a shared `keyring` and add a provider under each trust-domain key id,
or fill `store_options::custody` with key-id to custody-URI mappings.
`write_key` selects the domain for new data; when it is empty, the keyring default is used. Each
domain starts with one random 32-byte data key. Its KEK is held by the provider and wraps every
held version of the data key; each version derives different BLAKE3 keys for keyed chunk
identifiers and chunk encryption.

This scopes deduplication correctly. Equal chunks in one domain have the same `blake3k:<hex>` id
and reuse one randomized ciphertext. Equal chunks under distinct key ids have distinct ids and
are stored separately, so a storage server cannot confirm a guessed file. Every newly stored
chunk receives a fresh 24-byte nonce from the operating-system CSPRNG and uses
XChaCha20-Poly1305. Plaintext is compressed before encryption and never written to a pack.

## Public API

`key_provider` exposes asynchronous `wrap(data_key, wrap_context)` and
`unwrap(wrapped, wrap_context)` operations plus `custody_kind()`. `wrap_context` carries the
key id and version, and every provider authenticates or transmits that reference. Provider
configuration persisted in v3 is non-secret; credentials come only from the process environment.

`passphrase_provider` derives a KEK using Argon2i. Each wrapped entry carries its salt and Argon2
parameters, allowing the same passphrase to reopen the domain. Defaults are
64 MiB, three passes, and one lane. `key_file_provider::open(path)` reads exactly 32 raw bytes.
On POSIX the file must have mode `0600`; other modes return `unauthorized` and include the observed
mode in `error::context`. Windows has no equivalent POSIX mode check; place raw key files under the
current user's profile directory and restrict access with its ACL.

Use `keyring::add`, `set_default_key_id`, and `default_key_id` to configure custody, or use
`key_provider_from_uri` with one of these forms:

```text
passphrase:
file:<path>
age:<recipient>[,<recipient>...]
aws-kms:<arn>
gcp-kms:<resource>
azure-kv:<key-version-url>
lake:<base-url>/<key>
shamir:<threshold>/<share-count>
tpm:pcrs=0,7
```

Passphrases, age identities, cloud bearer credentials, AWS credentials, lake tokens, and Shamir
shares are read from `LIQUID_PASSPHRASE`, `LIQUID_AGE_IDENTITIES`, the cloud-standard variables
documented in the matrix below, `LIQUID_TOKEN`, and `LIQUID_SHAMIR_SHARES`. They are neither stored
nor logged. Use `keyring::open(keys_path)` to reconstruct v3 providers from their persisted public
parameters; providers whose environment credential is unavailable remain inspectable through
`keyring::show` but cannot unwrap.

The configured id names a trust domain; persisted object metadata records the exact
`key-id@version`. The first
memory or streaming-file write creates version 1. `keyring::rotate(key_id)` authenticates the current KEK, creates the next
data-key version, makes it the write version, and returns a `rotation_report`. Existing objects and
packs are untouched. A wrong KEK fails before the wrapped-key file is replaced.

`keyring::revoke(key_id, version)` removes that version's wrapped key and records the version as
revoked. Reads of its objects then return `unauthorized` with the exact `key-id@version`; newer
versions remain readable. Revocation is idempotent once recorded. The keyring does not cache
plaintext data keys: each unwrap is consumed and wiped by the caller before the operation returns.

`keyring::rewrap(key_id, new_provider)` changes only `keys/<key-id>.wrapped`: it authenticates and
unwraps every held version with the old provider, wraps every version with the new provider, wipes
the plaintext versions, and atomically renames the replacement file. Pack bytes do not change.
KEK rewrapping and data-key rotation are separate operations.

### Custody matrix

| Kind | Trust model | Environment input | Failure behavior |
|---|---|---|---|
| Passphrase | The operator supplies a high-entropy secret; Argon2i raises offline-guess cost. | `LIQUID_PASSPHRASE` | A missing or wrong passphrase returns `unauthorized` for `key-id@version`. |
| Raw key file | Filesystem permissions protect exactly 32 KEK bytes. | The URI path; key bytes are never copied into metadata. | Missing, non-regular, incorrectly sized, or non-`0600` POSIX files fail closed. |
| age / X25519 | Any listed age v1 recipient can decrypt; identities can remain offline. | `LIQUID_AGE_IDENTITIES`, comma or newline separated | No matching identity, malformed age data, or failed authentication returns `unauthorized`. |
| AWS KMS | AWS IAM and KMS policy control `Encrypt`/`Decrypt`; CloudTrail can audit calls. | `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, optional `AWS_SESSION_TOKEN` | Credentials are SigV4-signed for the ARN region; credential, transport, HTTP, and response errors fail closed. |
| GCP KMS | Google IAM controls the named CryptoKey and its versions; Cloud Audit Logs can record calls. | `GOOGLE_OAUTH_ACCESS_TOKEN` | The bearer call to `cryptoKeyVersions:encrypt` or `:decrypt` must succeed and return valid Base64. |
| Azure Key Vault | Azure RBAC or access policy controls the key version. | `AZURE_ACCESS_TOKEN` | `wrapkey`/`unwrapkey` uses `RSA-OAEP-256`; bearer, HTTP, and response errors fail closed. |
| lake transit | lake authorizes a named remote custody key; liquid never receives lake's KEK. | `LIQUID_TOKEN` | The lake-native wrap or unwrap route must return an authenticated result; there is no Vault-compatible surface. |
| Shamir | Any threshold of independently held shares reconstructs the random 32-byte KEK. | `LIQUID_SHAMIR_SHARES`, comma or newline separated direct shares or share-file paths; prefix a path with `@` to distinguish it explicitly | Bad checksums, incompatible shares, unreadable files, and fewer than the threshold return `unauthorized` for `key-id@version`. |
| TPM 2.0 sealing | The data key is bound to the local TPM and selected SHA-256 PCR state. Firmware, boot policy, and measured components extending those PCRs are trusted. | `tpm:pcrs=<comma-separated 0..23>`; `/dev/tpmrm0` on Linux or TBS on Windows | A missing TPM, changed selected PCR, malformed sealed object, or failed policy or authorization session refuses unwrap. macOS returns `unsupported` because Secure Enclave is a different device. |

Age wraps a fresh 16-byte file key for every recipient with X25519, HKDF-SHA-256, and RFC
ChaCha20-Poly1305, authenticates the age v1 header, and encrypts the liquid data key with the age
STREAM payload construction. The implementation uses Monocypher X25519 and the tree's SHA-256; it
does not invoke an `age` executable.

Shamir shares are `liquid-share-1-<index>-<threshold>-<material>-<checksum>`. The material is one
GF(256) evaluation of the 32-byte KEK and the checksum is the first four SHA-256 bytes of the
preceding text. `split_custody(k, n)` returns the provider and the `n` text shares; shares are the
only output that can reconstruct that provider's KEK.

`store::key_version(digest)` reads the persisted object map without decrypting the object. It
returns the trust-domain id, numeric version, and whether the attached keyring can unwrap that
version. `repository::readability(change_id?)` reports the sorted set of key versions used by the
revision's blob records and content chunks. With no id it walks every revision on the current
ref, newest first. A revision containing unchanged old blobs and newly written blobs therefore
names both versions.

`liquid keys show [key-id]` lists held and revoked versions. `rewrap <key-id> --to <custody-uri>`
changes custody, `rotate <key-id>` prints the version transition and the three disclosure
statements, `revoke <key-id> <version>` removes one local wrapped version, and
`split <key-id> --threshold <k> --shares <n>` rewraps under a new Shamir KEK and prints its shares.
The optional `--rewrite`
operation is not implemented; rewriting history is separate work.
Signed encrypted OCI manifests also remain deferred because the artifact-emission pipeline they
must cover is not present in Phase 1; this implementation makes no `ocicrypt` interoperability
claim.

TPM wrapping creates a TPM 2.0 sealed object under a PCR policy and stores only its public/private
object blobs in the v3 wrapped value. Unwrap loads the object, applies `TPM2_PolicyPCR` in a policy
session, and calls `TPM2_Unseal`. Linux uses the resource-manager device and Windows uses TBS. All
device calls run through `runtime::offload`; liquid owns the bounded command marshalling layer and
does not link tpm2-tss. The in-process transport double pins command framing and changed-PCR
refusal; the Linux verification image includes `swtpm` for its device lane.

## On-disk format

New `keys/<key-id>.wrapped` files use version 3:

```text
LQKEY\x03\0\0 | current_version:u64le | held_count:u32le | revoked_count:u32le |
held[held_count] { version:u64le | custody_length:u16le | parameters_length:u32le |
wrapped_length:u32le | custody[custody_length] | parameters[parameters_length] |
wrapped[wrapped_length] } |
revoked[revoked_count] { version:u64le }
```

Every held entry is independently wrapped by its named custody kind. Parameters contain only the
provider's public URI suffix: recipient list, key resource, endpoint/key name, threshold/count, or
key-file path. Passphrase entries keep their random salt and Argon2 values inside `wrapped`.
Version 1 files are read as a single held data-key version `key-id@1`; version 2 keeps its version
table. Both legacy formats use BLAKE3 derive-key under `liquid store data key wrapping v1` and
XChaCha20-Poly1305, and migrate to version 3 on rotation or rewrap. Revoke does not promote a
legacy file to v3; revoking from v1 writes v2 because v1 has no revocation table. Legacy object
metadata containing an unversioned id resolves to version 1. The pack format additions and object
metadata layout are documented in
[store.md](store.md).

### lake transit wire form

lake custody is lake-native. Liquid sends `Authorization: Bearer <LIQUID_TOKEN>` and canonical JSON:

```text
POST <lake>/v1/custody/<key>/wrap
{"key_id":"<liquid-key-id>","plaintext":"<base64>","version":<number>}

POST <lake>/v1/custody/<key>/unwrap
{"key_id":"<liquid-key-id>","version":<number>,"wrapped":"<base64>"}
```

Successful replies are `{"wrapped":"<base64>"}` and `{"plaintext":"<base64>"}` respectively.
lake implements these routes separately; liquid supplies the client contract and test double.

## How to verify

```sh
cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure
```

The suite covers passphrase derivation, key-file permissions, XChaCha20-Poly1305 known answers,
age specification vectors, Shamir thresholds, TPM seal/unseal and PCR refusal, signed and bearer-authenticated in-process KMS and
lake doubles, every ordered custody-kind rewrap pair with exact pack-byte comparison, all three
wrapped-key formats, the `keys` CLI,
encrypted deduplication, cross-domain duplicate storage, plaintext exclusion from packs, missing
and wrong keys, corruption, range reads, KEK rewrapping, version rotation without a pack rewrite,
old and new reads, revocation, wrong-KEK atomicity, and exact repository readability reports.

## Honest failures

- A missing domain key, a missing or revoked wrapped data-key version, or a wrong KEK produces
  `unauthorized` with the exact `key-id@version` in `error::context` when a version is known.
- Damaged wrapped-key metadata produces `corrupt`; a wrapped data key that cannot authenticate
  with the supplied provider produces `unauthorized`.
- Damaged ciphertext, an invalid tag, decompression failure, or a keyed-id mismatch produces
  `corrupt`. No ciphertext is returned as content.
- Rotation does not rewrite history.
- Revoking a collaborator prevents future access; it does not recall what they have.
- Metadata — paths, sizes, change structure and timing — remains visible unless separately
  protected.

The `rotation_report` and `readability_report` carry these three statements for clients to render.
Rotation and rewrap never rewrite existing chunks. Revocation removes local wrapped-key custody;
it cannot erase copies of plaintext or data keys already held elsewhere.

## How to change

Changing the on-disk envelope needs a new marker or versioned record, compatibility tests, and an
update here and in [store.md](store.md). Keep parsing and formatting `key-id@version` in
`src/identity/key_version.hpp` so store encryption and structured leaf encryption resolve versions
identically. Do not replace the operating-system random source with a counter or deterministic
input. Add new custody providers by implementing `key_provider`; keep their public surface free of
vendor SDK types.
