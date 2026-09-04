# Encrypted store objects

**Status: implemented for liquid #7's store-level scope.** The local store encrypts whole objects
inside a trust domain. Per-subtree policy, structured leaf-value encryption, recipient-based
custody, and the `liquid rotate` command are outside this implementation.

## What and why

Set `store_options::keys` to a shared `keyring` and add a provider under each trust-domain key id.
`write_key` selects the domain for new data; when it is empty, the keyring default is used. Each
domain has one random 32-byte data key. Its KEK is held by the provider and wraps the data key;
the data key derives different BLAKE3 keys for keyed chunk identifiers and chunk encryption.

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

Use `keyring::add`, `set_default_key_id`, and `default_key_id` to configure custody. After a store
has attached the keyring, `keyring::rewrap(key_id, new_provider)` changes only
`keys/<key-id>.wrapped`: it unwraps the unchanged data key with the old provider, wraps it with the
new provider, and atomically renames the replacement file. Pack bytes do not change. Replacing the
data key itself is `liquid rotate` work, not KEK rewrapping.

## On-disk format

`keys/<key-id>.wrapped` is exactly:

```text
LQKEY\x01\0\0 | salt[16] | argon2_memory_kib:u32le | argon2_iterations:u32le |
argon2_lanes:u32le | wrap_nonce[24] | wrap_tag[16] | encrypted_data_key[32]
```

The wrapping key is BLAKE3 derive-key under `liquid store data key wrapping v1`; the data key is
the XChaCha20-Poly1305 plaintext. The pack format additions and `LQOBJ2` metadata layout are
documented in [store.md](store.md).

## How to verify

```sh
cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure
```

The suite covers passphrase derivation, key-file permissions, XChaCha20-Poly1305 known answers,
encrypted deduplication, cross-domain duplicate storage, plaintext exclusion from packs, missing
and wrong keys, corruption, range reads, and KEK rewrapping without a pack rewrite.

## Honest failures

- A missing domain key, a missing wrapped data key, or a wrong KEK produces `unauthorized` with
  the key id in `error::context`.
- Damaged wrapped-key metadata produces `corrupt`; a wrapped data key that cannot authenticate
  with the supplied provider produces `unauthorized`.
- Damaged ciphertext, an invalid tag, decompression failure, or a keyed-id mismatch produces
  `corrupt`. No ciphertext is returned as content.
- KEK rewrapping does not rewrite history or revoke already disclosed plaintext. Metadata such as
  key ids, sizes, object structure, and write timing remains visible.

## How to change

Changing the on-disk envelope needs a new marker or versioned record, compatibility tests, and an
update here and in [store.md](store.md). Do not replace the operating-system random source with a
counter or deterministic input. Add new custody providers by implementing `key_provider`; keep
their public surface free of vendor SDK types.
