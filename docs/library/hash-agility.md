# Hash and signature agility

**Status: implemented for BLAKE3 and SHA-256 object identifiers, and Ed25519
signatures.** Every digest has an algorithm tag, so a store and its records can
carry BLAKE3 and SHA-256 identifiers at the same time. Every persisted drip and
change signature carries its signature-scheme tag.

## Digest identifiers

`liquid::digest` is the pair `(algorithm, bytes)`. Its text forms are
`blake3:<hex>`, `sha256:<hex>`, and private-store `blake3k:<hex>`. The algorithm
is encoded with every digest in object records, repository metadata, pack frames,
pack tables, and index manifests. A parser rejects an unrecognized text prefix
with `errc::unsupported` and the prefix in `error::context`.

BLAKE3 remains the default. Set `store_options::hash_algorithm` to `sha256` to
write SHA-256 object and chunk identifiers. The store keeps a separate index
memtable and immutable run set for each algorithm, allowing the two forms to
resolve side by side. Reopening a repository with the same
`repository_options::storage` preserves its selected write algorithm; the option
also reserves the repository boundary for future key custody configuration.
Change IDs reconstructed by `log` use each referenced change's stored algorithm and can be passed directly
to `at`. Repository directory creation uses it for both the placeholder object
reference and its operation identifier.

An application can rehash bytes from a BLAKE3 object, index the SHA-256 form by
writing those bytes through a SHA-256-configured store, and retain both object
identifiers. The two references resolve independently to the same bytes. This
does not rewrite existing references.

Sync pack receipt verifies bytes with the transmitted algorithm and preserves that identifier in
the receiving store. The receiving store's configured algorithm continues to control later local
writes; it does not rewrite a verified remote identifier.

The CRDT actor identifier is a fixed BLAKE3 derivation of the TAAP identifier and
device identifier. It is not a store digest or object reference, so selecting a
store algorithm does not alter CRDT actor identity.

The store's metadata-only reference extractor can decode a change without an
algorithm because it reads embedded references and discards the reconstructed
change ID. History traversal provides each referenced digest's algorithm when decoding its change ID,
including history imported by composition from a differently configured store.

## Signature identifiers

TAAP/1 signatures use the `signature_algorithm` field in their canonical CBOR
objects. Repositories write Ed25519 signatures for every drip and change. The
signature parser rejects an unrecognized scheme with `errc::unsupported` and
the scheme tag in `error::context`; encoding an unrecognized scheme remains
`errc::invalid_argument`.

## Not implemented

There is no migration command or automated repository rewrite. Rehashing is an
application-controlled process that adds alternate references while retaining
the original ones. ML-DSA support is not implemented; its tag is rejected until
an implementation and verification policy are added.

## Verification

`tests/test_store.cpp` exercises mixed BLAKE3 and SHA-256 objects before and
after reopening. `tests/test_repository.cpp` covers a SHA-256 repository round
trip, alternate-reference resolution after rehashing, and Ed25519 tags on
written drips and changes. `tests/test_hash.cpp` and `tests/test_repository.cpp`
cover rejection of unknown digest and signature tags.
