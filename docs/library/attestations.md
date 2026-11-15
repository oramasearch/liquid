# Attestations

libliquid has one TAAP attestation registry. A verifier selects its rules from the exact,
versioned profile name; it never infers policy from an attribute's spelling. An unregistered
profile is invalid, including when its signature is otherwise valid.

## Record and verification

`liquid/attestation.hpp` exposes `attestation`, the immutable global
`attestation_profile_registry`, typed `attestation_attribute` values, and
`verify_attestation`. Attribute values are `text`, `unsigned_integer`, `boolean`, or
`text_list`. New-profile records encode `subject_kind`, an `attributes` map, and an
`attribute_disclosure_commitments` map alongside the signed common record fields. The old
`scan_result/1` encoding has no new fields and is byte-identical to the #50 encoding.
`slsa_provenance/1` instead uses a typed SLSA predicate under `attributes`.

`verify_attestation` first reconstructs and verifies the TAAP signature, then verifies the
registered profile, subject kind, schema, freshness, status evidence, and supplied attribute
disclosures. Its result is `{ verified, reason }`; `verify(attestation)` remains the compact
boolean form. Encode also validates the profile schema so invalid records cannot be emitted by
the public encoder.

| Profile | Subject kind | Required attributes | Optional attributes | Status policy |
|---|---|---|---|---|
| `org_membership/1` | actor | `organization:text`, `role:text` | `display_name:text` | revocable; fresh status evidence no older than 24 hours |
| `delegation/1` | actor | `scopes:text_list`, `may_publish:boolean`, `may_lease:boolean`, `depth_remaining:unsigned_integer`, `dispatcher:text`, `not_after:unsigned_integer` | `model:text` | revocable; fresh status evidence no older than 1 hour; valid from `at` to `not_after`, at most 30 days |
| `authorship/1` | drip | `author:text` | none | none |
| `review_approval/1` | change | `reviewer:text`, `approved:boolean` | `comment:text*` | none |
| `slsa_provenance/1` | artifact | typed `buildDefinition` and `runDetails` SLSA v1 predicate | none | none |
| `sbom/1` | artifact | `format:text`, `document:text`, `derived:boolean`, `inferred:boolean`, `components:text_list*` | `supplier:text*` | none |
| `delta/1` | artifact | `source:text`, `target:text`, `algorithm:text`, `delta:text` | none | none |
| `hot_update/1` | artifact | `supersedes:text`, `target:text`, `tier:text`, `symbols:text_list`, `object:text`, `patch:text`, `outcome:text` | none | none |
| `model_provenance/1` | model | `training_dataset:text*`, `training_code:text*` | `base_model:text*` | none |
| `abi_compatibility/1` | library | `compatible_with:text`, `compatibility:text` | `notes:text` | none |
| `scan_result/1` | drip | fixed record fields `detectors:text_list`, `findings:unsigned_integer`, `allowances:text_list` | none | maximum attestation age: 24 hours |
| `reproducibility/1` | artifact | `verified_by:text`, `sandbox:text`, `agreement:boolean` | `differing_outputs:text_list` | none |
| `image_provenance/1` | artifact | `image:text`, `tree:text` | `result:text` | none |
| `projection/1` | artifact | `target:text`, `source_result:text_list`, `normalisation:text`, `tool:text` | none | none |
| `platform_signature/1` | artifact | `platform:text`, `unsigned_digest:text`, `signed_digest:text`, `signing_identity:text`, `verification:text` | `notarization_ticket:text`, `timestamp_response:text` | none |
| `release_policy/1` | repository | `role:text`, `threshold:unsigned_integer`, `signers:text_list`, `not_after:text` | none | none |
| `release_approval/1` | artifact | `policy:text`, `release:text` | none | none |

`*` marks a field that may be represented by a salted disclosure commitment rather than its
plaintext value. A required field is satisfied by its committed value when the profile permits it.

### Delegation chains

`delegation/1` is the authority carried by an agent session. The issuer is the principal and the
subject is the agent actor. `scopes` contains repository-relative path globs; `may_publish` and
`may_lease` are independent capabilities; `depth_remaining` limits further delegation; `model`
records the optional model label; `dispatcher` names who launched the agent; and `not_after` is the
end of its validity, in seconds since the Unix epoch.

`check_delegation_window(value, now)` refuses a delegation that has expired (`now` is at or past
`not_after`) or was issued more than `delegation_clock_skew` (5 minutes) in the future, with
`errc::unauthorized`, and one without `not_after` or valid for longer than
`delegation_maximum_lifetime` (30 days), with `errc::invalid_argument` (liquid #514). Revocation is
answered by a `delegation_status_resolver`, given the delegation and its digest. There is no
default: a session without a resolver, or whose resolver fails, is refused.
`local_delegation_status()` reads this machine's list, `$LIQUID_HOME/trust/delegation-revocations`
(one delegation digest per line, `#` comments), on every call; a missing file revokes nothing and a
malformed one refuses every delegation.

`validate_delegation(child, parent)` enforces attenuation. Every child scope must be contained by
a parent scope, a denied capability cannot become allowed, and remaining depth must decrease.
Session creation verifies the window, the signature, the status the caller's resolver returns, and
that the local principal issued the delegation; every lease and publish checks the window and the
status again, so a revoked or expired delegation stops a running session. Private writes need no publish capability. Scope and capability
checks happen before lease or base validation, and version-3 signed drips record the delegation
digest and agent actor. Versions 1 and 2 remain readable.

The `scan_result/1` names describe its established wire fields: detector ids, finding count, and
allowance records. Its full encoding and privacy limits remain in
[secret-scanning.md](secret-scanning.md#scan_result1).

`reproducibility/1` uses the cached result digest as its artifact subject. `verified_by` is the
device TAAP id, `sandbox` is the exact sandbox-definition digest, and `agreement` records whether
exit status and all output digests matched. A disagreement includes each differing output with its
cached and re-executed digest, including `missing` where one execution produced no value.
The `slsa_provenance/1` predicate is derived from action and result records, not supplied as
generic attributes. Its exact field mapping, interim artifact subject, and verification path are
in [provenance.md](provenance.md).
`sbom/1` names the canonical SBOM record by digest and makes the graph-derived versus
binary-inferred distinction an explicit signed claim. Its record chain and SPDX/CycloneDX mappings
are in [sbom.md](sbom.md).
`delta/1` uses the target digest as its artifact subject. Its attributes name the source and target
digests, algorithm id, and delta-record digest. Fetch verifies the signature and exact catalog to
record mapping before loading the patch; [distribution.md](distribution.md#delta-encoding-for-delivery-64)
defines the record and final target verification.
`abi_compatibility/1` is issued automatically during action-result publication when a built shared
library has an earlier descriptor for the same output path. The current library blob is its subject,
`compatible_with` names the earlier blob, `compatibility` records the deterministic verdict, and
`notes` records the change count. The repository device identity signs it through the same issuer
path as provenance and SBOM claims. [abi.md](abi.md) defines extraction and verdict rules.
`hot_update/1` uses the new result digest as its artifact subject. For tier-1 or tier-2 `patched`,
`object` names the relocatable definition object and `patch` names its canonical patch record. For
tier 0 or `restart`, `object` names the target result and `patch` names the canonical outcome
record. Verification binds every attribute to the record before any object is loaded;
[hot-update.md](hot-update.md) defines the tier rules, runtime delivery, and composition chain.
`image_provenance/1` uses the OCI image's SHA-256 digest as its subject. `image` is the complete
digest-pinned reference, `tree` is its `io.liquid.tree` digest, and optional `result` is the build
result projected into the image. [`admission.md`](admission.md) defines its minting and policy use.
`projection/1` uses the projected package, image manifest, or canonical chart digest as its
subject. `source_result` preserves every result or install-tree identity in request order;
`normalisation` records the fixed epoch, ownership, modes, and sorting rule; and `tool` binds the
liquid version. OCI and Helm outputs carry the statement beside source provenance and SBOM
statements through subject-bound OCI artifact manifests. See [projection.md](projection.md).
`platform_signature/1` uses the shipped, signed installer digest as its subject.
`unsigned_digest` names the artifact already covered by `projection/1`; `signed_digest` repeats the
subject for explicit consumer checks; `platform` selects the native verifier; and
`signing_identity` records the certificate fingerprint rather than a display name. macOS records
the accepted notary ticket after stapling, while Windows records the RFC 3161 timestamp service
response. The sidecar is signed by the local TAAP identity and `liquid sign verify` checks its
signature, both digest bindings, and `pkgutil`, `codesign`, or `signtool` before returning success;
an attested notary ticket also requires a fresh `spctl` assessment.
See [projection.md](projection.md#unsigned-projection-and-platform-signing).
`release_policy/1` and `release_approval/1` implement independent-signature release authorization.
The policy binds a repository role, threshold, allowed TAAP identities, and expiry; each approval
binds one artifact to the complete signed policy. Distinct counting, compromise-time rejection,
and TUF alignment are specified in [release-approval.md](release-approval.md).

For `max_age`, the verifier rejects a future or old attestation. For `revocable`, the caller
must provide an `attestation_status` that is not revoked and is no older than the profile's
maximum status age. Status retrieval belongs to the caller, so this library remains usable
offline and does not choose a network authority.

## Selective disclosure

For a field marked commit-capable, the issuer computes:

```text
SHA-512/256("TAAP/1 attribute" || 0x00 || name || 0x00 || canonical-CBOR(value) || 0x00 || salt)
```

The commitment is inside the signed record. The subject retains a distinct salt for each field
and supplies only the salt/value pairs it elects to disclose. The verifier recomputes supplied
commitments; an undisclosed committed field remains unknown rather than absent. A wrong salt,
value, field name, or missing commitment rejects verification. Commitments reveal the field
names and count, and do not prove predicates over hidden values.

## Adding a profile

Add one `attestation_profile` entry in `src/attestation/registry.cpp` with a versioned name,
each field's type and required/commit-capable flags, accepted subject kinds, and status policy.
Add accepted and missing-required-field tests under `tests/attestation/`, update this table and
the API map, and retain an earlier encoding when a profile has already shipped. Do not add an
unregistered-profile fallback.

## Open questions

- Final object-subject syntax remains the TAAP protocol gap described in
  [features/25](../features/25-identity-attestation-and-transparency.md#the-gap-you-need-to-close).
  `subject_kind` records the intended kind while object-subject protocol work is pending.
   `slsa_provenance/1` uses the interim result-body digest documented in
   [provenance.md](provenance.md#interim-taap-object-subject).
- Build-input policy consumes the registry exactly as documented in [policy.md](policy.md); it has
  no unregistered-profile fallback.
