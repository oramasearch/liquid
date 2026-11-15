# Attestation registry

This directory owns libliquid's immutable TAAP attestation profile registry and its
profile-keyed schema, freshness, status, and selective-disclosure verification.
The `reproducibility/1` profile validates the device verifier, exact sandbox digest, agreement, and
optional differing-output digest pairs emitted by the build verifier.
`slsa_provenance/1` is the structured exception to the generic attribute schema and validates a
typed SLSA v1 predicate.
`sbom/1` validates the format, canonical record digest, derived and inferred flags, and component
claims used by SPDX/CycloneDX generation and generated package-script inventories.
`projection/1` optionally binds the lifecycle script names and SHA-256 digests emitted for a
package target.
`delta/1` validates source, target, algorithm, and delta-record digest claims for artifact delivery.
`hot_update/1` validates the old and new results, tier, changed symbols, patch object and record,
and the patched or restart outcome.
`image_provenance/1` validates a digest-pinned image reference, its `io.liquid.tree` digest, and an
optional producing result. `image.cpp` mints that record with the device identity into a selected
store.
`projection/1` optionally binds OS projections to one base tree digest and an ordered list of
overlay tree digests in addition to its target, source, normalisation, and tool fields.
`delegation/1` validates attenuation-only scoped agent authority for session publication and lease
claims. `check_delegation_window` bounds its validity to `not_after` (at most 30 days), and
`local_delegation_status` answers revocation from `$LIQUID_HOME/trust/delegation-revocations`
(liquid #514). `delegation_subject` is the subject a delegation names for its agent, and its
`repository` attribute binds it to one repository (liquid #515).
`platform_signature/1` binds a projected unsigned digest to the signed installer digest,
certificate fingerprint, native verification, and optional notarisation or timestamp evidence.
