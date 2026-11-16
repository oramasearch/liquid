# Object records

Canonical CBOR encoders and decoders for store records live here. Attestations use the
profile registry in `src/attestation/`; `scan_result/1` retains its original fixed record shape
while `reproducibility/1` carries the verifier, sandbox, agreement, and optional differing outputs
as typed attributes, while generic profiles carry typed attributes and optional disclosure
commitments.
`slsa_provenance/1` carries typed `buildDefinition` and `runDetails` maps in canonical SLSA field
names. Every signature is excluded from the canonical signing body.

Build result version 4 carries non-overlapping queued, sandbox, exec, hash, and store durations plus
the complete local/remote/race schedule: timing history, size class, byte counts, queue depth, cache
locality, batch fields, estimates, and reason. It adds `abi/1` record and
`abi_compatibility/1` attestation references. Producer actor, execution bounds, provenance digest,
SBOM-attestation digests, ABI references, and the schedule are covered by the canonical TAAP
signature. The decoder accepts versions 1 through 3 and maps version 1's upload duration to store
time. Cache hit/miss diagnostics are
request state and stay outside those immutable bytes. `build_trace` records preserve action results,
dependency edges, phase timing, cache outcomes, schedules, and build wall time. Canonical `sbom`
records hold the unsigned base SPDX or CycloneDX document. Canonical `abi` records bind a library
blob to its binary kind, architecture, install name, exports, and needed libraries.
Incomplete ABI inspection adds an optional `truncated=true` field; its absence means false and
complete records retain their canonical bytes. Decoding rejects a non-boolean truncation flag.
The scheduling-record encoder and decoder are also reused as one length-delimited field in the
private worker protocol, keeping the signed result and submitted placement decision byte-identical.
Result record version 4 adds the canonical `output_tree` referenced by the producer signature.
Sandbox record version 2 adds the ordered output-normaliser rule names, binding normalisation
behavior into action identity while preserving version-1 encoding for definitions without rules.
`action-index-root` records persist sorted candidate/result entries, divergence, and the optional
verification-attestation digest plus ordered trace digests so the disk projection can be rebuilt
and collection can traverse every published result, attestation, and trace.

Blob version 2 appends a whole-content digest and optional remote source/instance to the existing
chunk arrays. Action version 2 carries the opt-in remote-execution eligibility flag; default-off
actions and local blobs keep their version 1 canonical bytes.

Change version 2 appends the cut's refs, lock paths, nested-repository pointers, and per-directory
CRDT heads. Version 1 remains byte-compatible for decoding and signature verification.
Change version 3 replaces legacy inline conflict inputs with path-to-conflict-digest references and
adds conflict-to-selected-value resolution references. Canonical `conflict/1` records retain the
base, both sides, exactly one path range, merge kind, and explanation. Optional structural-merge
fields add a conflict kind, proposal blob, ordered reasons, and syntax node without changing the
record version; their absence decodes as the original entry conflict.
