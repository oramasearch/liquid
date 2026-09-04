# Object records

Canonical CBOR encoders and decoders for store records live here. Attestations use the
profile registry in `src/attestation/`; `scan_result/1` retains its original fixed record shape
while `reproducibility/1` carries the verifier, sandbox, agreement, and optional differing outputs
as typed attributes, while generic profiles carry typed attributes and optional disclosure
commitments.
`slsa_provenance/1` carries typed `buildDefinition` and `runDetails` maps in canonical SLSA field
names. Every signature is excluded from the canonical signing body.

Build results carry the producer actor, execution bounds, provenance digest, and canonical TAAP
signature. Cache hit/miss diagnostics are request state and stay outside those immutable bytes.
`action-index-root` records persist sorted candidate/result entries, divergence, and the optional
verification-attestation digest so the disk projection can be rebuilt and collection can traverse
every published result and attestation.
