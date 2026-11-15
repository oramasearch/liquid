# TUF metadata verification

Liquid issue #186 adds a repository-backed TUF 1.0 verifier for metadata served by Lake. It is a
trust gate for registry and drive clients: a caller may use a target only after the complete target
bytes pass `liquid::tuf::client::verify_target`. Fetching registry metadata or artifacts remains the
calling client's responsibility.

## Trust bootstrap

Trust begins with a root JSON file supplied out of band. `client::pin` verifies the root's type,
version, expiry, key identifiers, role definitions, Ed25519 signatures, and its own root threshold
before storing it. The exact root bytes and their SHA-256 digest are retained in repository metadata
under `config.tuf`; `client::trusted` returns all three values so an operator can compare the pin
with the value distributed through the trusted channel.

```cpp
auto pinned = co_await liquid::tuf::client::pin(repository, std::move(root_json));
if (!pinned) {
    co_return std::unexpected(pinned.error());
}
```

Pinning replaces the repository's existing TUF state. It is therefore an explicit operator action,
not part of ordinary metadata refresh. `config.tuf` is never exported by sync, so a clone or pull
starts with no pinned root and a remote repository cannot replace one (liquid #420,
[sync.md](sync.md#trust-configuration-stays-with-the-checkout-that-set-it)).

The command-line forms operate on the repository found from the current directory:

```text
liquid registry trust pin root.json
liquid registry trust show
```

`pin` prints the accepted version and SHA-256 digest. `show` also prints the exact pinned JSON.

## Update order and rollback state

Open a client after pinning, supply the fetched metadata as exact response bytes, and keep the
numbered root files in ascending order:

```cpp
auto verifier = co_await liquid::tuf::client::open(repository);
if (!verifier) {
    co_return std::unexpected(verifier.error());
}
auto update = co_await verifier->update({
    .roots = std::move(numbered_roots),
    .timestamp = std::move(timestamp_json),
    .snapshot = std::move(snapshot_json),
    .targets = std::move(targets_json),
});
if (!update) {
    co_return std::unexpected(update.error());
}
```

The verifier performs the TUF workflow in this order:

1. Each new root must be exactly one version newer than the preceding trusted root. Its signatures
   must satisfy both the old root role and the new root role. A key identifier contributes at most
   once to either threshold, even if its signature is repeated. Intermediate expiry does not block
   recovery; the final trusted root must be current before the remaining roles are considered.
2. Timestamp, snapshot, and targets must have a version greater than the corresponding persisted
   version, must not be expired at the verification time, and must satisfy their role threshold.
3. Timestamp binds the exact snapshot bytes by version, length, and SHA-256. Snapshot applies the
   same checks to the exact targets bytes.
4. A fully verified root chain is stored before the remaining roles are considered, so a later
   failure cannot restore compromised keys. Timestamp, snapshot, and targets counters are stored
   together only after all three roles pass; a failure leaves those counters unchanged.

The overloads taking a `std::chrono::sys_seconds` are for deterministic verification and tests.
Normal callers use the overloads that read the current system time.

## Canonical JSON and signatures

Signatures cover the canonical JSON encoding of the `signed` object, not the enclosing metadata
bytes. Liquid accepts the TUF integer-only JSON domain, rejects malformed UTF-8 and duplicate object
keys, orders object members lexicographically, and emits compact JSON with the TUF string escaping
rules. This matches Lake's metadata producer. Floating-point values and integers outside the signed
or unsigned 64-bit range are rejected.

Root key identifiers are checked against the SHA-256 of each canonical TUF key object. Top-level
roles must use Ed25519 keys in the TUF `ed25519`/`ed25519` form. Signature verification uses the
repository's pinned Monocypher Ed25519 implementation; no external trust-path library is exposed or
used.

This implementation supports the four top-level TUF roles and SHA-256 bindings produced by Lake.
Delegated targets roles and alternate hash or signature algorithms are rejected as unsupported.

## Target acceptance gate

`update_result::targets` contains the trusted target paths, exact byte lengths, and SHA-256 digests.
The client retains the same descriptions for lookup. A registry or drive client must finish the
download and verify all bytes before publication or use:

```cpp
auto verified = verifier->verify_target(target_path, complete_bytes);
if (!verified) {
    co_return std::unexpected(verified.error());
}
```

Paths are relative and cannot contain empty, `.` or `..` components or backslashes. Partial data is
never accepted: the exact length is checked before SHA-256.

## Fixtures and attack coverage

`tests/fixtures/tuf/` contains one metadata generation from Lake's real TUF producer plus its target.
Its README records the Lake revision, generation procedure, exact sizes, and SHA-256 values. The test
suite also constructs attacks independently to prove rejection of repeated signatures, an expired
timestamp, metadata rollback and mixed versions, altered bound bytes, skipped root versions, roots
meeting only one of the old/new thresholds, and replacement by a compromised old key.
