# Format registry and codecs

**Status: implemented by liquid #29, extended by #28, and expanded by #70.** libliquid owns a format registry used by
repository diff, search, storage, and package projection. The built-ins are ZIP, tar, HDF5,
Parquet, SQLite, Debian, RPM, and Alpine packages. They are synchronous CPU codecs; coroutine
callers invoke them through `runtime::offload`.

Issue #70 also registers ELF, Mach-O, PE, PDF, Jupyter, OOXML, ODF, wheel, JAR, safetensors, and
pickle readers. All are Tier 2: their original bytes remain authoritative while logical entries
support search, retention, and container diff.

## Public API and module boundary

[`format.hpp`](../../include/liquid/format.hpp) exposes `format_descriptor`, `format_entry`,
`format_tree`, `format_diff`, `format_codec`, and `format_registry`. A descriptor supplies a stable
name, fidelity tier, extensions, magic prefixes, volatile fields, and resource limits. A tree entry
has a path, kind, logical size and digest, canonical-CBOR attributes, materialised bytes or a byte
range of the original, an optional nested tree, and an explanation when an object is opaque.

`format_registry::global()` contains the built-ins. `find_by_magic`, `find_by_extension`, and
`recognise` identify candidates; `unpack` recursively expands recognised members. Codecs implement
recognition, unpacking, canonicalisation, and logical diff. Merge and repack are optional.
`support_matrix()` projects every live descriptor into `liquid tiers`; new codecs automatically
appear in its JSON and generated format table.

[`format_abi.h`](../../include/liquid/format_abi.h) is the versioned C11 module boundary. A module
exports only `liquid_format_get_module_v1`, returning a `liquid_format_module_v1` descriptor. All
operation inputs and outputs are canonical CBOR. Output bytes carry a release callback owned by the
module, and progress and cancellation callbacks cross the boundary without C++ or third-party
types. Built-in C++ codecs use the same descriptor through `format_codec::linked`; external modules
load through `format_registry::load`.

The v1 operation values are:

| Operation | Input map | Output |
|---|---|---|
| `recognise` | `{blob: bytes, path: text}` | `{recognised: bool, tier: uint, reason: text}` |
| `unpack` | `{blob: bytes, limits: {maximum_depth, byte_budget, maximum_entries}}` | format-tree map |
| `canonicalise` | `{tree: format-tree}` | format-tree map |
| `diff` | `{old: format-tree, new: format-tree, absolute_tolerance: text, relative_tolerance: text}` | format-diff map |
| `merge` | `{base: format-tree, left: format-tree, right: format-tree}` | format-tree map |
| `repack` | `{tree: format-tree}` | `{bytes: bytes}` |

A format-tree map contains `codec`, `tier`, `entries`, `original`, `truncated`, and `limits`. Each
entry contains `path`, numeric `kind`, `size`, `content`, `attributes`, `bytes`, optional
`source_range`, optional `nested`, and `opaque_reason`. A format-diff map contains `codec`, `tier`,
`entries`, and `truncated`; each entry has `path`, numeric `kind`, optional old/new digests, and an
optional nested diff. Absent optionals are CBOR null. The C++ wrapper rejects non-canonical or
malformed results before exposing them.

## Fidelity tiers

| Tier | Registry guarantee |
|---|---|
| 1, `canonical_round_trip` | Repack is available and a codec test proves byte-identical recovery for the recognised input. |
| 2, `semantic_verbatim` | The original remains authoritative; unpacked logical entries support diff, search, and deduplication, but repack is unavailable. |
| 3, `chunk_only` | The registry does not decode the object. Ordinary content-defined storage still applies. |

Recognition may lower one instance below the codec's advertised best tier. ZIP is Tier 1 only for
STORED members; other supported ZIP inputs report Tier 2 because their compressed stream cannot be
re-emitted byte for byte. Uncompressed tar is Tier 1. gzip- and zstd-wrapped tar report Tier 2
because compressor settings cannot be recovered. HDF5, Parquet, and SQLite are Tier 2.

Canonicalisation removes only fields declared by the codec and sorts entries by path. ZIP removes
member modification times, external attributes, and timestamp extra fields. tar removes mtime,
uid, and gid. HDF5 ignores physical object placement, Parquet ignores writer metadata, and SQLite
ignores the freelist and physical page placement.

## Structural parser limits

JSON, XML, Parquet, ELF DWARF, SQLite, and HDF5 readers bound document structure independently
of `format_limits::maximum_depth`, which counts nested archives (liquid #434). The shared private
budget in `src/formats/structural_budget.hpp` permits 64 structural levels, with the root at level
zero, and 1,000,000 work units. Every descent checks depth before it recurses. Exhaustion returns
an error, including from recognition paths that invoke these readers.

JSON charges one unit per value and one per object key. An ordered set detects duplicate decoded
keys in logarithmic comparisons while the value tree preserves source order; escaped spellings of
the same key remain duplicates. The JSON limits apply to every `formats::json::parse` caller, not
only format codecs. XML charges every element and attribute. Each element records only the
namespaces it declares and resolves prefixes by walking outward through its ancestors, so no
element copies the declarations it inherits. Parquet charges fields, values, collection slots
before reservation, and schema nodes to one allowance spanning the footer and all column page
headers. Lists, sets, maps, and structs each check depth; flat schema group traversal checks its
own depth against the same limit.

ELF decodes indirect DWARF forms iteratively and caps both form chains and the iterative DIE
parent stack. Compilation units, abbreviations, attributes, DIEs (including null entries), and
forms share one inspection work allowance across compilation units. Structural exhaustion is an
inspection error, so ABI, SBOM, relocation, and registry callers receive the same failure.
Existing 16-level type-reference traversal remains bounded.

SQLite table b-tree traversal and HDF5 group, group B-tree, and chunk B-tree traversal reject a
64th level. Their cycle checks alone allowed one distinct page or node per level. A chunk B-tree
past the limit makes its dataset opaque, as other unreadable chunk layouts do; a group or SQLite
b-tree past the limit fails the unpack. Archive byte and entry limits continue to apply
separately.

Previously accepted documents beyond these structural or work limits are now rejected. Limits
are fixed parser safeguards, including when recognition occurs before unpack limits are supplied.

## Supported subsets

### ZIP

The decoder reads local headers, the central directory, ZIP64 sizes and offsets, and archives whose
members use data descriptors. STORED and raw-DEFLATE members are decoded with CRC and size checks.
Multi-disk and encrypted archives and compression methods other than STORED or DEFLATE return an
explicit unsupported result.

### tar

The decoder reads ustar headers, pax extended headers, and GNU long-name and long-link records. It
recognises uncompressed, gzip, and zstd wrappers. The deterministic writer sorts entries, reads
`SOURCE_DATE_EPOCH`, writes uid/gid zero, clears names and extended attributes, and normalises file
and directory modes. It writes uncompressed tar for package and image consumers.

### HDF5

The dependency-free Tier 2 reader supports superblocks v0 through v3, object headers v1 and v2,
old symbol-table B-trees, link messages, groups, and compact, contiguous, and version-1 chunked
datasets. Fixed-point, floating-point, and fixed/variable string datatypes are described. Shuffle
and deflate filters are decoded. Dataset attributes record rank, dimensions, datatype, element
size, and layout; comparisons use the logical decoded value bytes rather than file offsets. The
`format_diff_options` absolute and relative tolerances apply element-by-element to 32-bit and
64-bit floating-point datasets; the default zero values preserve exact comparison. An
unknown datatype, link, layout, chunk index, or filter leaves an entry with `opaque_reason` instead
of inventing a value.

### Parquet

The Tier 2 reader implements the Thrift compact protocol for the footer and page headers. It emits
one entry per row-group column chunk with schema, statistics, and decoded values. Data page v1,
PLAIN, RLE/bit-packed levels, and dictionary indices are supported with uncompressed, gzip, and
zstd pages. Snappy and unknown physical types, encodings, page types, or compressors are preserved
as opaque column entries with the reason recorded.

### SQLite

The Tier 2 reader validates the database header, walks table b-tree interior and leaf pages, decodes
the record serial types, reads `sqlite_schema`, and emits schema plus rowid-keyed row entries for
ordinary tables, including overflow payload chains. The freelist is ignored. UTF-16 databases,
index b-trees, and `WITHOUT ROWID` tables are outside this initial subset and return a specific
unsupported result.

### Debian, RPM, and Alpine packages

Issue #28 registers bounded Tier 2 readers for `.deb`, `.rpm`, and `.apk`. Debian decoding checks
the `ar` table, decompresses zstd or deterministic-gzip control and data tar members, and exposes
control text plus payload paths. RPM decoding validates the lead and both header stores, reads name
and version tags, decompresses the zstd payload, and walks cpio newc names. Alpine decoding requires
exactly the control and data gzip members, exposes `.PKGINFO`, and lists data paths. The original
package bytes remain authoritative; complete read-side codecs remain issue #70. Writer and
dependency rules are in [projection.md](projection.md).

### Linked binaries

ELF, Mach-O, and PE entries expose bounded section byte ranges, exported symbols, and imported
libraries using the validated #27 readers. Load addresses are volatile. Disassembly and
decompilation are explicitly out of scope.

Mach-O symbol names are truncated to 64 KiB before copying. A cumulative budget charges each
retained name copy and symbol record, even when many nlist entries share one string-table offset.
Both 32-bit and 64-bit readers, in either byte order, apply the limit to exports and compatibility
name samples. The registry supplies its `format_limits::byte_budget`, subtracts the inspector's
charge before retaining sections and export entries, and marks the tree truncated when a limit
is reached. Other callers use a 16 MiB symbol budget. Inspection preserves facts collected before
exhaustion. ABI descriptors persist truncation and cannot produce a compatible comparison; SBOMs
record it as evidence, and relocation checks report a blocking finding. Names exactly
64 KiB long remain intact; longer UTF-8 names end at a complete character. These limits apply in
the shared inspector used by registry
recognition/unpack, ABI extraction, SBOM inference, and relocation checking (liquid #475).

PE inspection caps each install, export, and import name at 64 KiB, excluding its NUL
terminator (liquid #474). Longer names retain a capped prefix ending at a complete UTF-8 character
and mark inspection truncated.
One allowance covers retained names and their records for the whole inspection and is charged
before allocation. The default is 16 MiB; registry callers supply their remaining byte budget.
Exhausting the allowance stops name collection and marks the partial result truncated.
Repeated file offsets within exports or imports are skipped before scanning or copying, so one
long name cannot be copied once per table entry. Distinct and overlapping offsets still spend the
same allowance. COFF evidence names retain their existing 160-byte and 256-record limits and
share this allowance. These limits apply to registry recognition and unpacking, ABI extraction, and
SBOM inspection through their shared PE reader.

ELF section and symbol names, version definitions and requirements, dynamic library/search paths,
and DWARF names and generated signatures share one cumulative inspection allowance (liquid #479).
Each name is limited to 64 KiB. Collection stops before an oversized name or an unaffordable copy,
retains earlier facts, and marks the result truncated. Copies and records are charged before
allocation, including repeated string-table offsets, temporary metadata, and the second section
name copy in the output. Generated DWARF signatures use the same length and byte limits.
The default allowance is 16 MiB; registry unpacking supplies its byte budget and charges subsequent
section data and entry paths against what remains. All ELF consumers use this inspector, including
recognition, ABI extraction, SBOM inference, and relocation checks. Truncation reaches their existing
partial-result handling and blocks ABI compatibility and relocation approval.

### PDF

The PDF reader recognises indirect objects and simple stream objects, inflates `FlateDecode`,
extracts literal text used with standard text-show operators, and exposes title, author, subject,
and keyword metadata. `/ID`, `/CreationDate`, `/ModDate`, and `/Producer` are volatile. Rendering,
encrypted files, JBIG2, JPX, JavaScript, malformed xref repair, and full font encoding support are
outside the reader's subset.

### Jupyter and model files

Jupyter exposes one entry per cell source while `outputs` and `execution_count` are volatile.
Safetensors exposes JSON-header tensor entries with dtype, shape, content bytes, and source ranges.
Pickle never executes content: the scanner recognises text `GLOBAL` and `INST`, binary unicode and
`STACK_GLOBAL`, and records `REDUCE`, `OBJ`, `NEWOBJ`, and `BUILD` as code-deserialisation-risk
opcodes. It deliberately does not reconstruct objects or execute reducers.

### OOXML, ODF, wheels, and JARs

OOXML and ODF are ZIP views with namespace-expanded XML names and sorted attributes. OOXML `rId*`
relationship attributes and ZIP member order are noise; media remains a separate digest-bearing
entry. Debian and APK control members and payloads are represented below `control/` and `data/`;
RPM payload files are represented below `data/` alongside decoded package name and version. Wheels
retain `.dist-info/METADATA` and report available SHA-256 `RECORD` checks. JARs retain
`META-INF/MANIFEST.MF` and note `.SF`, `.RSA`, and `.DSA` signature members. These views do not
validate XML schemas, office rendering, wheel signatures, or JAR signatures.

## Limits and nesting

The default descriptor limit is depth 8, 256 MiB of decoded bytes, and 100,000 entries. Callers may
lower each value. Every decoder bounds offsets, lengths, allocations, and counts before use. When a
nested member reaches the depth or byte budget, unpack succeeds with `format_tree::truncated` set
and a human-readable item in `format_tree::limits`; it never silently continues. This protects
diff, search, and capture from hostile recursive archives.

Nested paths use exactly one `!/` separator: `archive.zip!/inner/file.txt`. Further containers
append another component, for example `outer.tar!/inner.zip!/file.txt`. Search hits use the same
notation. The hit's blob digest is the logical entry digest, so identical contents share index
files even when their container paths differ.

## Repository integration and measurement

`tree_diff_options::formats` opts into container decoding. A `tree_diff_entry` then carries a
`container_diff` with codec, observed tier, added/removed/modified entry paths, nested diffs, and a
truncation flag. `liquid diff <a> <b> [path] --formats` renders those entries; `--json` preserves the
same shape.

When `repository::write` or capture stores a recognised Tier 1 or Tier 2 container, it also stores
each logical entry as an ordinary blob and records those digests on the parent blob record. Garbage
collection follows those references. The verbatim container remains stored and readable.

The committed `archive-a.zip` and `archive-b.zip` fixtures are 419 and 418 bytes, or 837 verbatim
bytes together. Their six logical members total 169 bytes without derived-entry deduplication. The
content-addressed store retains four unique payloads totalling 107 logical bytes: the repeated
24-byte `stored.txt` and 38-byte `zip64.txt` bodies are each stored once, saving 62 logical bytes.
The repository test also counts one physical store frame for each repeated entry digest.

## SquashFS writer

Issue #77 adds a private SquashFS 4 writer for AppImage and Snap projection. It writes sorted
inodes and directories, zero IDs, fixed timestamps and normalised modes. File data uses 128 KiB
blocks and the pinned zstd compressor when compressed bytes are smaller; fragments, xattrs,
duplicate aliases and export tables are omitted. Metadata blocks are stored uncompressed, making
every inode and directory reference independent of compressor-version output. The writer is not a
format-registry reader: `unsquashfs` provides the external round-trip check.

## Verification contract

`tests/formats/test_registry.cpp` is the registry conformance suite. It covers dynamic C module
loading, recognition and unpack for every launch codec, volatile-field canonicalisation, logical
diff, Tier 1 byte-identical ZIP and tar repack, deterministic tar output, every byte truncation
boundary, and reported recursion limits. Real fixtures and their producing commands live in
[`tests/fixtures/formats/README.md`](../../tests/fixtures/formats/README.md). Repository, search,
and CLI tests prove nested diffs, derived-entry retention, archive search paths, and ZIP/HDF5
rendering.
