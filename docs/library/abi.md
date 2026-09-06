# Binary ABI records

Every successfully published shared-library output has one canonical `abi/1` record. The record is
bound to the output blob digest, referenced by result record version 4, and available through
`repository::abi`. ABI extraction is synchronous CPU work; executor and wrapper publication call it
through `runtime::offload` before signing the result.

## Descriptor

The canonical CBOR record has `v=1`, `kind=abi`, the library-blob `subject`, `binary`,
`architecture`, optional `install_name`, `symbols`, and `needed` fields. `binary` distinguishes
ELF, Mach-O, and PE. Each exported symbol contains:

- `name` and optional ELF `version`;
- `kind`, either `function` or `object`;
- `size`, with zero meaning that the format did not provide a reliable size;
- `binding` and `visibility`;
- an optional normalized debug `signature`.

The needed-library list comes from ELF `DT_NEEDED`, Mach-O load-dylib commands, or the PE import
directory. The install name comes from ELF `DT_SONAME`, Mach-O `LC_ID_DYLIB`, or the PE export
directory's DLL name.

Publication recognizes an ELF `ET_DYN` file only when it has a soname, a Mach-O `MH_DYLIB` only
when it has an ID command, and a PE image only when it has both the DLL characteristic and an
exported DLL name. Executables, objects, static archives, and unrecognized outputs receive no ABI
record. A remote worker extracts records before publishing its signed result; an importing client
uses those transferred records without hydrating lazy output bytes.

## Binary readers

The private readers in `src/formats/binary/` validate offsets before following them and return plain
value types. They expose architecture, section headers, dynamic exports, library identity, and
needed libraries. ELF additionally combines `.gnu.version`, `.gnu.version_d`, and
`.gnu.version_r` with `.dynsym`. ABI extraction uses defined, externally visible dynamic symbols;
it does not infer exports from printable strings.

The implemented DWARF subset reads ELF `.debug_info`, `.debug_abbrev`, `.debug_str`,
`.debug_line_str`, and `.debug_str_offsets`. It handles DWARF 2 through 5 compilation units, common
fixed and variable forms, base types, typedefs, pointers, references, qualifiers, structures,
classes, unions, enumerations, function return types, and formal parameters. Aggregate names carry
their byte size when present. A variable's type supplies its object size when the ELF symbol table
does not. Unknown forms or type references make a signature or fallback size unknown instead of
guessing.

This is intentionally smaller than a full debug type graph. It does not interpret split DWARF,
location expressions, bit-field layout, calling-convention attributes, C++ template semantics,
PDB types, or Mach-O dSYM bundles. Stripped libraries still receive descriptors; their signatures
remain absent.

## Compatibility rules

`diff_abi(old, current)` is deterministic and sorts changes by symbol and change kind. Its verdict
is:

- `compatible` when the exported surface is unchanged;
- `compatible_additions` when the only changes are added exports;
- `incompatible` when an old export is removed, changes symbol version, changes between function
  and object, changes a known signature, or changes an object's size.

A signature change is reported only when both descriptors know the signature. An object-size
change is reported only when both formats provide a nonzero size. Function code-size changes are
not ABI changes. An added symbol does not override an incompatible change elsewhere.
The report change kinds are `added`, `removed`, `type_changed`, `size_changed`, and
`version_changed`.

## Publication and attestation

`action_result.abis` references each output descriptor. When an earlier result exists
for the same output path and has a descriptor, publication compares it with the new descriptor and
issues a registered `abi_compatibility/1` attestation. The output blob is the library subject;
`compatible_with` is the previous blob digest, `compatibility` is the verdict, and `notes` records
the change count. The repository device identity signs the attestation before its digest is placed
in `action_result.abi_compatibility`.

Remote workers transfer both record lists with the signed result. Store reachability follows the
result to descriptors and compatibility attestations, descriptor subjects to their library blobs,
and compatibility claims to the preceding library blob.

`wrap_report.abi_breaks` contains incompatible reports. The default command prints one
`abi-break:` line with at most the first three sorted changes and preserves the build's successful
exit status. `wrap --fail-on-abi-break` returns status 1 after a successful build that reported at
least one break.

## Public API and CLI

```cpp
result<abi_descriptor> extract_abi(std::span<const std::byte> binary);
abi_report diff_abi(const abi_descriptor& old_descriptor,
                    const abi_descriptor& current_descriptor);
runtime::task<abi_descriptor> repository::abi(digest library_blob);
```

`liquid abi show` and `liquid abi diff` accept a regular library path or stored blob digest. A diff
operand may also be `<result-digest>:<output-path>`, which selects a specific build instead of the
newest producer of a path. `--json` emits the stable version-1 descriptor or report projection.

```sh
liquid abi show build/libexample.so --json
liquid abi diff blake3:<old-blob> blake3:<new-blob>
liquid abi diff blake3:<old-result>:build/libexample.so \
  blake3:<new-result>:build/libexample.so --json
```

## Verification

Portable tests decode pinned ELF, Mach-O, and PE fixtures and assert their BLAKE3 digests. Native
shared-library fixtures cover additions, removals, changed signatures, changed object sizes, ELF
symbol versions, and ELF debug signatures. Publication tests retrieve the stored descriptor,
verify the compatibility attestation with `verify_attestation`, and reconstruct the wrapper break.
The privileged Linux wrapper fixture rebuilds a C library incompatibly and checks both the printed
event and `--fail-on-abi-break` status. CLI tests pin the JSON report shape, and the SBOM inference
tests continue to exercise the same evidence model after reader extraction.

The private ELF and Mach-O readers also expose file-offset path references for relocation
publication. ELF includes `DT_NEEDED`, `DT_SONAME`, `DT_RPATH`, and `DT_RUNPATH`; Mach-O includes
install names, needed libraries, and `LC_RPATH`. The public rules and findings belong to
issue #73's relocation check, not the ABI descriptor.

## Deferred

- Full debug type graphs and libabigail-level classification are outside this record.
- Package dependency projection and its compatibility gate belong to issue #28.
- Hot-update classification belongs to issue #32.
- Registry codec registration belongs to issue #70.
