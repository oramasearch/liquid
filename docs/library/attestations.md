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
| `delegation/1` | actor | `principal:text`, `scopes:text_list` | `not_after:unsigned_integer` | revocable; fresh status evidence no older than 1 hour |
| `authorship/1` | drip | `author:text` | none | none |
| `review_approval/1` | change | `reviewer:text`, `approved:boolean` | `comment:text*` | none |
| `slsa_provenance/1` | artifact | typed `buildDefinition` and `runDetails` SLSA v1 predicate | none | none |
| `sbom/1` | artifact | `format:text`, `document:text`, `derived:boolean`, `inferred:boolean`, `components:text_list*` | `supplier:text*` | none |
| `delta/1` | artifact | `source:text`, `target:text`, `algorithm:text`, `delta:text` | none | none |
| `model_provenance/1` | model | `training_dataset:text*`, `training_code:text*` | `base_model:text*` | none |
| `abi_compatibility/1` | library | `compatible_with:text`, `compatibility:text` | `notes:text` | none |
| `scan_result/1` | drip | fixed record fields `detectors:text_list`, `findings:unsigned_integer`, `allowances:text_list` | none | maximum attestation age: 24 hours |
| `reproducibility/1` | artifact | `verified_by:text`, `sandbox:text`, `agreement:boolean` | `differing_outputs:text_list` | none |
| `image_provenance/1` | artifact | `image:text`, `tree:text` | `result:text` | none |

`*` marks a field that may be represented by a salted disclosure commitment rather than its
plaintext value. A required field is satisfied by its committed value when the profile permits it.

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
`image_provenance/1` uses the OCI image's SHA-256 digest as its subject. `image` is the complete
digest-pinned reference, `tree` is its `io.liquid.tree` digest, and optional `result` is the build
result projected into the image. [`admission.md`](admission.md) defines its minting and policy use.

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
