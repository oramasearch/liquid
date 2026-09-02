# Secrets and encryption

> Evidence: [`sources.md`](sources.md)
>
> **Requirement:** liquid should store encrypted data — password- or key-protected
> — so secrets can live in the repository.

## The honest framing

Encryption can make a repository a reasonable **distribution channel for
ciphertext**. It does not turn the repository into a secrets manager. Two facts
drive everything below:

1. **History is forever, and replication is the point.** Anyone who ever had
   plaintext or a decryption key retains it. Re-encrypting the current state does
   nothing about the ciphertext in older commits, and rotating a file-encryption
   key does not un-disclose what was already disclosed.
2. **Every authorised endpoint is a plaintext endpoint.** Each laptop, CI runner
   and deploy target that can decrypt is a place the secret exists in the clear.

So the correct scope for in-repository encryption is: declarative configuration,
bootstrap material, recovery data, private source or assets, and artifacts that
genuinely must be versioned. Live production credentials should be *referenced*
from the repository and *minted* at deploy time.

That is not a reason to skip the feature — it is a reason to be precise about what
it guarantees.

## What existing tools teach

**git-crypt** — worth correcting a common misconception: it uses AES-256-**CTR**
with a synthetic IV derived from HMAC-SHA-1 of the plaintext, *not* standardised
AES-SIV. It is deterministic, so identical plaintext gives identical ciphertext.
But it does **not** give useful deltas: changing the plaintext changes the
synthetic IV, which changes the entire ciphertext, so Git stores another full blob.
Its own documentation warns that clean/smudge filters are not a security boundary —
a bad `.gitattributes` rule or an incompatible GUI can stage plaintext.

**SOPS** is the strongest existing design for structured config. It encrypts
*leaf values* while preserving keys and structure, so diffs and reviews remain
meaningful, and wraps a random per-file data key to age, PGP, AWS/GCP/Azure KMS, or
Vault Transit recipients. `encrypted_regex` limits encryption to the fields that
need it. **This is the model for liquid's structured-config case.**

**age** — X25519 recipients, fresh file key and ephemeral secret per file,
HKDF-SHA-256 with chunked ChaCha20-Poly1305; scrypt for passphrases. Randomised, so
re-encrypting identical plaintext gives different ciphertext — which makes it a bad
naive clean filter but a good file encryptor. It displaced GPG for most file
encryption because it has small explicit keys, no global trust database, and modern
fixed constructions.

**Sealed Secrets** — asymmetric one-way delivery: anyone can encrypt with the
cluster's public cert, only the in-cluster controller can decrypt. Even the author
cannot read it back. Relevant to liquid's deployment story
([20](../20-deployment-runtime-k8s/)).

## The determinism vs dedup tension

This is the technically interesting problem, and it matters more for liquid than
for Git because liquid's whole storage model is content-defined chunking.

- **Deterministic encryption** allows equality testing and therefore dedup — but it
  leaks equality, and for low-entropy content (booleans, short status values, known
  templates) it enables guessing attacks. **Convergent encryption** — deriving the
  key from the plaintext — enables cross-user dedup and is vulnerable to the
  **confirmation-of-a-file attack**: an attacker who can guess the plaintext
  computes the convergent result and tests whether the store contains it.
- **Randomised encryption** is semantically secure and destroys ciphertext-level
  dedup entirely.

**restic and Borg show the resolution, and it is the right answer for liquid:**
deduplicate *before* encryption, in the trusted client.

1. Chunk the plaintext with CDC.
2. Compute a **keyed** chunk identifier (HMAC/MAC with the repository key), so an
   outside storage server cannot compute identifiers for guessed plaintext.
3. Consult an encrypted local index.
4. If the chunk is already stored, reuse the existing ciphertext.
5. Otherwise encrypt with fresh randomness and store.

This gives repository-scoped dedup with randomised, semantically secure ciphertext,
and no convergent-encryption exposure. It does mean dedup is scoped to a trust
domain rather than global — which is the correct trade.

## Envelope encryption and key management

The standard construction: a random **DEK** per file/object/chunk encrypts the
data; a longer-lived **KEK** wraps the DEK; store `{ciphertext, wrapped DEK,
metadata}` together. Rotating the KEK rewraps a small key instead of re-encrypting
gigabytes.

KEK custody options, all supported by SOPS-style tooling: AWS/GCP/Azure KMS
(IAM-controlled, audited, key material never leaves the boundary), Vault Transit,
Shamir threshold sharing for root keys, YubiKey PIV (private key never exported),
FIDO2 `hmac-secret`/WebAuthn PRF (a credential-scoped derived secret), and TPM
sealing to PCR state for unattended machine-bound decryption.

**The historical catch:** if the repository retains an earlier wrapped DEK and the
old KEK still works — or a former collaborator cached the unwrapped DEK — the old
revision remains decryptable. Rotation must be explicit about whether history stays
readable.

## Per-subtree access control

Options, in increasing order of ambition: independent per-subtree keys; hierarchical
per-directory keys where child keys are wrapped by parents (easy downward
delegation, expensive selective revocation — Linux `fscrypt` works this way);
Keybase KBFS's top-level-folder model (rotates keys for *future* data only, and
explicitly does not re-encrypt old data). Tahoe-LAFS uses capabilities, where
possession *is* authority and revocation means creating replacements.

The recurring truth: **cryptography withholds plaintext, not metadata.** Paths,
sizes, commit structure and change timing remain visible unless separately hidden.

## Signing and encryption together

- **Sign plaintext then encrypt** — recipients verify the original author after
  decrypting; outsiders cannot verify without decrypting. Right for authored
  content.
- **Encrypt then sign the ciphertext** — anyone can authenticate the envelope
  without decrypting, but the signature does not prove plaintext authorship.
- **Both** — strongest when packager and author differ.

For encrypted OCI layers, `ocicrypt` changes layer bytes and therefore digests, so
sign the *final encrypted manifest digest*, and retain an inner attestation if
consumers need pre-encryption provenance
([13](../13-supply-chain-security/)).

## Implications for liquid

1. **Encrypt below the chunk layer, dedup above it — restic's model.** CDC the
   plaintext, derive keyed chunk IDs with a repository secret, dedup on those IDs,
   then encrypt new chunks with fresh randomness. This preserves liquid's entire
   storage story ([02](../02-large-files-and-binary-assets/)) while giving real
   confidentiality and avoiding convergent-encryption attacks.

2. **Structured, value-level encryption for config — SOPS's model.** Keep keys and
   structure in the clear so diffs, reviews and merges still work. Whole-file
   encryption should be the fallback, not the default, because it destroys every
   collaboration property liquid is trying to provide.

3. **Envelope encryption with pluggable KEK custody.** Passphrase (scrypt/Argon2),
   age/X25519 recipients, cloud KMS, Vault, hardware tokens, TPM. Steve asked for
   "password protected or key protected" — both, plus the organisational options,
   through one interface.

4. **Per-subtree keys as a first-class concept, aligned with repository
   composition.** Since liquid makes nested repositories first-class
   ([01](../01-version-control/)), the natural encryption boundary is a nested
   repository or a declared subtree — which is also the natural sharing and access
   boundary. One concept, three uses.

5. **Filters are not a security boundary; enforce at commit.** git-crypt's own
   warning. liquid owns its client, so it should refuse to record plaintext at a
   path declared encrypted, and verify stored content rather than working-tree
   content. Add scanning and push protection for accidental secrets.

6. **Be explicit and loud about what encryption does not do.** Rotation does not
   rewrite history; revocation does not un-disclose; metadata stays visible. State
   this in the tool's own output when someone enables encryption, and provide a
   real `rotate` command that says exactly which revisions remain readable under
   which keys.

7. **Never make encryption break a checkout silently.** Same rule as large files
   ([02](../02-large-files-and-binary-assets/)): if content cannot be decrypted,
   that is a clear error, never a working tree containing ciphertext or pointers.
