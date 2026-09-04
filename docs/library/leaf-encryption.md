# Structured configuration encryption

**Status: implemented for liquid #52's Phase 1 scope.** JSON and `.env` files can encrypt
selected leaf values while retaining their keys and containers. YAML and TOML remain deferred to
the format registry in liquid #29; whole-file encryption is available when structure cannot remain
visible.

## What and why

Repository configuration declares encrypted paths in `repository_options::encrypt.paths`. The
configuration is copied into the repository document at creation and is authoritative when the
repository is reopened. Each ordered rule contains:

```cpp
encryption_path_rule{
    .path = "config/**.json",
    .key_id = "team",
    .mode = encryption_mode::leaf,
    .encrypted_regex = "(password|token)$",
};
```

`path` is a repository-relative glob. `*` and `?` do not cross `/`; `**` does. The first matching
rule wins. In leaf mode, `encrypted_regex` is an ECMAScript regular expression searched against
each full key path. Key paths use JSON Pointer spelling: `/database/password`, `/servers/0/token`,
and `/API_TOKEN` for `.env`. `~` and `/` in a JSON object key are escaped as `~0` and `~1`.

On `repository::write` or `repository::write_file`, matching plaintext leaves are encrypted before
any blob or chunk record is stored. A missing declared key returns `errc::unauthorized`, and
validation rejects a result that still has plaintext at a selected key. Existing valid envelopes
are retained. When a caller writes a decrypted revision back, liquid decrypts the prior revision
long enough to retain the ciphertext of matching leaves whose values did not change. Fresh
randomness therefore marks changed values, not every selected value, in a structural comparison.
File writes to declared paths are buffered because leaf parsing and the whole-file envelope require
the complete input; unmatched paths retain the bounded-memory streaming path.

`repository::read` returns the stored configuration document with ciphertext values. It never
decrypts leaf values implicitly. `repository::read_decrypted` is the explicit plaintext operation.
A repository opened without the keyring can still read and structurally inspect a leaf-encrypted
file, but `read_decrypted` returns `errc::unauthorized`.

## Envelope and authentication

Every selected leaf becomes a JSON string or `.env` value with this spelling:

```text
ENC[liquid:v1:<key_id>@<version>:<base64 nonce+tag+ciphertext>]
```

The nonce is 24 fresh bytes from the operating-system CSPRNG and the tag is 16 bytes.
XChaCha20-Poly1305 uses a key derived from the trust-domain data key with the context
`liquid configuration leaf encryption v1`. The JSON Pointer key path is associated data. Copying
an envelope from one key path to another therefore fails authentication. The versioned key id in
the envelope lets reads resolve the exact data key after rotation. New or changed leaves use the
current write version; retained leaves keep their original version. Legacy unversioned ids resolve
as version 1. A leaf-envelope key id cannot contain `:` or `]`.

JSON scalar type is inside the authenticated ciphertext. Strings, numbers, booleans, and null
round-trip with their original type; number spelling is retained. Arrays and objects stay in the
clear and are traversed to their leaves.

## Format behavior

The codecs are small in-tree implementations and take no parser dependency.

- JSON parsing is strict: invalid UTF-8, trailing input, duplicate keys, invalid number forms,
  invalid escapes, and unpaired UTF-16 surrogates are rejected. Object insertion order and number
  spelling are retained. Writing normalizes to two-space indentation, one object member or array
  item per line, a space after `:`, no insignificant trailing whitespace, and one final newline.
- `.env` parsing retains comment and blank lines, assignment order, optional `export`, and the
  exact value bytes after `=`. Writing normalizes line endings to LF, removes whitespace around
  assignment keys and `=`, retains value bytes and comment text, and emits one final newline.
  Duplicate or invalid keys are rejected. Variable interpolation is not performed.

Leaf mode accepts paths ending in `.json` or `.env`. Other formats return `errc::unsupported`
instead of risking plaintext storage.

## Structural key diff

`repository::diff_keys(path, change_a, change_b)` loads the two encrypted historical blobs, parses
their visible structure, validates selected leaves as envelopes, and compares leaf representations
without opening any envelope. It returns sorted JSON Pointer paths for added, removed, or changed
leaves. Comments in `.env` are not keys and do not appear in the result.

For `mode: file`, liquid first wraps the complete byte stream in the same versioned envelope and
then writes its chunks through the store's explicit trust-domain encrypted path. This keeps pack
plaintext out of the store but intentionally gives up structural review. `diff_keys` can only name
the repository path when two file-mode versions differ.

## Scope and deferred work

YAML and TOML need the shared format registry from liquid #29 so this implementation does not add
temporary parser or dispatch APIs. Rotation does not rewrite retained leaf envelopes; rewriting
stored values remains separate work. Recipient distribution and keys below a declared path remain
outside this Phase 1 implementation.

## Verification

```sh
cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure
cmake -S . -B build-shared -G Ninja -DBUILD_SHARED_LIBS=ON && cmake --build build-shared && ctest --test-dir build-shared --output-on-failure
```

The focused cases cover strict JSON and `.env` round trips, nested objects and arrays, `.env`
comments, selected-leaf encryption, fresh envelopes, associated-data failure after moving a value,
plaintext enforcement, explicit decryption, mixed-version reads after rotation, historical
key-only diff, persisted rules, memory and file write paths, and the whole-file fallback.
