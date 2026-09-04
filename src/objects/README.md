# Object records

Canonical CBOR encoders and decoders for store records live here. Attestations use the
profile registry in `src/attestation/`; `scan_result/1` retains its original fixed record shape
while `reproducibility/1` carries the verifier, sandbox, agreement, and optional differing outputs
as typed attributes, while generic profiles carry typed attributes and optional disclosure
commitments.
`slsa_provenance/1` carries typed `buildDefinition` and `runDetails` maps in canonical SLSA field
names. Every signature is excluded from the canonical signing body.

Build result version 3 carries non-overlapping queued, sandbox, exec, hash, and store durations plus
the complete local/remote/race schedule: timing history, size class, byte counts, queue depth, cache
locality, batch fields, estimates, and reason. Producer actor, execution bounds, provenance digest,
SBOM-attestation digests, and the schedule are covered by the canonical TAAP signature. The decoder
accepts version 2 and maps version 1's upload duration to store time. Cache hit/miss diagnostics are
request state and stay outside those immutable bytes. `build_trace` records preserve action results,
dependency edges, phase timing, cache outcomes, schedules, and build wall time. Canonical `sbom`
records hold the unsigned base SPDX or CycloneDX document.
The scheduling-record encoder and decoder are also reused as one length-delimited field in the
private worker protocol, keeping the signed result and submitted placement decision byte-identical.
`action-index-root` records persist sorted candidate/result entries, divergence, and the optional
verification-attestation digest plus ordered trace digests so the disk projection can be rebuilt
and collection can traverse every published result, attestation, and trace.

Blob version 2 appends a whole-content digest and optional remote source/instance to the existing
chunk arrays. Action version 2 carries the opt-in remote-execution eligibility flag; default-off
actions and local blobs keep their version 1 canonical bytes.
