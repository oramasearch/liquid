# Build provenance

Every liquid action result carries a `slsa_provenance/1` TAAP attestation. The executor and
`liquid wrap` derive it from the stored action and result records, sign its canonical CBOR body
with the repository's device identity, store it beside the result, and bind its digest into the
signed result record. There is no provenance input file and no opt-out.

This is useful because the observed read manifest is already the most accurate account of a
build's dependencies. Turning that record into provenance at result creation preserves the
builder, inputs, command, environment, toolchain, sandbox, outputs, logs, and execution timing
without asking a build author to repeat them in configuration.

## Stored record and signatures

The result record has optional `started_at`, `finished_at`, and `provenance` fields so records
written before this format still decode and keep their original signing bytes. Every new result
sets all three. Result creation proceeds in this order:

1. Load the canonical `action` record and set the result producer to the device identity.
2. Derive the SLSA predicate and the interim object subject from only the action and result.
3. Sign the canonical CBOR attestation body with TAAP's `attestation` signature purpose and store
   the complete attestation.
4. Put the attestation digest in `result.provenance`, sign the result with TAAP's `result`
   signature purpose, and store the result.

The result signature therefore authenticates the attestation reference. The action-cache root
reaches the result, the result reaches the provenance attestation, and collection retains the
whole graph together. Publication and lookup both verify the result and provenance signatures;
a cached hit returns the existing result ID and does not create another signature.

The attestation profile registry treats `slsa_provenance/1` as a structured profile. Its
`attributes` value is the SLSA predicate itself, with `buildDefinition` and `runDetails`; it is not
the older flattened `build_type`/`invocation`/`materials` example. Canonical CBOR map ordering
makes the signing body deterministic.

## Interim TAAP object subject

TAAP/1 does not yet standardize a content-addressed subject. Until that protocol change lands,
the attestation records `subject_kind: "artifact"` and uses the digest of the canonical unsigned
result body with the `provenance` field omitted. Omitting that one field avoids a content-addressed
cycle: the attestation digest can be inserted into the final result while the result signature
binds the two records. Verification recomputes this digest.

This is the documented interim form required by issue #14. When TAAP gains its object-subject
form, introduce a new record/profile version and retain verification for this form; do not silently
reinterpret existing signatures.

## Liquid action v1

The build type is
`https://github.com/oramasearch/liquid/blob/main/docs/library/provenance.md#liquid-action-v1`.
It maps a liquid action and result to the
[SLSA v1 Provenance predicate](https://slsa.dev/spec/v1.2/build-provenance) as follows.

| SLSA field | Liquid source | Encoding |
|---|---|---|
| `buildDefinition.buildType` | profile constant | the build-type URI above |
| `buildDefinition.externalParameters.command` | `action.command` | ordered argv text array |
| `buildDefinition.externalParameters.cwd` | `action.cwd` | fixed-root relative path |
| `buildDefinition.externalParameters.environment` | `action.env` | complete name/value map |
| `buildDefinition.internalParameters.toolchain` | `action.toolchain` | resource descriptor with `liquid:tree:<digest>` URI |
| `buildDefinition.internalParameters.sandbox` | `action.sandbox` | resource descriptor with `liquid:sandbox:<digest>` URI |
| `buildDefinition.internalParameters.platform` | `action.platform` | `{os, arch}` |
| `buildDefinition.resolvedDependencies[]` | every `result.manifest` entry | resource descriptor with `liquid:blob:<digest>` URI and algorithm-keyed hexadecimal digest |
| `runDetails.builder.id` | `result.producer.taap_id` | the builder device's TAAP identifier |
| `runDetails.metadata.invocationId` | interim result-subject digest | algorithm-tagged digest text |
| `runDetails.metadata.startedOn` | `result.started_at` | UTC RFC 3339 timestamp |
| `runDetails.metadata.finishedOn` | `result.finished_at` | UTC RFC 3339 timestamp |
| `runDetails.byproducts[]` | `result.stdout` and `result.stderr` | named `stdout`/`stderr` blob resource descriptors |

Each resource digest map uses the algorithm name (`blake3`, `sha256`, or `blake3k`) as its key
and lowercase hexadecimal digest bytes as its value. Resolved dependencies retain manifest order;
the manifest itself is path-sorted by the executor.

## DSSE and in-toto export

`liquid provenance --dsse` emits the standard JSON envelope fields `payload`, `payloadType`, and
`signatures`. The base64 payload is an in-toto Statement v1 whose predicate type is
`https://slsa.dev/provenance/v1`; its subjects are the result's output paths and blob digests. The
signature entry identifies the TAAP issuer and carries the stored TAAP signature.

The optional envelope fields `liquidAttestation` and `liquidAttestationDigest` preserve the exact
canonical CBOR record and its store digest. They let liquid round-trip the projection to the same
content-addressed attestation while ordinary in-toto readers ignore the extension fields. The
stored form remains the TAAP record; the JSON envelope is never stored as a second source of truth.

## How to verify

From a liquid repository, pass a result digest or produced path. From a wrapped project's root,
pass a produced path and the command opens that project's persistent wrap repository:

```sh
liquid provenance blake3:<result-digest>
liquid provenance build/output.o
liquid provenance --dsse build/output.o
liquid verify build/output.o
```

`build::verify_provenance(repository&, result_id)` verifies the result signature, loads the
referenced attestation, verifies its TAAP signature and profile, checks that issuer and producer
match, recomputes the interim subject, and derives the expected predicate again from the action and
result. It returns the decoded statement and a verification reason. `build::provenance_json`
writes either the native JSON view or DSSE/in-toto projection.

Run the repository's static and shared proof commands. The portable `BuildProvenance` tests cover
the mapping, tamper failure, envelope shape, and canonical-record digest round-trip. The privileged
Linux `BuildCache.identical_second_execution_is_a_hit_without_a_spawn` test proves that a real
execution carries verified provenance and a cached hit reuses its attestation.

## How to change

SLSA field changes start in the typed records in `liquid/attestation.hpp`, their canonical encoder
and decoder in `src/objects/records.cpp`, and the single derivation in
`src/build/provenance.cpp`. Add a profile version for an incompatible meaning. Keep result fields
optional when old signing bytes must remain verifiable, update the native and DSSE JSON projections
together, and add mapping plus tamper tests before changing this table.

## Open questions

- The final TAAP content-addressed subject syntax remains owned by the TAAP protocol change.
- A transparency-log publisher is deferred to Phase 6.
