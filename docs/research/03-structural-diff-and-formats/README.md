# Structural diff and container formats

> Evidence: [`sources.md`](sources.md)
>
> **Requirement:** liquid should be able to diff *inside* a zip, and inside other
> binary formats such as HDF5. A byte diff of a container is useless; the
> interesting change is two levels down.

## The general technique

Every workable approach in this space is the same pipeline:

```
identify → losslessly unpack into named parts → decode
         → strip/isolate volatile fields → canonicalise ordering
         → hash and diff the tree → (optionally) repack
```

The first four steps are well understood. **The last one is where everything is
hard.**

## diffoscope: the reference implementation

diffoscope is the most complete existing recursive comparator, and its
architecture is the right one to learn from. Each input is specialised into a
format-specific comparator, which may expose a *container* whose members are paired
and recursively specialised again — to a default depth of 50. A report nests
exactly like the data: `deb → data.tar.xz → tar → usr/lib/foo.jar → ZIP →
Foo.class → bytecode`.

A comparator implements recognition plus either `compare_details` (run a
deterministic presenter, then line-diff its output) or a container interface
(enumerate members, pair by path, fuzzy-match renames, recurse). Missing optional
tools degrade the report rather than breaking it; the final fallback is text, then
hexdump.

The critical distinction: **diffoscope is diagnostic, not a canonicaliser.** It
never has to reproduce the original bytes. liquid, which wants to *version* these
files, does — and that is a categorically harder problem.

## The compression-locality problem

This is why "just diff the zip" fails. DEFLATE keeps a 32 KiB history window, and
match choices, block boundaries and Huffman coding all depend on preceding input.
One changed byte can perturb the rest of the compressed stream even though the
uncompressed remainder is identical.

An important correction to the naive intuition: **ZIP compresses each entry
independently**, so a change corrupts locality only *within* the changed entry.
But the changed compressed length shifts every subsequent local header and every
central-directory offset, so a byte diff still looks total.

Mitigations, in order of usefulness to liquid:

| Mitigation | Effect | Cost |
|---|---|---|
| Store entries uncompressed (`STORED`) | Unchanged runs stay byte-identical; CDC resynchronises perfectly | Larger archive |
| `gzip --rsyncable` / `zstd --rsyncable` | Periodic content-defined compressor resets, so a local edit perturbs only nearby output | Small ratio loss — one measured zstd case was **0.031%** |
| `Z_FULL_FLUSH` | Explicit reset points | Frequent use degrades compression seriously |
| zsync | Server-side map from uncompressed blocks to compressed ranges | Requires prepared metadata; one benchmark: 106 kB zsync vs 82 kB `rsync -z`, of which ~23 kB was zsync's own metadata |

**`--rsyncable` at 0.031% cost is nearly free** and should be liquid's default
whenever it produces a compressed artifact.

## HDF5: the hard case, and why

HDF5 is a filesystem in a file: superblock, object headers, heaps, B-tree-indexed
chunked datasets, per-chunk filters, and a free-space manager.

Why it deduplicates badly:

- It is one **mutable monolithic file**, so any file-level versioner sees a wholly
  new object.
- Compression and filters are **per chunk**, so changing one cell recompresses the
  whole chunk. Chunk geometry bounds the damage: a NASA example changed chunk shape
  from `4×9×30×717` (~3 MB/chunk) to `1×1×30×717` (~82 kB), a 36× reduction in
  rewrite amplification for a point edit.
- B-tree nodes, checksums, offsets and free-space metadata change alongside.
- Object and attribute **ordering is not part of the logical contract** — creation
  order is only tracked if requested — so two logically identical files can have
  entirely different physical layouts.
- `h5repack` normalises layout but is explicitly *not* a byte-canonical round trip.

`h5diff` compares logical objects — groups, datasets, dimensions, dtypes, values,
with tolerances — not physical bytes. That is the right model, and it is what
liquid should surface.

NetCDF-4 sits on HDF5 and inherits all of this.

## Formats that are already right

| Format | Structure | Verdict |
|---|---|---|
| **Zarr** | One object per chunk (v3 adds optional sharding) | **The ideal.** Chunks are already separate, named, content-addressable objects. Change one chunk → one new object plus metadata. |
| **Parquet** | Row groups → column chunks → pages; footer metadata | Good units, but one file. Recommended row groups are 512 MB–1 GB, so the rewrite unit is large. |
| **Arrow IPC / Feather V2** | Framed record batches + footer | Easy to compare batch-wise; batches are not content-addressed, and rebatching rewrites everything. |
| **ORC** | Stripes with independent footers | Stable if the writer partitions stably. |
| **TileDB** | Immutable fragments, resolved by timestamp | Naturally append-only; consolidation rewrites. |

**Zarr's design is what a good binary format looks like from a versioning
standpoint**, and it is not a coincidence — it was designed for object stores,
which forced the same constraints content addressing does.

## The higher-level escape hatch

Iceberg, Delta Lake, Hudi, lakeFS and Nessie all refuse to diff bytes at all.
They version by **immutable file addition plus manifests**:

```
new snapshot = old immutable files + new files - removed files + small manifest
```

This is the same idea as a Merkle tree over a directory, and it is strictly easier
than canonical repacking. Where liquid can push users toward chunked, immutable
layouts, it should.

## Other formats worth handling

- **SQLite** — `sqldiff` compares *content* and emits SQL, not pages. For
  versioning, a canonical dump beats raw bytes.
- **Jupyter** — `nbdime` diffs the JSON model, eliding base64 outputs and
  normalising execution counts. The prior art for "strip volatile fields."
- **OOXML/ODF** — zip of XML. Unzip, canonicalise namespaces/attribute order, diff
  parts, compare media separately. Relationship IDs and member order are noise.
- **PDF** — needs several views: extracted text, normalised object tree, metadata,
  and rendered pages.
- **Protobuf** — parse with the schema; **serialisation is explicitly not
  canonical**, so hashing serialised bytes is unsound across library versions.

## Why deterministic repacking is the hard part

To reproduce identical bytes a repacker must control: member order, path
normalisation, timestamps, UID/GID/modes, compression implementation and level,
dictionaries, thread count, block boundaries, padding, alignment, free-space
placement, checksums, offsets, generated UUIDs and nonces, XML namespace prefixes
and attribute order, and float/NaN representation.

Some formats have no canonical encoding at all. Mutable formats (HDF5, SQLite, PDF
incremental updates) encode their own construction history in their layout.

**The honest conclusion:** aim for a *canonical semantic tree plus a deterministic
export profile*, not universal byte-for-byte round-tripping. Where exact bytes must
be preserved, keep the original alongside the canonical form, or store unchanged
members verbatim and rebuild only what changed.

## Implications for liquid

1. **A format registry of pluggable codecs.** Each codec declares: recognise;
   unpack to a named tree; canonicalise; diff; merge (optionally); and — where
   possible — repack deterministically. This is diffoscope's architecture with
   repack added, and it should be an extension point, not a fixed list.

2. **Three fidelity tiers, declared per format, never guessed.**
   - **Tier 1 — canonical round-trip.** Unpack, store the tree, repack
     deterministically. The original bytes are recoverable. Only for formats where
     this is provably true (uncompressed tar, Zarr, well-behaved zip).
   - **Tier 2 — semantic diff, verbatim storage.** Store the original bytes
     chunked; additionally store a derived canonical tree for diff, merge and
     search. HDF5, Parquet, PDF, SQLite live here. This is the honest default.
   - **Tier 3 — chunk and hexdump.** Unknown formats. CDC still gives dedup.

   Tier 2 is the important one: **do not pretend to repack what cannot be
   repacked.** Storing the original and deriving a view is safe, cheap given CDC,
   and never loses data.

3. **Recursive containers, with the diffoscope depth limit.** A `.deb` inside an
   OCI layer inside a zip must decompose all the way down. Cap the depth; a
   maliciously nested archive is a denial-of-service vector.

4. **Default to `--rsyncable` and prefer `STORED` for liquid-produced archives.**
   At ~0.03% size cost this makes everything liquid emits dedup-friendly. It is
   the cheapest large win available.

5. **Reuse the format registry everywhere.** The same unpacked tree feeds
   structural diff, merge, full-text and code search
   ([19](../19-full-text-search/)), SBOM extraction from binaries
   ([15](../15-binary-introspection/)), and OS package decomposition
   ([10](../10-os-packaging/)). Build it once, as a core service, not as a diff
   feature.

6. **Treat volatile-field stripping as declared metadata.** Timestamps, UUIDs,
   build IDs and execution counts should be named per format and excluded from the
   canonical hash, so that logically-identical artifacts compare equal — which is
   also precisely what reproducible-build verification needs
   ([13](../13-supply-chain-security/)).
