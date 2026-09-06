# Software bills of materials

Every new build result carries signed `sbom/1` attestations for each output in SPDX 2.3 JSON and
CycloneDX 1.5 JSON. The component model is derived from the stored action graph. A foreign binary
that has no liquid result uses composition analysis instead, and the resulting document, claim, and
every component say that they are inferred.

| Source | `derived` | `inferred` | Component marker |
|---|---:|---:|---|
| liquid action graph | `true` | `false` | `false` on every component |
| foreign ELF, Mach-O, or PE binary | `false` | `true` | `true` on every component |

Policy can require graph-derived material without interpreting component names or the selected
interchange format.

## One component model

`src/build/sbom.cpp` builds one ordered model before writing either JSON format. For a target
output it starts at the producing result and follows each manifest digest to an earlier indexed
result whose output has that digest. Traversal stops at manifest blobs with no producing result.
The producing result's index sequence is the replay cutoff, and each intermediate result becomes
the cutoff for its inputs. Later cache entries therefore cannot change an existing SBOM, and the
current result cannot identify its own output as an input producer.
The model contains only records reached through this process:

- the requested output and any intermediate output reached by digest;
- every leaf input blob, with its observed path and algorithm-tagged digest;
- every blob leaf in each reached action's toolchain tree, with its tree-relative path and digest;
- each nested-repository tree entry, with the entry's repository id and pinned change digest.

No package-name lookup, lockfile, host inventory, or network result is added to a derived model.
Wrapped toolchain snapshots set both the tree object id and content reference to the stored tool
blob, making the same component graph decodable in static and shared builds.
If a digest is not in an action, result, manifest, toolchain tree, or nested-repository entry reached
from the target, it cannot appear in the derived SBOM.

Relationships also come from the graph. An output is `generated_from` its manifest inputs and
intermediate outputs, `depends_on` its toolchain components, and the root target `contains` the
complete reached component set. Duplicate components and relationships are removed without losing
distinct paths that happen to have the same content digest.

Graph construction indexes every earlier result output by content digest once, then resolves all
observed reads against that index. Reached result and action records, toolchain trees, and input
trees are retained for the duration of the graph build. A multi-output result builds this graph
once and changes only the root output name and digest before encoding each output's two records.
Cache publication and lookup validate the same batch with one graph build. This keeps finalization
proportional to indexed actions plus observed reads plus emitted records instead of multiplying a
full index and tree walk by every read and output. Validation also indexes the result's attestations
by output and format once rather than searching the complete list for each output.

## Records and signatures

Result creation stores both formats before signing the result:

1. Build the component model from the action, result, action index, input trees, and toolchain
   trees.
2. Write the base SPDX or CycloneDX JSON into a canonical `sbom` record.
3. Create an `sbom/1` TAAP attestation whose subject is the output blob digest and whose `document`
   attribute is the canonical SBOM-record digest. Its signed `output` attribute distinguishes
   outputs with identical blob digests.
4. Sign the attestation with the repository device identity and store it.
5. Put every SBOM-attestation digest in `result.sboms`, then create provenance and sign the result.

The profile fields are:

| Field | Meaning |
|---|---|
| `format` | exactly `SPDX-2.3` or `CycloneDX-1.5` |
| `output` | the result-relative output path |
| `document` | algorithm-tagged digest of the canonical `sbom` record |
| `derived` | true only for a model rebuilt from liquid action records |
| `inferred` | true only for binary composition analysis |
| `components` | ordered claims of kind, name, digest or pin, and derivation class |

The signed result reaches the SBOM attestations, and each attestation reaches its SBOM record.
Collection therefore retains the result, claims, documents, action graph, inputs, tools, and
outputs as one graph.

Reading a derived SBOM is verification, not a stored-string lookup. libliquid verifies the result
provenance and the `sbom/1` signature, checks the subject and profile fields, rebuilds the component
model from the stored graph, regenerates the base document, and requires its canonical record bytes
to match. A mismatch returns an unverified report and the CLI refuses to print it.

The emitted document adds a detached-attestation reference through each format's standard
extension point. SPDX uses an `OTHER` annotation. CycloneDX uses metadata properties. Both include
the SBOM record digest, attestation digest, and base64 canonical attestation bytes; the stored base
document stays independent of its signature and avoids a digest cycle.

## SPDX 2.3 mapping

The writer supplies the required document fields `spdxVersion`, `dataLicense`, `SPDXID`, `name`,
`documentNamespace`, and `creationInfo`. Each model component is a package with an SPDX id,
`downloadLocation: NOASSERTION`, `filesAnalyzed: false`, copyright text, and a comment carrying its
liquid kind and inferred marker.

SHA-256 content uses an SPDX checksum. BLAKE3 and keyed BLAKE3 are not SPDX 2.3 checksum algorithms,
so their complete algorithm-tagged values use an `OTHER` external reference named
`liquid-digest`; they are never relabelled as SHA-256. Nested repositories use
`liquid-repository` and `liquid-pinned-change` external references.

The document `DESCRIBES` the requested output. Model relationships map to SPDX `CONTAINS`,
`DEPENDS_ON`, and `GENERATED_FROM`.

## CycloneDX 1.5 mapping

The writer supplies `bomFormat`, `specVersion`, numeric `version`, `metadata.timestamp`, and the
target as `metadata.component`. Other model components use the top-level `components` array.
`bom-ref` values connect the dependency graph, and a complete composition lists the reached
assemblies when there are any.

Each component has `liquid:kind`, `liquid:inferred`, and, when applicable, `liquid:digest`,
`liquid:repo-id`, and `liquid:pinned-change` properties. Supported unkeyed content digests also
use CycloneDX `hashes`. The dependency graph covers all three internal relationship kinds; the
component properties and the shared model retain their exact liquid meaning.

## Foreign-binary inference

`build::infer_sbom` hashes the supplied file and performs local inspection without a
package database or network call. The ELF, Mach-O, and PE parsing now comes from the private
readers documented in [abi.md](abi.md), while the inference evidence remains unchanged. It reads
section names, Mach-O and PE symbol tables, and useful ELF symbol strings; extracts printable embedded
version strings; and checks a built-in signature table for OpenSSL, zlib, glibc, libstdc++, musl,
and libc++.

The target binary is always an inferred component. A recognized library adds another inferred
component and a `contains` relationship. Section, symbol, and version-string observations are
included as analysis evidence in an SPDX document comment or CycloneDX metadata properties.
Absence of a recognized signature is not evidence that a library is absent. This path remains a
small composition analysis; it does not present the linked-binary ABI descriptor as dependency
evidence.

Foreign SBOMs use the local device identity and the same `sbom/1` profile, but set
`derived=false` and `inferred=true`. The base document is hashed as a canonical `sbom` record in
memory, and the emitted detached attestation names that digest. Every component carries the
inferred marker; there is no mixed inferred/derived document.

## API and CLI

The public entry points are:

```cpp
runtime::task<sbom_report>
generate_sbom(repository&, result_id, std::string output_path, sbom_format);
runtime::task<sbom_report>
generate_sbom(repository&, std::string output_path, sbom_format);
runtime::task<sbom_report>
generate_wrapped_sbom(std::filesystem::path working_tree,
                      std::filesystem::path output_path,
                      sbom_format);
runtime::task<sbom_report> infer_sbom(std::filesystem::path artifact, sbom_format);
```

The CLI resolves a produced path through the current repository or wrapped-project cache. It falls
back to binary inference only when no liquid result exists and the target is a regular file.
Absolute regular-file targets are foreign artifacts by definition.

```sh
liquid sbom build/app --spdx
liquid sbom build/app --cyclonedx
liquid sbom /usr/bin/tool --spdx
```

Exactly one format flag is required. A signature or graph mismatch exits with failure instead of
printing an unverified document.

## Verification

The portable `BuildSbom` tests construct a graph containing two manifest inputs, two toolchain
leaves, a nested repository, and one output. They verify both required JSON structures, every
digest, the repository id and pin, the profile flags, signature verification, and that both writers
use the same component and relationship model. A bounded finalization case publishes 24 producer
actions, then records 96 reads and 24 outputs and requires result creation plus cache validation to
finish within twenty seconds. A system-binary test verifies that the document is never derived and
every component is inferred.

The privileged Linux wrap test generates an SBOM for the unchanged CMake fixture and compares every
manifest digest and toolchain-tree leaf with the component model. CLI tests invoke both format
flags on the wrapped fixture. Run the repository's static, shared, and privileged Linux proof
commands.

## Deferred

- Full archive, package, debug-information, and language-metadata codecs remain Phase 5 issue #31.
- VEX is not part of an SBOM record.
- Publishing SBOMs to lake remains Phase 6 work.

## Open questions

- The final TAAP content-addressed subject syntax remains a protocol decision; `sbom/1` uses the
  artifact digest in the same interim object-subject position as build provenance.
- Package URL and CPE enrichment would require a trusted mapping source and must not weaken the
  graph-only guarantee for derived documents.
