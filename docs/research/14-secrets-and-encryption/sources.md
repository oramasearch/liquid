<!-- Raw research source. Generated via Perplexity `perplexity_research` (web-grounded,
     multi-source) on 2026-09-01 for the liquid project. Lightly reformatted only.
     Treat as evidence, not as liquid design decisions - see README.md in this
     directory for the curated analysis and the implications drawn for liquid. -->

# Encrypted data and secrets in version control and artifact systems, 2024-2026

**Bottom line:** Encryption can make Git or an artifact store a reasonable distribution channel for ciphertext, but it does not turn the repository into a secrets manager. Git preserves history and is designed for replication; anyone who once received plaintext or a decryption key can retain it indefinitely. For operational credentials, prefer short-lived, dynamically issued secrets from a secrets manager, and use repository encryption mainly for declarative configuration, bootstrap material, recovery data, or artifacts that genuinely must be versioned.

## 1. Tools and designs

| Tool/design | Encryption and key model | Version-control behavior | Important limitations / best fit |
|---|---|---|---|
| **git-crypt** | **Correction:** git-crypt does not implement standardized AES-SIV. It uses AES-256-CTR with a *synthetic IV derived from HMAC-SHA-1 of the plaintext*. A repository symmetric key is exported directly or stored once per authorized user as a GPG-encrypted key file under `.git-crypt`. Files selected by `.gitattributes` pass through Git clean/smudge filters. [web:301] | Encryption is deterministic: unchanged plaintext produces unchanged ciphertext. This exposes whole-file equality, length and change timing. Contrary to a common claim, it does **not** give useful Git deltas for modified files: changing the plaintext changes the synthetic IV, effectively changing the entire ciphertext, so Git generally stores another full encrypted blob. [web:301] | Filters were not designed as a security boundary; a bad `.gitattributes` rule or incompatible GUI can stage plaintext. Paths, filenames, commit messages, symlink targets and other metadata remain visible. It has no effective collaborator revocation or built-in key rotation, encrypted blobs are not compressible, and repository tampering can disable encryption rules. Best only for a small encrypted subset of an otherwise ordinary repository. [web:301] |
| **git-secret** | A Bash/GPG workflow: trusted users are registered by public key, selected plaintext files are "hidden" into GPG-encrypted counterparts and later "revealed" by authorized private-key holders. [web:12] | Explicit rather than transparent: typically commit the encrypted files and ignore the plaintext copies. This avoids some clean-filter surprises but makes synchronization dependent on users remembering to re-hide changed secrets. | Inherits GPG keyring, trust and recipient-management complexity. Removing a recipient and re-encrypting protects only new ciphertext; it cannot retract old ciphertext or plaintext already cloned. |
| **BlackBox** | GPG-encrypts explicitly registered files for a list of administrators; only the encrypted version is intended to remain in source control. It can work with Git and other repository systems. [web:1][web:2] | Explicit edit/decrypt/re-encrypt workflow rather than invisible filtering. | Similar operational burden to git-secret: GPG recipient management, binary diffs, user-error risk and no retroactive revocation. Useful for legacy GPG-centered organizations. |
| **transcrypt** | Uses OpenSSL symmetric ciphers and Git clean/smudge filters to encrypt selected paths automatically on staging and decrypt them on checkout. [web:5] | Ergonomically similar to git-crypt, generally with a shared password/key rather than per-user public-key envelopes. | Shared-secret distribution and rotation are difficult; filter misconfiguration can expose plaintext. It does not solve repository-history or former-collaborator problems. |
| **SOPS** | Structured-document editor for YAML, JSON, ENV and INI, plus whole binary files. It normally encrypts leaf values while preserving keys and structure. A random file data key is wrapped to one or more age, PGP, AWS KMS, GCP KMS, Azure Key Vault or Huawei Cloud KMS recipients; current SOPS also supports HashiCorp Vault/OpenBao Transit. [web:31][web:32][web:273] | Preserved YAML/JSON structure and selectively clear fields make diffs and reviews understandable. Rules such as `encrypted_regex` can encrypt only `data`, `stringData`, or specifically named secret fields. [web:34] | Usually the strongest "encrypted configuration in Git" option. Clear keys, structure, comments and selected values still leak metadata. Authorization to a KMS/Vault key is the real security boundary, and old Git revisions retain old wrapped data keys. |
| **age** | Narrow file-encryption format and CLI. Native recipients use X25519; every file receives a random file key, each X25519 stanza has a fresh ephemeral secret, and the payload uses HKDF-SHA-256 and chunked ChaCha20-Poly1305. Passphrase mode uses a fresh salt with scrypt. [web:303] | Randomized: re-encrypting identical plaintext produces different ciphertext, so it is unsuitable as a naive clean filter and yields noisy binary diffs. | It has displaced GPG in many file-encryption workflows because it has small explicit keys, no global trust database or broad configuration language, modern fixed constructions and Unix-style composition. It does **not** replace the entire OpenPGP ecosystem: age deliberately focuses on encryption, while signing, certificates and Web-of-Trust functions belong elsewhere. Its simplicity is a usability advantage, not retroactive revocation. [web:302] |
| **Sealed Secrets** | Kubernetes controller plus `kubeseal`: anyone with the cluster's public certificate can create a `SealedSecret`, but only the in-cluster controller possesses the private key needed to produce the Kubernetes `Secret`. Strict, namespace-wide and cluster-wide scopes bind ciphertext to different amounts of object metadata. [web:62][web:63] | A safe-to-commit Kubernetes custom resource supports GitOps without giving every author decryption access; even the original author normally cannot decrypt the sealed object. | Protect and back up the controller keys: losing them makes existing objects unrecoverable; compromising them exposes all objects they can unseal. It is cluster-delivery encryption, not a general secret manager, and the resulting ordinary Kubernetes Secret still exists in the cluster. |
| **Ansible Vault** | Password-based symmetric AES-256 encryption for complete files, or inline encrypted variables inside otherwise readable YAML. It supports viewing, editing, decrypting and re-keying encrypted material. [web:64][web:65][web:69] | Inline variables preserve more context; whole-file encryption produces opaque, noisy diffs. | Appropriate when Ansible is the consumer, but vault-password delivery becomes the central problem. Every operator or CI job with the password can retain plaintext, and re-keying the current file does not alter old Git revisions. |

### Practical selection

| Requirement | Preferred design |
|---|---|
| Readable GitOps YAML/JSON diffs with cloud identity controlling decryption | **SOPS + KMS/Vault Transit**, or SOPS + age for smaller/offline teams |
| Kubernetes-only one-way delivery to a particular cluster | **Sealed Secrets** |
| Ansible-native encrypted variables | **Ansible Vault** |
| Simple encrypted attachment/archive with explicit recipients | **age** |
| Legacy team already standardized on OpenPGP | **git-secret or BlackBox** |
| Transparent encryption of a few files in an existing repository | **git-crypt**, only with strong checks against accidental plaintext commits |
| Runtime passwords, tokens and database credentials | Do not make Git authoritative; store references or secret identifiers in Git and fetch/mint values at deployment time |

---

## 2. Fundamental cryptographic and version-control problems

| Problem | What happens | Consequence / mitigation |
|---|---|---|
| **Deterministic encryption** | Equal plaintext under the same key/context gives equal ciphertext. This exposes equality and often approximate size/change timing. Convergent encryption takes this further by deriving identity or keys from content. | Enables ciphertext equality testing and potentially server-side deduplication. Use it only where equality leakage is acceptable. For low-entropy secrets-booleans, short status values, known templates-guessing attacks are especially relevant. |
| **Randomized encryption** | Fresh nonce, ephemeral key or salt makes the same plaintext encrypt differently each time; age explicitly generates new file keys, X25519 ephemeral secrets and payload nonces per file. [web:303] | Provides stronger semantic confidentiality but defeats naive ciphertext-only deduplication and creates opaque Git diffs. Dedup can still occur **before encryption** in a trusted client, as restic and Borg demonstrate. |
| **"Diff versus encryption"** | Git's textual delta and review model operates on stored blobs. Whole-file authenticated/randomized encryption makes tiny plaintext edits look like whole-blob replacements. Determinism only preserves exact unchanged blobs; it does not automatically preserve localized deltas. | Use structured, field-level encryption such as SOPS for reviewability, or chunk plaintext client-side and deduplicate by a keyed content identifier before randomized encryption. Do not weaken encryption merely to obtain pretty ciphertext diffs. |
| **Key rotation with immutable history** | Re-encrypting the current branch under key B does not remove historical ciphertext encrypted under key A. A clone can check out those commits indefinitely. git-crypt explicitly identifies prior-history access as the reason revocation/rotation is intrinsically difficult. [web:301] | Keep old keys if old history must remain decryptable, or rewrite history and delete old keys if it must not. With envelope encryption, rewrapping current DEKs is cheap, but every historical envelope must also be rewrapped or removed. |
| **Revoking a collaborator** | Removing their GPG recipient, KMS role or repository account only prevents future access. It cannot make them forget plaintext, exported keys, cached DEKs, screenshots, backups or a full clone. Keybase similarly rotates future folder keys but does not re-encrypt old data. [web:213] | Rotate the *underlying operational credentials*, not merely the file-encryption key. Re-encrypt future repository states and rewrite history if warranted, but treat already disclosed material as permanently disclosed. |
| **Repository compromise** | Encryption protects contents against someone who lacks the key; it does not necessarily protect branch rules, filenames, commit metadata, availability, rollback, or the encryption-selection policy. In filter systems an attacker may alter `.gitattributes` so later plaintext is committed. [web:301] | Sign protected commits/tags or artifacts, enforce CI checks and secret scanning, protect branches, and authenticate the repository state separately from encrypting selected blobs. |
| **"Encrypted in Git" versus "secret"** | A ciphertext blob is confidential only while keys and decrypted working copies remain controlled. Every authorized laptop, CI runner and deployment system becomes a plaintext endpoint. Static credentials also retain their authority after decryption or copying. | A true secrets system adds identity-based authorization, audit trails, expiry, dynamic issuance, revocation, policy and often non-exportable key operations. Repository encryption supplies primarily **confidentiality at rest and in replication**, not those lifecycle controls. |

---

## 3. Envelope encryption and key management

### Core construction

1. Generate a random **data-encryption key (DEK)** for a file, object, value or chunk.
2. Encrypt the data locally with an authenticated cipher and the DEK.
3. Use a longer-lived **key-encryption key (KEK)** to wrap the DEK.
4. Store `{ciphertext, wrapped DEK, algorithm/context metadata}` together.
5. At read time, authorize an unwrap operation, use the recovered DEK briefly, then erase it from memory.

AWS and Google describe precisely this DEK/KEK construction. AWS KMS can return both a plaintext data key and a KMS-encrypted copy; the application encrypts locally, discards the plaintext DEK and stores the wrapped key with the ciphertext. [web:76][web:77][web:78]

| KEK/custody option | Design and advantages | Limits and operational cautions |
|---|---|---|
| **AWS KMS** | KMS key wraps per-object data keys; IAM, grants, CloudTrail and key versions centralize control. Only small keys-not large artifacts-need cross the KMS boundary. [web:77][web:83] | Disabling/deleting the KEK can make all dependent envelopes unavailable. Cached plaintext DEKs and previously exported plaintext remain unaffected. Rotation is not complete until relevant old envelopes are rewrapped or retired. |
| **Google Cloud KMS** | Application generates/uses the DEK locally; Cloud KMS acts as the KEK and wraps the DEK. KEK material does not leave the Cloud KMS boundary. [web:81][web:88] | The same history, availability and cached-key caveats apply. Bind access to workload identity rather than copying long-lived service-account keys into CI. |
| **Azure Key Vault / Managed HSM** | Azure services use a customer-managed KEK to wrap and unwrap DEKs; Key Vault exposes `wrapKey`/`unwrapKey`, with RSA-OAEP or AES key-wrap depending on key type. [web:106][web:109] | Preserve old key versions until all required DEKs are rewrapped; Azure explicitly notes that rotation can rewrap DEKs without re-encrypting underlying data. [web:119] |
| **Vault Transit** | Encryption-as-a-service: applications submit plaintext or data keys to a named Transit key, while Vault retains key versions and enforces token policy. It can also generate data keys for client-side envelope encryption. [web:79][web:80] | Vault becomes an online trust and availability dependency. Restrict encrypt/decrypt paths separately, use short-lived workload authentication, and plan backup, unseal and disaster recovery. |
| **Shamir threshold sharing** | Splits a root/unseal key into `n` shares so any threshold `t` reconstructs it, while fewer than `t` reveal nothing. Vault's default Shamir seal uses shares to reconstruct the root key that decrypts Vault's internal encryption key. [web:125][web:128] | This is custody/quorum protection, not per-file encryption or continuous multi-party authorization. Once reconstructed, the key exists at the reconstruction endpoint. Secure each share independently and test recovery. |
| **YubiKey PIV** | RSA/ECC private-key operations occur using a key stored on the smart card via PIV/PKCS#11. It can unwrap a DEK or support an age PIV recipient without exporting the private key. [web:91] | Encrypting needs only the public key; decrypting needs the token, PIN and possibly touch. Provision backup tokens: a destroyed sole token is permanent data loss. PIV is different from the YubiKey's FIDO2 application. |
| **FIDO2 `hmac-secret` / WebAuthn PRF** | A credential-scoped secret is derived inside the authenticator from a caller-provided salt; YubiKey returns 32-byte values suitable for deriving a wrapping key. [web:92] | This is a derivation mechanism, not generic public-key encryption. Recovery and multi-device enrollment must be designed before encrypting data; ordinary FIDO authentication support does not imply `hmac-secret` support. |
| **TPM sealing** | A local DEK/KEK can be sealed to a TPM and optionally to expected Platform Configuration Register values, making it available only when measured hardware, firmware and boot state satisfy policy. [web:293][web:300] | Good for unattended machine-bound decryption, but brittle PCR policies can lock out legitimate upgrades. It does not prove the running application is benign unless measured boot, policy authorization and attestation are designed coherently. Backup/recovery requires a separate escrow route. |

**Envelope-encryption advantage:** ordinary KEK rotation can rewrap a small DEK instead of re-encrypting gigabytes of content. **Historical catch:** if Git retains an earlier wrapped DEK and the old KEK is still usable-or a former user cached the unwrapped DEK-the earlier revision remains decryptable.

---

## 4. Encrypted content-addressed storage

| System/design | Encryption, addressing and deduplication | Leakage / security properties |
|---|---|---|
| **Naive convergent encryption** | Derive an encryption key or ciphertext identifier deterministically from the plaintext. Equal files/chunks converge to equal ciphertext, permitting cross-user server-side deduplication. | Vulnerable to a **confirmation-of-a-file attack**: an attacker who can guess or obtain candidate plaintext computes the convergent result and tests whether the target store contains it. It also enables offline dictionaries for low-entropy content. [web:136][web:144] |
| **restic** | A repository has random master key material protected by one or more password/key records. Files are content-defined into variable-size chunks; clients identify and deduplicate plaintext chunks first, then encrypt newly stored blobs using AES-256-CTR with Poly1305 authentication and a fresh random nonce. [web:155][web:162][web:164] | Randomized ciphertext and deduplication coexist because the trusted client maintains an encrypted index keyed by plaintext chunk IDs and simply reuses an existing ciphertext blob instead of encrypting a duplicate again. This is repository-local dedup, not unauthenticated global convergent encryption. |
| **BorgBackup** | Content-defined chunks are identified by a cryptographically strong keyed hash/MAC. Only unseen chunks are stored. Encrypted repositories use repository/keyfile key material; Borg 1.x `repokey`/`keyfile` modes use AES-CTR-256 plus HMAC-SHA-256, while Borg 2 offers AEAD modes such as AES-OCB and ChaCha20-Poly1305. [web:168][web:170][web:175] | Repository-wide dedup works before storing another chunk, including across backups and hosts sharing that repository/key context. The keyed chunk ID prevents an outside storage server from freely computing identifiers for guessed plaintext, although a compromised client with repository keys can. |
| **Tahoe-LAFS** | Clients encrypt and integrity-check every file with a unique key before erasure-coding and uploading shares. Access is represented by unguessable capabilities: read, verify, and-for mutable files/directories-write capabilities. [web:181][web:185] | Storage operators cannot decrypt or undetectably modify data. Possession of a capability *is authority*, so capability leakage grants access and is difficult to revoke. It is a capability-based distributed filesystem rather than a Git-compatible history/diff system. |

### Deduplication nuance

Randomized encryption does not make all deduplication impossible. It prevents an untrusted backend from finding equality by comparing ciphertext alone. A trusted client can:

- chunk plaintext;
- compute a **keyed** content identifier;
- consult an encrypted/local index;
- reuse already stored ciphertext for a known chunk; and
- apply randomized authenticated encryption only the first time that chunk is stored.

This is how designs such as restic and Borg retain deduplication without exposing a public convergent function.

---

## 5. Per-path and per-subtree encrypted access control

| Design | How access can be represented | Constraints and revocation reality |
|---|---|---|
| **Independent encrypted subtrees** | Give each protected top-level subtree its own random key; distribute or KMS-wrap that key only to the subtree's readers. Commit ciphertext or an encrypted manifest beneath that path. | Git hosting permissions still normally apply to the whole repository. Cryptography withholds plaintext, not object IDs, pathnames, commit relationships, sizes or change timing. Moving a file between subtrees requires decrypt/re-encrypt and careful removal of earlier copies. |
| **Hierarchical per-directory keys** | Each directory has a random key; file keys are wrapped by the directory key, while child-directory keys are wrapped by parents or directly to authorized principals. Granting a subtree key grants descendants without exposing siblings. Linux `fscrypt`, for example, protects an encrypted directory tree with a master key and derives per-directory/per-file keys. [web:224] | A simple parent-wrap hierarchy makes downward delegation easy but selective revocation expensive: changing an ancestor may require rewrapping or re-encrypting many descendants. Git history again retains old wraps and ciphertext. |
| **Multiple-key Git layout** | With tools that support separate rules/key groups, use one key per security domain-such as `environments/dev`, `environments/prod`, and `customers/A`-rather than one repository-wide key. | Avoid overlapping rules and accidental reuse of a broad key. CI must receive only the key for the subtree it deploys. Metadata and historical-access caveats remain. |
| **Keybase KBFS** | Access is defined at a top-level folder (TLF), whose canonical readers and writers control a shared end-to-end-encrypted folder; clients also verify writer signatures. [web:211][web:213] | It is TLF-level rather than arbitrary ACLs on every nested directory. Revoking a device/member rotates keys for **future** data, but old data is not re-encrypted, even though Keybase servers refuse to serve it to revoked devices. [web:213] |
| **Tahoe-LAFS directories/capabilities** | Sharing a directory's read or read/write capability delegates that directory/subtree; capabilities can be diminished from write to read/verify authority. [web:181][web:186] | Bearer-capability leakage is equivalent to key leakage. Fine-grained revocation generally requires creating replacement objects/directories and redistributing new capabilities. |

For strict cryptographic separation, separate repositories-or separate signed, encrypted artifacts-are often operationally safer than an elaborate key hierarchy inside one Git object graph.

---

## 6. Signing and encryption interaction

| Construction/order | What the signature proves | Visibility and lifecycle implications |
|---|---|---|
| **Sign plaintext, then encrypt `{plaintext, signature}`** | After decryption, a recipient can verify the original author and exact plaintext. | Outsiders cannot verify authorship without decrypting. Re-encryption for different recipients preserves the inner signature and plaintext provenance. Usually the right choice for confidential authored documents. |
| **Encrypt, then sign the ciphertext** | Anyone with the verification key can authenticate the exact encrypted envelope and signer, without decrypting it. | It does not by itself prove who authored the plaintext-the signer might merely have repackaged unknown ciphertext. Changing recipients, rewrapping metadata or re-encrypting changes the signed bytes and requires another signature. |
| **Both layers** | Inner signature authenticates plaintext provenance; outer signature authenticates the distributed envelope, recipients and encryption metadata. | Strongest when packagers and content authors are different actors, at the cost of more key and policy management. |
| **Sign a manifest/digest** | Signature binds identity to a content-addressed artifact descriptor or digest. OCI signing systems use this model: verification checks publisher trust and whether the descriptor has changed. [web:204] | The signing policy must specify whether the digest is for plaintext, ciphertext, or both. Signing only a mutable tag is insufficient; sign an immutable digest. |
| **ocicrypt encrypted OCI layers** | ocicrypt implements OCI image-layer encryption and exposes layer encryption/decryption operations. [web:196][web:197] | Encryption changes layer bytes, digests and consequently the image manifest. Therefore, for distribution, first create the final encrypted image and then sign its encrypted manifest digest. If consumers also need publisher assurance over the pre-encryption image, retain an inner/plain-image signature or attestation linking plaintext and encrypted digests. |

Encryption supplies confidentiality; signatures supply origin/integrity. One does not substitute for the other. Also, a valid signature does not make content safe-it only identifies the signer and detects changes relative to what was signed.

---

## 7. Failure stories and incident response

| Failure story | Why the attempted fix fails | Correct response |
|---|---|---|
| **A secret was committed, then deleted in the next commit** | Git records snapshots and keeps the original blob reachable from the earlier commit, tags, pull requests, caches and existing clones. Deleting the working-tree file merely creates a new snapshot without it. | **First revoke/rotate the credential.** Then use `git-filter-repo` to remove the path or replace the value throughout all affected history, force-update branches/tags, and coordinate cleanup of forks, PR references, caches and colleague clones. GitHub's official procedure calls for rotation first, local history rewriting, updating GitHub and cleaning other clones. [web:256] |
| **The repository was made private after exposure** | Prior public clones, search indexes, package artifacts, logs and credential-scanning bots may already possess the secret. | Treat it as compromised and rotate it; privacy changes do not revoke copied data. |
| **The plaintext file was replaced with an encrypted version** | Earlier commits still contain plaintext. Adding `.gitattributes`, SOPS or an encrypted replacement only protects later revisions. | Rewrite every affected revision or start a clean repository, but rotate the underlying secret regardless. |
| **A collaborator was removed and files re-encrypted** | Their old clone, plaintext, old repository key and historical ciphertext remain usable. | Rotate all operational credentials they could access; introduce new repository/DEK keys for future data; rewrite old history only to reduce further exposure, not as a guarantee of forgetting. |
| **A Git filter was assumed to guarantee encryption** | A file may have been staged before the filter rule, through an incompatible GUI, from another worktree, or after `.gitattributes` was altered. git-crypt warns that Git filters are not a complete security boundary. [web:301] | CI should inspect committed blobs, not working-tree files; fail if protected paths lack the expected ciphertext header; protect `.gitattributes`; and run independent secret scanning. |
| **GitHub secret scanning was treated as a vault** | Scanning is detection, based on supported patterns and validation; it cannot guarantee detection of every password, private format or custom secret. [web:247] | Use it as defense in depth. Enable **push protection**, which blocks supported hardcoded credentials before they reach the repository; alert on and tightly govern bypasses. [web:241][web:244] |
| **A blocked push was fixed only in the newest commit** | The secret may still be present in an earlier commit included in the same push. | Remove it from **every commit** reported by push protection-amend/rebase or rewrite as appropriate-then retry. GitHub explicitly requires removal from all commits containing it. [web:248] |

## Recommended architecture

| Layer | Recommendation |
|---|---|
| Repository | Store templates, secret references, access policy and encrypted declarative configuration-not freely reusable production credentials. |
| Encryption format | Prefer **SOPS** for structured configuration and **age** for opaque files. Use separate DEKs per file/object and authenticated randomized encryption. |
| KEK custody | Wrap DEKs with cloud KMS or Vault Transit using workload identity, audit logging and least-privilege decrypt policy. Use PIV/TPM for selected human or machine bootstrap keys. |
| Access partitioning | Use separate repositories or independently keyed top-level security domains. Do not claim that Git path permissions alone are cryptographic isolation. |
| Rotation | Rotate KEKs by rewrapping DEKs; rotate DEKs when exposure is suspected. Explicitly decide whether historical revisions remain decryptable. |
| Deployment | Fetch or mint runtime secrets using short-lived workload identity; decrypt only in memory or ephemeral restricted storage. |
| Integrity | Sign commits/tags or final artifact digests independently of encryption. For encrypted OCI images, sign the final encrypted manifest. |
| Prevention and response | Enable secret scanning and push protection, test committed blobs in CI, and maintain a runbook: revoke first, rewrite history second, clean clones/caches third. |

The central rule is: **once plaintext or a decryption-capable key reaches another principal, repository cryptography can control future distribution, but it cannot revoke knowledge already obtained.**

## Citations

[1] https://github.com/StackExchange/blackbox
[2] https://github.com/StackExchange/blackbox/blob/master/README-v2.md
[5] https://www.gsp.com/cgi-bin/man.cgi?section=1&topic=TRANSCRYPT
[12] https://sobolevn.me/git-secret/
[31] https://getsops.io/docs/reference/
[32] https://github.com/getsops/sops/blob/master/README.rst
[34] https://getsops.io/docs/usage/common-operations/
[62] https://github.com/bitnami-labs/sealed-secrets
[63] https://github.com/bitnami-labs/sealed-secrets/blob/main/README.md
[64] https://docs.ansible.com/projects/ansible/latest/vault_guide/vault.html
[65] https://docs.redhat.com/en/documentation/red_hat_enterprise_linux/10/html/automating_system_administration_by_using_rhel_system_roles/ansible-vault
[69] https://docs.ansible.com/projects/ansible/latest/vault_guide/vault_encrypting_content.html
[76] https://docs.aws.amazon.com/kms/latest/developerguide/kms-cryptography.html
[77] https://docs.aws.amazon.com/kms/latest/cryptographic-details/client-side-encryption.html
[78] https://docs.cloud.google.com/kms/docs/envelope-encryption
[79] https://developer.hashicorp.com/vault/docs/secrets/transit
[80] https://developer.hashicorp.com/vault/tutorials/encryption-as-a-service/eaas-transit
[81] https://docs.cloud.google.com/docs/security/key-management-deep-dive
[83] https://aws.amazon.com/kms/faqs/
[88] https://docs.cloud.google.com/kms/docs/client-side-encryption
[91] https://docs.yubico.com/software/yubikey/tools/pivtool/Introduction.html
[92] https://docs.yubico.com/yesdk/users-manual/application-fido2/hmac-secret.html
[106] https://learn.microsoft.com/en-us/azure/key-vault/keys/about-keys-details
[109] https://learn.microsoft.com/en-us/azure/security/fundamentals/encryption-customer-managed-keys-support
[119] https://learn.microsoft.com/en-us/azure/virtual-machines/disk-encryption
[125] https://developer.hashicorp.com/vault/docs/commands/operator/init
[128] https://web.mit.edu/6.857/OldStuff/Fall03/ref/Shamir-HowToShareASecret.pdf
[136] https://smarx.com/posts/2020/09/convergent-encryption-and-why-no-one-uses-it/
[144] https://eprint.iacr.org/2017/1089.pdf
[155] https://restic.readthedocs.io/en/latest/100_references.html
[162] https://words.filippo.io/restic-cryptography/
[164] https://restic.readthedocs.io/en/v0.13.1/design.html
[168] https://borgbackup.readthedocs.io/en/1.1.5/usage/init.html
[170] https://borgbackup.readthedocs.io/en/latest/usage/repo-create.html
[175] https://borgbackup.readthedocs.io/en/stable/introduction.html
[181] https://tahoe-lafs.readthedocs.io/en/latest/about-tahoe.html
[185] https://tahoe-lafs.org/~trac/lafs.pdf
[186] https://blog.torproject.org/tor-heart-tahoe-lafs/
[196] https://github.com/containers/ocicrypt
[197] https://github.com/containers/ocicrypt/blob/main/README.md
[204] https://learn.microsoft.com/en-us/azure/container-registry/overview-sign-verify-artifacts
[211] https://book.keybase.io/docs/crypto/kbfs
[213] https://book.keybase.io/docs/files/details
[224] https://android.googlesource.com/kernel/common/+/6be064d42c55/Documentation/filesystems/fscrypt.rst
[241] https://docs.github.com/en/code-security/concepts/secret-security/about-push-protection
[244] https://docs.github.com/en/code-security/how-tos/secure-your-secrets/prevent-future-leaks/enable-push-protection
[247] https://docs.github.com/en/code-security/reference/secret-security/secret-scanning-scope
[248] https://docs.github.com/en/enterprise-server@3.14/code-security/how-tos/secure-your-secrets/work-with-leak-prevention/working-with-push-protection-from-the-command-line
[256] https://docs.github.com/en/authentication/keeping-your-account-and-data-secure/removing-sensitive-data-from-a-repository
[273] https://getsops.io/docs/usage/identities/hashicorp-vault-openbao/
[293] https://learn.microsoft.com/en-us/windows/security/hardware-security/tpm/switch-pcr-banks-on-tpm-2-0-devices
[300] https://www.kernel.org/doc/html/v5.1/security/keys/trusted-encrypted.html
[301] https://github.com/AGWA/git-crypt
[302] https://neilmadden.blog/2019/12/30/a-few-comments-on-age/
[303] https://github.com/FiloSottile/age
