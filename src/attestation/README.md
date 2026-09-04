# Attestation registry

This directory owns libliquid's immutable TAAP attestation profile registry and its
profile-keyed schema, freshness, status, and selective-disclosure verification.
The `reproducibility/1` profile validates the device verifier, exact sandbox digest, agreement, and
optional differing-output digest pairs emitted by the build verifier.
`slsa_provenance/1` is the structured exception to the generic attribute schema and validates a
typed SLSA v1 predicate.
`sbom/1` validates the format, canonical record digest, derived and inferred flags, and component
claims used by SPDX/CycloneDX generation.
