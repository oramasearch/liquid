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
`delegation/1` validates attenuation-only scoped agent authority for session publication and lease
claims.
