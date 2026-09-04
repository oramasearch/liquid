# Secrets and encryption

> Research: [14-secrets-and-encryption](../research/14-secrets-and-encryption/)
>
> **Requirement:** store encrypted data — password- or key-protected — so secrets can
> live in the repository.

## What this does and does not give you

liquid makes a repository a sound **distribution channel for ciphertext**. It does
not make it a secrets manager, and the tool says so plainly rather than implying
otherwise.

Two facts drive the design:

1. **History is permanent and replication is the point.** Re-encrypting the current
   state does nothing about ciphertext in older changes. Rotating a file key does not
   un-disclose what was already disclosed.
2. **Every authorised endpoint is a plaintext endpoint.** Each laptop, CI runner and
   deploy target that can decrypt is a place the secret exists in the clear.

So the right scope is declarative configuration, bootstrap material, recovery data,
private source and assets, and artifacts that must be versioned. Live production
credentials should be *referenced* from the repository and minted at deploy time
([13](13-deployment-runtime.md)).

## Encryption below chunking, dedup above it

The technically interesting problem: randomised encryption gives semantic security
and destroys deduplication; deterministic encryption preserves dedup and leaks
equality — and convergent encryption enables the **confirmation-of-a-file attack**,
where an attacker who guesses the plaintext can test whether the store contains it.

liquid resolves it the way restic and Borg do — **deduplicate before encrypting, in
the trusted client**:

1. Chunk the plaintext with FastCDC.
2. Compute a **keyed** chunk identifier (a MAC under the repository key), so an
   outside storage server cannot compute identifiers for guessed plaintext.
3. Consult the encrypted local index.
4. If the chunk is already stored, reuse the existing ciphertext.
5. Otherwise encrypt with fresh randomness and store.

The result: full deduplication with randomised, semantically secure ciphertext and no
convergent-encryption oracle. Dedup is scoped to a trust domain rather than global,
which is the correct trade.

## Structured encryption for configuration

For YAML, JSON, TOML and `.env`, liquid encrypts **leaf values** while leaving keys
and structure in the clear — the SOPS model.

Diffs, reviews and merges keep working. `encrypted_regex`-style rules limit
encryption to the fields that need it.

Whole-file encryption is the fallback, not the default, because it destroys every
collaboration property liquid exists to provide.

## Envelope encryption

A random **data key** per trust domain, wrapped by a longer-lived **KEK**. The data
key derives independent chunk-identifier and chunk-encryption keys. Rotating the KEK
rewraps the small data key instead of re-encrypting gigabytes. Per-object data keys
would prevent chunks shared by different objects in the same trust domain from
deduplicating, so they are deliberately not used.

KEK custody is pluggable, covering both halves of "password protected or key
protected":

| Custody | Use |
|---|---|
| **Passphrase** (Argon2 / scrypt) | Individuals; the simple case |
| **age / X25519 recipients** | Small teams, offline |
| **Cloud KMS** (AWS, GCP, Azure) | Organisations; IAM-controlled and audited |
| **Vault Transit** | Encryption as a service |
| **Hardware tokens** (PIV, FIDO2 `hmac-secret`) | Keys that never leave the device |
| **TPM sealing** | Unattended machine-bound decryption |
| **Shamir threshold shares** | Root-key custody requiring a quorum |

## Per-subtree keys

The encryption boundary is a **declared subtree or a nested repository** — which is
also the natural sharing and access boundary ([02](02-version-control.md)). One
concept, three uses.

Each protected subtree has its own key, distributed or KMS-wrapped only to that
subtree's readers.

## Enforcement on the recorded bytes

git-crypt's own documentation warns that clean/smudge filters are not a security
boundary: a bad rule or an incompatible tool can stage plaintext.

liquid owns its client, so it **refuses to record plaintext at a path declared
encrypted**, and verifies stored content rather than working-tree content. Secret
scanning and push protection run on what is actually being written.

Local capture also runs the detector set inline before the drip journal append. Findings block by
default; repository path-and-detector rules and a literal same-line marker are the only exceptions.
Each accepted scan writes `scan_result/1`, while `repository::redact` commits tombstones and proves
the replaced blob digest is unreadable. The implementation and limits are in
[`../library/secret-scanning.md`](../library/secret-scanning.md).

## Signing and encryption together

- **Sign plaintext, then encrypt** for authored content, so recipients verify the
  original author after decrypting.
- **Sign the final encrypted artifact digest** for distribution — for encrypted OCI
  layers, `ocicrypt` changes the bytes and therefore the digest, so the signature
  must cover the final manifest ([10](10-supply-chain.md)).

## Honest failure modes, stated by the tool

When encryption is enabled, liquid says explicitly:

- Rotation does not rewrite history. `liquid rotate` reports **which revisions remain
  readable under which keys**.
- Revoking a collaborator prevents future access; it does not recall what they have.
  Rotate the underlying credential, not just the file key.
- Metadata — paths, sizes, change structure and timing — remains visible unless
  separately protected.

## Never fail silently

If content cannot be decrypted, that is a clear error. liquid never presents a
working tree containing ciphertext or pointer files — the same rule as large files
([01](01-object-model-and-storage.md)).

## Open questions

- **Encrypted subtrees and the CRDT metadata layer.** The namespace document is
  shared; how much of it can be encrypted per-subtree without breaking merge
  ([17](17-implementation.md))?
- **Encrypted content and search.** Indexing requires plaintext. Client-side indexing
  with an encrypted index is possible and expensive ([12](12-search.md)).
- **Key distribution UX for non-technical users** ([15](15-consumer-sync-and-web.md)).
