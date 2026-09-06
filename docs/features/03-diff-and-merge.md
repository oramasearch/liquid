# Diff and merge

> Research: [03-structural-diff-and-formats](../research/03-structural-diff-and-formats/) ·
> [01-version-control](../research/01-version-control/)
>
> **Container status: implemented by liquid #29. Source status: implemented by liquid #68.** The registry, ZIP/tar/HDF5/Parquet/SQLite
> codecs, format-aware repository and CLI diff, archive search, derived-entry storage, and
> deterministic tar writer, graph-owned source parse, structural edit script, and labelled text
> fallback are available. Structured merge remains separate work.

## Structural diff for source

liquid parses source with **tree-sitter** and diffs the syntax tree, not lines.
Reindentation, brace movement and line wrapping produce empty diffs. Moving a
function is a move, not a delete plus an add.

The same parse serves incremental compilation ([04](04-build-engine.md)) and symbol
search ([12](12-search.md)). One parse, three consumers.

**Status.** Liquid #72 implements the digest-pinned 49-language grammar registry. Liquid #68
implements the graph-owned canonical parse, GumTree-style matching, public structural field,
explicit structural/text/binary tiers, and `liquid diff --structural`. Structural merge remains
liquid #69 work. Every structural result carries the grammar lock identity.

Text diff remains the fallback for languages without a grammar, and the fallback is
always available.

## Structural merge

Two branches adding different functions to the same region of a file is not a
conflict. Merge operates on the tree.

**A note on the CRDT layer.** liquid's datastore merges concurrent edits
automatically ([17](17-implementation.md)). That is correct for the namespace and
for consumer file sync, and it is *not* sufficient for source code: a CRDT will
happily interleave two concurrent edits into something syntactically valid and
semantically wrong. So:

- The CRDT guarantees **no data is lost** and replicas converge.
- Structural merge decides whether the converged result is **meaningful**.
- Where it is not, liquid raises the result for review as a first-class conflict
  value ([02](02-version-control.md)).

"No conflict" must never silently mean "no attention required."

## Diffing inside containers

liquid diffs *inside* archives and binary formats. A byte diff of a zip is useless;
the interesting change is two levels down.

### The pipeline

```
identify → unpack into a named tree → decode → strip volatile fields
        → canonicalise ordering → diff the tree → (optionally) repack
```

Containers nest, and liquid recurses: a `.deb` inside an OCI layer inside a zip
decomposes all the way down, to a bounded depth (a maliciously nested archive is a
denial-of-service vector).

### Fidelity tiers

Declared per format, never guessed. This is the honest part of the design.

| Tier | Guarantee | Formats |
|---|---|---|
| **1 — canonical round-trip** | Unpack, store the tree, repack deterministically. Original bytes recoverable. | Uncompressed tar, Zarr, well-behaved zip |
| **2 — semantic diff, verbatim storage** | Store original bytes as chunks; *additionally* derive a canonical tree for diff, merge and search. Nothing is lost, and no repack is attempted. | **HDF5**, Parquet, PDF, SQLite, OOXML, ORC, Arrow |
| **3 — chunk and hexdump** | Unknown formats. CDC still deduplicates. | Anything unrecognised |

**Tier 2 is the default for hard formats, and that is deliberate.** Deterministic
repacking requires controlling member order, timestamps, ownership, compression
implementation and level, padding, free-space placement, checksums, embedded UUIDs,
XML attribute order and float representation. Some formats have no canonical
encoding at all — protobuf explicitly does not — and mutable formats like HDF5,
SQLite and PDF encode their construction history in their layout. liquid does not
pretend to repack what cannot be repacked.

### Formats at launch

| Format | Treatment |
|---|---|
| **zip / jar** | Implemented for STORED and DEFLATE members, including ZIP64 and data descriptors. XML-aware OOXML/ODF handling is later work. |
| **tar** | Implemented for ustar, pax, GNU long names/links, gzip and zstd wrappers, plus deterministic uncompressed output. `ar` and cpio are later work. |
| **HDF5 / NetCDF-4** | Logical object comparison in the manner of `h5diff` — groups, datasets, ranks, dimensions, dtypes, values, with tolerances. Not physical bytes: HDF5's layout is not part of its logical contract. |
| **Parquet** | Implemented for row groups, column chunks, statistics, PLAIN/RLE/dictionary encoding, and uncompressed/gzip/zstd pages. Snappy is reported opaque. |
| **SQLite** | Implemented for ordinary rowid table schemas and records; comparison ignores free lists and page placement. |
| **ELF / Mach-O / PE** | Sections, symbols, relocations, disassembly — so an address shift is not a total rewrite ([15](15-consumer-sync-and-web.md) is unrelated; see research 15). |
| **`.deb` / `.rpm` / `.apk` / wheels** | Decompose into files plus metadata. |
| **PDF** | Extracted text, normalised object tree, metadata, rendered pages. |
| **Jupyter** | JSON model, eliding outputs and normalising execution counts. |

### Compression locality

Compression destroys the locality that chunking depends on. For everything liquid
*produces*, it defaults to `zstd --rsyncable` — measured at roughly **0.03%** size
cost in one dataset, which makes it close to free — and prefers `STORED` entries in
archives it generates. A local edit then perturbs only nearby output instead of the
whole stream.

## The format registry

All of the above is a **plugin registry**, not a fixed list. A codec declares:
recognise, unpack, canonicalise, diff, optionally merge, and optionally repack
deterministically. Plugins load across the C11 `liquid_format_module_v1` ABI with canonical-CBOR
operation values, owned output bytes, and progress and cancellation callbacks. Built-ins register
through the same descriptor. See [formats.md](../library/formats.md).

The registry is a core service, not a diff feature. It also serves:

- storage deduplication ([01](01-object-model-and-storage.md)) — decomposing a
  container means the zlib vendored inside a wheel and inside a `.deb` is stored once;
- SBOM extraction from binaries;
- OS package decomposition ([08](08-packaging-and-images.md));
- search inside published artifacts ([12](12-search.md)).

## Volatile fields

Timestamps, UUIDs, build IDs and execution counts are declared per format and
excluded from the canonical hash, so logically identical artifacts compare equal.
This is the same normalisation reproducible-build verification needs
([10](10-supply-chain.md)) — one declaration, two uses.

## Open questions

- **Merge semantics for structured binary formats.** Diffing an HDF5 dataset is
  well-defined; merging two concurrent edits to one is not, and may have no answer
  beyond a conflicted copy.
- **How far to take repack determinism.** Some Tier 2 formats could be promoted to
  Tier 1 with enough work. Which are worth it is an empirical question.
