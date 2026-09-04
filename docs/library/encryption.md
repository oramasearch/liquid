# Encrypted store objects

**Status: implemented for liquid #7's store-level scope and #53's data-key rotation and
readability reporting.** The local store encrypts whole objects inside a trust domain, versions
the domain data key, retains held versions for historical reads, and reports the versions used by
repository revisions. Per-subtree policy, structured leaf-value encryption, and recipient-based
custody remain outside this implementation.

## What and why

Set `store_options::keys` to a shared `keyring` and add a provider under each trust-domain key id.
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

`passphrase_provider` derives a KEK using Argon2i. The generated salt and Argon2 parameters are
stored with the wrapped data key, allowing the same passphrase to reopen the domain. Defaults are
64 MiB, three passes, and one lane. `key_file_provider::open(path)` reads exactly 32 raw bytes.
On POSIX the file must have mode `0600`; other modes return `unauthorized` and include the observed
mode in `error::context`. Windows has no equivalent POSIX mode check; place raw key files under the
current user's profile directory and restrict access with its ACL.

Use `keyring::add`, `set_default_key_id`, and `default_key_id` to configure custody. The configured
id names a trust domain; persisted object metadata records the exact `key-id@version`. The first
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

`store::key_version(digest)` reads the persisted object map without decrypting the object. It
returns the trust-domain id, numeric version, and whether the attached keyring can unwrap that
version. `repository::readability(change_id?)` reports the sorted set of key versions used by the
revision's blob records and content chunks. With no id it walks every revision on the current
ref, newest first. A revision containing unchanged old blobs and newly written blobs therefore
names both versions.

There is no `src/cli/` command table in this revision. The `rotate`, `keys`, and `revoke` command
wiring and rendering of these library reports are assigned to #119. The optional `--rewrite`
operation is documentation only and is not implemented; rewriting history is separate work.
Signed encrypted OCI manifests also remain deferred because the artifact-emission pipeline they
must cover is not present in Phase 1; this implementation makes no `ocicrypt` interoperability
claim.

## On-disk format

New `keys/<key-id>.wrapped` files use version 2:

```text
LQKEY\x02\0\0 | salt[16] | argon2_memory_kib:u32le | argon2_iterations:u32le |
argon2_lanes:u32le | current_version:u64le | held_count:u32le | revoked_count:u32le |
held[held_count] { version:u64le | wrap_nonce[24] | wrap_tag[16] | encrypted_data_key[32] } |
revoked[revoked_count] { version:u64le }
```

Every held entry is independently wrapped by the current KEK. The wrapping key is BLAKE3
derive-key under `liquid store data key wrapping v1`; the data key is the XChaCha20-Poly1305
plaintext. Version 1 files are read as a single held data-key version `key-id@1` and migrate to
version 2 on rotation or rewrap. Legacy object metadata containing an unversioned id also resolves
to version 1. The pack format additions and object metadata layout are documented in
[store.md](store.md).

## How to verify

```sh
cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure
```

The suite covers passphrase derivation, key-file permissions, XChaCha20-Poly1305 known answers,
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
