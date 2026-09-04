# Object records

Canonical CBOR encoders and decoders for store records live here. Attestations use the
profile registry in `src/attestation/`; `scan_result/1` retains its original fixed record shape
while newer profiles carry typed attributes and optional disclosure commitments. Every signature
is excluded from the canonical signing body.

Build results carry the producer actor and canonical TAAP signature. Cache hit/miss diagnostics are
request state and stay outside those immutable bytes. `action-index-root` records persist sorted
candidate/result entries so the disk projection can be rebuilt and collection can traverse every
published result.
