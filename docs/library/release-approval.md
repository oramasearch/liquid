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

`verify_release(artifact, policy, approvals, trust)` first checks the policy against the trust root
(next section), verifies its signature and expiry, then verifies every matching approval through
the normal identity-chain verifier. Both bind each signer's name to its key only through an identity
chain the verifying machine holds (liquid#383): a policy whose issuer is unknown is refused with
`errc::unauthorized`, and an approval from a listed identity whose chain is unknown does not count
and is reported in `unknown_signers`. Distribute each signer's chain with `liquid identity show
--chain` and accept it with `liquid identity trust <chain>`
([identity.md](identity.md#how-a-verifier-obtains-and-trusts-another-persons-chain)). It counts
distinct TAAP identity identifiers that appear in the policy. Repeated approvals from one identity count
once. An approval from an unlisted identity, for another artifact or policy, or after policy
expiry does not count, and neither does any approval by a key that a later identity document
declares compromised: its `signed_at` is the signer's own claim, so an approval made before the
declaration is indistinguishable from one a thief backdated (liquid #391). Approvals carry no
third-party timestamp yet, so such an approval must be made again with the identity's new key
([identity.md](identity.md#rotation)). The result reports
`satisfied`, the sorted `distinct_valid_signers`, and the policy identities still `missing`.

## Trust root

The policy decides who approves a release, and it travels with the repository. Whoever can write
the repository or the release bundle could otherwise replace it with a policy they signed
themselves (threshold one, signer themselves) and approve any artifact; a held identity chain
proves only that the signature is theirs, and sync stores every author's chain (liquid#384). The
trust root is therefore held by the verifier, outside the repository, as a `release_trust` record:

- **`policy_issuers`**: the TAAP identity ids allowed to issue the policy. The policy's issuer must
  be listed, and its signature must verify through that identity's chain in this machine's
  identity store. **An empty list refuses every policy**; an unlisted issuer is refused with
  `release policy issuer <id> is not a configured release-policy issuer`.
- **`repository`**: the digest of the repository being released. A policy whose subject is another
  repository is refused with `release policy is for another repository`, so a pinned issuer's
  policy for one repository cannot authorize releases of another.

`liquid release verify` reads `policy_issuers` from **`$LIQUID_HOME/trust/release-policy-issuers`**,
one identity id per line ([identity.md](identity.md#local-trust-files)), and takes `repository`
from the repository it runs in (the BLAKE3 digest of its id, as `release policy set` signs it). A
missing or empty file refuses the command and names the file to create. Each listed issuer's chain
must also be trusted with `liquid identity trust`. libliquid callers pass their own `release_trust`.

**Why an issuer pin rather than a pinned policy digest.** A digest pin admits exactly one policy, so
every legitimate change (a new signer, a new threshold, and the renewal that `not_after`, ninety
days by default, forces) must be re-pinned by hand on every verifier, and in practice the new
digest would be copied from the repository, the very source this trust root excludes. An issuer pin
is the TUF root-to-targets delegation: a small set of policy owners, pinned once, may change the
policy, their keys rotate through their identity chains without re-pinning, and the subject check
keeps a policy to its own repository. Fewer re-pins mean fewer chances to pin the wrong thing.

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
those canonical records, prints the distinct count and missing identities, prints
`unknown_signer=<id>` for each listed identity whose approval is present but whose chain this
machine does not hold, and exits nonzero when the threshold is not satisfied.

Policy and approval files are evidence and are distributed with the release (in its bundle, or
copied beside the artifact). They live under the checkout's `.liquid/` directory, which is
checkout-local since liquid #420: tree import never records it, and a drive or clone never writes
it ([sync.md](sync.md)), so a repository sync does not carry them. They are not trusted because of where they are: `release verify` accepts the policy only from an
issuer listed in `$LIQUID_HOME/trust/release-policy-issuers` (see *Trust root*). The directory is
not a private-key store.

## Verification

`ReleaseApproval.threshold_counts_distinct_valid_identities` covers one-of-three refusal,
duplicate-identity counting, and two-of-three success.
`ReleaseApproval.approval_after_declared_compromise_is_rejected` proves that the identity chain's
compromise rule applies to release approvals.
`ReleaseApproval.policy_from_an_unpinned_issuer_is_refused` refuses a self-signed policy from an
identity whose chain is held but which is not a pinned issuer, and refuses every policy with an
empty issuer list. `ReleaseApproval.policy_for_another_repository_is_refused` refuses a pinned
issuer's policy for another repository and accepts it for its own. The threshold test first verifies with no
other identity trusted (the policy is refused), then with only the policy issuer trusted (both
approvals are `unknown_signers`), before trusting the approvers. `cli-release` does the same through
`liquid identity show --chain` and `liquid identity trust`; it first proves that `release verify`
refuses with no `trust/release-policy-issuers` file, and last that a policy replaced by a
repository writer who is not a pinned issuer is refused.

Real hardware is not required by CI. Hardware runs should record the date, token model, selected
backend, and result here when performed.
