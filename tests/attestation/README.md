# Attestation tests

The registry tests exercise every shipped profile's required schema fields, unknown-profile
rejection, freshness, selective disclosure commitments, signatures, structured SLSA provenance,
and the legacy `scan_result/1` digest fixture. The reproducibility case pins its verifier, sandbox,
and agreement schema. The `sbom/1` example pins the required document digest and mutually exclusive
derived/inferred flags while retaining selective disclosure for component claims.
