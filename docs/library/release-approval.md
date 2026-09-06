# Threshold release approval

**Status: implemented for liquid #81.** A release role is a signed policy and its threshold is
evaluated over independent TAAP signatures. This is TUF-style `k`-of-`n` authorization, not an
aggregate or threshold-signature scheme.

## Signed profiles

`release_policy/1` has the repository digest as its `repository` subject. Its attributes are
`role:text`, `threshold:unsigned_integer`, `signers:text_list`, and `not_after:text`. The signer list
is sorted and deduplicated before signing, the threshold must be between one and the number of
signers, and `not_after` is an RFC 3339 UTC timestamp.

`release_approval/1` has the artifact digest as its `artifact` subject. `policy:text` is the BLAKE3
digest of the complete canonical signed policy and `release:text` names the release. Binding the
policy by digest prevents an approval from moving to a different role, threshold, signer set, or
expiry.

`verify_release(artifact, policy, approvals)` first verifies the policy signature and expiry, then
verifies every matching approval through the normal identity-chain verifier. It counts distinct
TAAP identity identifiers that appear in the policy. Repeated approvals from one identity count
once. An approval from an unlisted identity, for another artifact or policy, after policy expiry,
or at or after that key's signed compromise declaration does not count. The result reports
`satisfied`, the sorted `distinct_valid_signers`, and the policy identities still `missing`.

These are the same role and distinct-issuer semantics used by TUF metadata. Client-side TUF
metadata verification remains liquid #186; the shared evaluator is
`src/identity/threshold.cpp`.

## Custody choices

Release policy and approval signers use the current identity backend. Custody changes the attack
surface, not the threshold rule.

| Custody | Trust model | Operational fit | Failure boundary |
|---|---|---|---|
| Software Ed25519 | The encrypted local key file and host account are trusted. | Developer identities and automation whose host is already the trust boundary. | Loss of the wrapping secret or identity directory prevents signing; host compromise can use the key. |
| FIDO2 token | The credential private key remains in the authenticator; liquid trusts the selected USB HID device and user-presence result. | Interactive approvals and portable hardware custody. | Missing token, denied presence, malformed CTAP2 response, or assertion verification failure refuses signing. |
| PIV slot `9c` | The P-256 private key remains in the smart-card applet; liquid trusts the OS PC/SC service and token policy. | Organisational cards and established PIV issuance. | Missing reader/card, APDU status failure, or malformed ECDSA result refuses signing. |
| TPM PCR policy | A data key is released only by the same TPM under the selected PCR state; this is encryption custody, not an identity signer. | Unattended machine-bound decryption for release automation. | A different TPM, changed selected PCR, TPM authorization failure, or absent device refuses unseal. |
| Independent mixed custody | Each listed identity chooses its own backend, so one backend or host need not control the threshold. | High-value release roles. | Availability requires any `k` listed identities; compromise of fewer than `k` does not authorize a release. |

## CLI and storage

`liquid release policy set --threshold <k> --signer <id>...` writes the signed policy to
`.liquid/release/policy.cbor`; `--role` defaults to `targets`, `--not-after` accepts an explicit UTC
expiry, and the default expiry is printed. `liquid release approve <artifact>` writes one approval
per issuer below `.liquid/release/approvals/<artifact>/`. `liquid release verify <artifact>` loads
those canonical records, prints the distinct count and missing identities, and exits nonzero when
the threshold is not satisfied.

Policy and approval files are evidence and should be committed or distributed with the release.
The directory is not a private-key store.

## Verification

`ReleaseApproval.threshold_counts_distinct_valid_identities` covers one-of-three refusal,
duplicate-identity counting, and two-of-three success.
`ReleaseApproval.approval_after_declared_compromise_is_rejected` proves that the identity chain's
signing-time compromise rule applies to release approvals.

Real hardware is not required by CI. Hardware runs should record the date, token model, selected
backend, and result here when performed.
