# Keys and encryption

Liquid encrypts store objects inside a trust domain. Equal chunks in one domain deduplicate under a
keyed digest; equal plaintext in different domains has different identifiers. JSON and `.env` path
rules can encrypt selected leaf values while leaving the document structure visible, and file mode
encrypts the whole file when structure cannot remain visible.

Before signing up for lake, run one command and paste its output into the TAAP/1 identity document
field. The private signing key stays local for software, FIDO2, and PIV custody alike:

<!-- guides-run: keys-document -->
```sh
liquid identity show --document
```

The output is only unpadded base64url canonical CBOR, with no newline. This works without a
repository and creates a software identity if one does not exist. `--document` cannot be combined
with `--json` or `rotate`; those combinations report a usage error and exit 2. See the
[identity guide](../library/identity.md) for the document and verification contract.

The current user-facing identity commands manage the device's TAAP signing identity. They do not
configure store encryption keys:

<!-- guides-run: keys-identity -->
```sh
mkdir repository
cd repository
liquid init .
liquid identity show --json | python3 -m json.tool
liquid identity rotate --json | python3 -m json.tool
```

Store encryption is currently configured through libliquid's `keyring`, `passphrase_provider`, or
32-byte `key_file_provider` and repository encryption path rules. Recipient, cloud KMS, Vault,
TPM, and threshold custody URIs from issue #51 have not landed. The `keys`, store-key `rotate`, and
`revoke` CLI wiring is tracked separately, so this guide does not present those spellings as usable.
Identity rotation above rotates the signing identity, not a store data key.

When data-key rotation is used through the library, retain these three facts:

1. Rotation does not rewrite history. Existing objects stay encrypted under their recorded key
   version, while new writes use the next version.
2. Revoking a collaborator prevents future access through the remaining custody path; it cannot
   recall plaintext or key material already copied elsewhere.
3. Encryption does not hide repository metadata. Paths, sizes, change structure, and timing remain
   visible unless another layer protects them.

Rewrapping changes the provider that protects held data-key versions without changing pack bytes.
That is distinct from data-key rotation. Before revoking an old version, use the public readability
report in an embedding application to identify revisions that still require it.

See the contributor-facing [encrypted-store design](../library/encryption.md) and [structured
configuration design](../library/leaf-encryption.md) for the exact API, on-disk envelopes, failure
codes, and tests.
