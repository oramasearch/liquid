<!-- Raw research source. Generated via Perplexity `perplexity_research` (web-grounded,
     multi-source) on 2026-09-01 for the liquid project. Lightly reformatted only.
     Treat as evidence, not as liquid design decisions - see README.md in this
     directory for the curated analysis and the implications drawn for liquid. -->

# Format-aware structural diffing and dedup of binary container formats, 2024-2026

## 1. `diffoscope`: recursive, format-aware comparison

`diffoscope` builds a **tree of differences**, not a binary patch. It specializes each input into a format-specific `File` comparator; comparators may expose a `Container` whose members are paired and recursively specialized again. It supports fuzzy matching for renamed members and ultimately falls back to text or hexadecimal comparison. The default maximum container depth is **50**, a non-disableable security limit. [web:31][web:155]

| Format | Structural treatment and typical semantic views |
|---|---|
| ZIP | Compare archive metadata/listing, then extract entries as children. Each child may itself become another container. |
| tar | Compare headers-path, mode, owner, timestamps, link targets-and recursively compare members. Compression wrappers such as gzip/xz are handled as an outer, usually single-member container. |
| `.deb` | Descend through the `ar` package into `debian-binary`, `control.tar.*`, and `data.tar.*`; then recurse through the tar members. |
| RPM | Compare RPM headers/tags and extract the cpio payload, then recurse through payload files. |
| ISO 9660 | Compare filesystem metadata/listing and extracted paths, including nested files or archives. |
| PDF | Produce human-oriented views such as metadata and extracted text (`pdftotext` support is documented), while retaining generic binary fallback. [web:240] |
| ELF | Compare parsed headers, sections, symbols, relocations, dynamic data, disassembly and strings-normally through binutils/elfutils-style external tools-rather than treating address-shifted bytes as independent changes. |
| JAR | First a ZIP container; `.class` children can additionally be rendered into Java class/bytecode structure. |
| SquashFS | Compare `unsquashfs`-style filesystem metadata and recursively compare extracted files. |

Architecturally, a comparator normally implements recognition plus one or both of:

1. **`compare_details`**: run deterministic presenters/decoders and line-diff their output.
2. **A container interface**: enumerate/extract members, pair equal paths, optionally pair renamed files using fuzzy hashes, and recursively call the normal specialization machinery.

The resulting report mirrors this nesting-for example, `deb -> data.tar.xz -> tar -> usr/lib/foo.jar -> ZIP -> Foo.class -> bytecode`.

**Fallback chain:** specialized comparisons are preferred; absent optional tools simply reduce semantic detail. Remaining content can be compared as plain text when recognized as text, otherwise by hexdump. `tlsh`, RPM bindings and external utilities are explicitly optional but give more meaningful results; `diffoscope --list-tools` reports the installation's dependencies. [web:31][web:275] This makes `diffoscope` diagnostic rather than a canonicalizer: extraction output and external-tool versions can affect its report, but it does not need to reproduce the original container.

---

## 2. Compression locality

DEFLATE maintains a history window of up to **32 KiB**, while match choices, block boundaries and Huffman coding depend on preceding input. A substitution can therefore alter later dictionary contents and coding decisions; an insertion or deletion also shifts the input. In a single gzip member, divergence can persist for most of the remaining stream even though the uncompressed suffix is identical.

A qualification is important: **ZIP compresses each entry independently**. A change normally destroys locality only inside the changed compressed entry, not every entry. However, a changed compressed length moves later local headers and changes central-directory offsets, producing a large naive byte diff. ZIP's central directory explicitly stores each entry's relative local-header offset. [web:166][web:174]

| Mitigation | Locality gained | Limitations |
|---|---|---|
| ZIP `STORED` entries | Method 0 stores payload bytes uncompressed. Unchanged source runs remain byte-identical, so rolling-checksum or content-defined dedup can resynchronize after insertions. [web:172][web:173] | Larger archive; CRC, lengths, offsets and central-directory bytes still change. |
| `Z_FULL_FLUSH` | Emits pending data and resets DEFLATE state, permitting decompression to restart at that point. Damage or a substitution cannot propagate beyond the next reset. [web:1] | Flush markers and dictionary resets reduce compression; zlib warns that frequent use can degrade it seriously. Fixed input intervals also shift after an insertion unless boundaries are content-defined. |
| `gzip --rsyncable` | Selects periodic synchronization points from rolling input state and resets compression, so a local insertion perturbs only nearby compressed regions before boundaries resynchronize. | Non-standard compressor option, though output remains ordinary gzip; sacrifices some ratio and CPU. |
| `zstd --rsyncable` | Periodically synchronizes compression state with negligible documented ratio impact. [web:7] | Can reduce compression speed, does not work with `--single-thread`, and interacts poorly with long-range mode. |
| `rsync` | Receiver hashes fixed blocks; sender scans every offset using a cheap rolling checksum and confirms candidates with a strong checksum. Thus shifted but byte-identical ranges are reused. [web:184][web:187] | Cannot discover reuse when compression has changed the bytes themselves. `rsync -z` compresses the delta transport; it does not make an already-compressed source locally stable. |
| `zsync` | Publishes checksums and a map from uncompressed blocks to positions in a specially prepared compressed file, then obtains required compressed ranges using HTTP. [web:188][web:195] | Requires server-side `.zsync` metadata and suitably mapped/rsyncable gzip; ordinary HTTP ranges cannot address arbitrary uncompressed positions inside generic gzip. |

**Measured example:** in the zsync experiments, optimized rsyncable gzip reduced the optimum transfer by nearly **25%**; a later result transferred **106 kB with zsync** versus **82 kB with `rsync -z`**, with about **23 kB** of the difference being zsync mapping metadata. [web:140] One reported zstd dataset grew from **513,672,097** to **513,831,761 bytes** under `--rsyncable`: **159,664 bytes, ~0.031%**, although that is workload-specific. [web:142]

---

## 3. HDF5 and NetCDF-4

| Internal feature | Consequence for structural diff and deduplication |
|---|---|
| Superblock | Identifies the HDF5 format and points to root/file-wide structures; depending on format version it contains sizes, versions, addresses and consistency information. HDF5 consists at low level of a superblock, object headers, heaps and B-tree nodes. [web:19] |
| Object headers and links | Groups, datasets, attributes and links are logical objects reached through file-relative addresses. Two logically equivalent construction histories can allocate different addresses and header layouts. |
| Chunked datasets | An array is split into independently stored, equally sized chunks. [web:30] Chunk indexes use B-tree-family structures or newer extensible/fixed-array indexes; changing/extending data can alter both one chunk and index metadata. |
| Filters/compression | Shuffle, checksums, gzip, SZIP and plugin filters operate **per chunk**; accessing a compressed chunk decompresses that chunk. [web:27] Thus one changed cell normally recompresses its entire chunk, not the entire dataset. |
| Free space | Deleted/replaced objects leave holes. Depending on the configured strategy, free space may be reused and optionally persisted; otherwise it can remain unreachable after close. [web:198] Allocation history therefore affects later physical layout. |
| Ordering | Link/attribute creation order is optional. With tracking enabled, iteration follows creation order; otherwise APIs commonly expose name order. [web:207][web:208] Physical header, heap and index order can nevertheless differ with insertion history, library version and repacking. |
| `h5diff` | Compares groups by member names recursively, then dataset/attribute rank, dimensions, datatype and values; it supports exclusions and numeric tolerances. [web:196] It compares logical HDF5 objects, not physical block identity. |
| `h5repack` | Rewrites objects into a new HDF5 file, changing layout, chunk shape and filters and removing unused space. [web:20][web:26] It is useful for normalization, but not a guaranteed byte-canonical round trip. |

Why HDF5 often deduplicates poorly:

- It is ordinarily one **mutable monolithic file**, so object-store or file-level versioners see a wholly new object.
- Backup block boundaries do not necessarily align with HDF5 chunks, object headers or free-space allocation.
- A tiny value update rewrites the complete compressed chunk; compression eliminates sub-chunk byte similarity.
- B-tree/index nodes, checksums, offsets and free-space metadata also change.
- Appending, deleting and recreating objects can relocate later allocations.
- A full `h5repack` may preserve logical data while rewriting nearly every address, index and compressed stream.

This is not an assertion that every one-cell edit changes every byte: an in-place edit with stable chunk allocation can leave most other chunk payloads identical. The problem is that those payloads are not separately named/content-addressed, and physical stability is not part of HDF5's logical contract.

A NASA example changed a dataset's chunk shape from `4x9x30x717`-about **3 MB per chunk**-to `1x1x30x717`, about **82 kB**, illustrating how chunk geometry bounds rewrite amplification. [web:248]

**NetCDF-4:** it layers the enhanced NetCDF data model on HDF5; NetCDF groups, variables and attributes map substantially to HDF5 groups, datasets and attributes. [web:211][web:214] Consequently `.nc` files using NetCDF-4 inherit HDF5 chunking, filters, metadata-allocation behavior and binary-dedup limitations. Not every arbitrary HDF5 file is valid NetCDF-4.

---

## 4. Naturally structural or chunk-addressable formats

| Format | Addressable structure | Diff/dedup characteristics |
|---|---|---|
| Zarr | N-dimensional array split into hyperrectangular chunks; traditionally each chunk is a separate object. Zarr v3 can instead group chunks into shards. [web:61][web:64] | Strongest natural fit: changing one chunk creates/replaces one object plus small metadata. Object-level hashes and manifests can directly reuse unchanged chunks. Sharding trades object count for larger rewrite units. |
| Apache Parquet | File -> row groups -> one column chunk per column per row group -> pages; schema, offsets and statistics are in footer metadata. [web:67][web:75] | Independently compressed pages/column chunks give good scan and structural-diff units, but ordinary Parquet remains one file. Rewriting a row group and footer changes more than the edited values; stable row grouping and writer settings are essential. Official guidance recommends **512 MB-1 GB row groups** and approximately **8 kB pages**, favoring scans rather than tiny-update dedup. [web:74] |
| Arrow IPC / Feather V2 | Schema plus independently framed record batches; IPC file format has a footer and supports random access. Feather V2 is exactly Arrow IPC. [web:108][web:110] | Batch-level comparison is straightforward and mostly zero-copy, but the standard does not content-address batches. Rebatching, dictionary encoding or metadata changes can rewrite much of a file. |
| ORC | Self-contained stripes with index, data and stripe footer; file footer/postscript locate stripes. Default row-index groups are **10,000 rows**. [web:106] | Unchanged stripes can remain stable if writer partitioning is stable. Compression blocks, stream encodings and final footer still make byte-level edits writer-dependent. |
| TileDB | Writes create immutable dense or sparse **fragments**, internally divided into attribute/data tiles. [web:109][web:112] | Updates naturally add fragments rather than mutate an old monolith; timestamps and metadata resolve overlapping versions. Consolidation rewrites fragments and changes physical dedup identity. |

"Diff-friendly" here means **structural units are independently decodable**, not necessarily that `git diff` or generic block dedup will automatically recognize them. Zarr's object-per-chunk layout exposes the units directly; Parquet, ORC and Arrow usually require a format-aware reader.

---

## 5. Higher-level versioning: compare manifests, not bytes

| System | Versioning unit and mechanism | Dedup/diff implication |
|---|---|---|
| Apache Iceberg | Each snapshot references a manifest list; immutable Avro manifests enumerate data/delete files with partition data and metrics. [web:256] | Commits add metadata and new immutable data/delete files while reusing references to all unchanged files. Snapshot diff is set difference over manifest entries. |
| Delta Lake | Ordered `_delta_log` commits contain file-add and file-remove actions; periodic Parquet checkpoints summarize state. [web:58] | Logical diff is replayed actions, not Parquet byte comparison. Updated rows may still cause one or more immutable data files to be rewritten. |
| Apache Hudi | Timeline plus file groups/file slices. Copy-on-write produces new base files; merge-on-read appends delta logs and later compacts them. [web:264][web:266] | MoR reduces small-update write amplification; CoW and compaction deliberately replace larger files. |
| lakeFS | Versions object-store paths through metadata pointers; branches are zero-copy, and a changed object receives a new physical version. [web:122][web:125] | Reuses unchanged whole objects, not unchanged blocks inside a modified HDF5/Parquet object. |
| DVC | Git tracks small `.dvc` metadata while data resides in a content-addressable cache, usually keyed by a 32-character MD5-derived hash. [web:295][web:297] | Identical whole files are stored once. A one-byte change to a monolithic binary normally creates a completely new cached object; directory outputs deduplicate unchanged member files. |
| Project Nessie | Git-like branches/tags and commits over catalog `Content` keys; commits record put/delete/unmodified operations for table metadata state. [web:286][web:287] | Versions table pointers atomically across tables; underlying Iceberg/Delta/Hudi supplies data-file immutability and manifests. Nessie is principally a catalog, not block storage. |

These systems avoid demanding canonical byte identity from an entire table. Their efficient case is:

```text
new snapshot = old immutable files
             + newly written files
             - logically removed files
             + small commit/manifest metadata
```

---

## 6. Structural diff for other formats

| Format | Appropriate structural comparison |
|---|---|
| SQLite | `sqldiff database1.sqlite database2.sqlite` compares database content and emits SQL-like changes rather than comparing pages, free lists or B-tree allocation. [web:78] For replication/versioning, a canonical SQL dump or changeset is often preferable to raw database bytes. |
| Jupyter notebook | `nbdime` compares the notebook's nested JSON model-cells, sources, metadata and outputs-can elide base64 images, and supplies terminal/web views and three-way merge. [web:76][web:82] Normalize execution counts, transient metadata and generated outputs when they are not semantically relevant. |
| OOXML / ODF | `.docx`, `.xlsx`, `.pptx` and `.odt` are ZIP packages containing XML parts plus media. Unzip, pair package paths, canonicalize XML namespaces/attributes/whitespace, and separately compare media. Tools such as `odiff` and `zipdiff` follow this model. [web:77] Relationship IDs, timestamps and ZIP member order are common noise. |
| PDF | Compare several views: extracted text for content, normalized object trees for internals, metadata separately, and rendered pages for visual equivalence. `diff-pdf` renders/compares pages and defaults to **300 dpi**. [web:228][web:229] Object numbers, stream compression, incremental updates and xref layout make raw bytes especially misleading. |
| Protocol Buffers | Parse using the correct schema and compare typed fields: maps as maps, repeated fields in order unless the application says otherwise, and unknown fields explicitly. Protobuf deterministic serialization is **not canonical** across schema, build or library changes, so serialized hashes are fragile. [web:98] |

---

## 7. Unpack -> canonicalize -> tree-diff -> repack

A robust generic pipeline is:

```text
identify
  -> losslessly unpack into named parts
  -> decode metadata and payloads
  -> remove or isolate volatile fields
  -> canonicalize ordering, numbers, text and XML/JSON
  -> hash/diff the resulting tree
  -> optionally rebuild the container
```

| Git mechanism | Role | Constraint |
|---|---|---|
| Clean filter | Converts a worktree binary/container into a canonical blob when staging. | Must be stable: logically identical inputs should produce identical canonical output. |
| Smudge filter | Reconstructs the convenient worktree representation on checkout. | For transparent use, it should be an inverse of clean at the semantic level. |
| `diff.<driver>.textconv` | Converts each binary blob to text on demand solely for display by `git diff`. The converter receives a filename and writes text to stdout. [web:91] | One-way and display-only: it neither changes stored blobs nor provides structural merge. |
| External diff/merge driver | Can unpack both sides into temporary trees and invoke XML-, JSON-, image- or domain-specific comparison. | Must define member identity and conflict semantics, not merely lines. |

Git applies **clean before staging** and **smudge on checkout**. [web:96] A practical design is often to store the canonical representation permanently and generate the binary only as a build artifact; that avoids requiring Git filters on every client.

### Why deterministic repacking is the hard part

To reproduce identical bytes, the repacker must control all of the following:

- member ordering, path spelling and Unicode normalization;
- timestamps, UID/GID, modes and host/platform fields;
- compression implementation, version, level, dictionaries, threads and block boundaries;
- padding, alignment, free-space placement and allocation order;
- checksums, central-directory offsets, PDF object numbers/xrefs and HDF5 addresses;
- generated UUIDs, salts, nonces and signatures;
- XML namespace prefixes and attribute ordering;
- float/NaN representation and schema defaults.

Some formats do not specify one canonical encoding at all-Protobuf explicitly does not-and mutable formats such as HDF5, SQLite and PDF incremental updates encode construction history. Therefore the generally reliable objective is:

> **canonical semantic tree + deterministic export profile**, not arbitrary lossless byte-for-byte round-tripping.

If exact preservation is required, retain the original binary alongside the canonical tree or store unchanged opaque members verbatim and rebuild only modified parts.

## Citations

[1] https://www.zlib.net/manual.html
[7] https://man.archlinux.org/man/zstd.1.en
[19] https://support.hdfgroup.org/documentation/hdf5/latest/_f_m_t11.html
[20] https://support.hdfgroup.org/documentation/hdf5/latest/_h5_t_o_o_l__r_p__u_g.html
[26] https://support.hdfgroup.org/releases/hdf5/v2_0/v2_0_0/documentation/doxygen/_view_tools_edit.html
[27] https://ntrs.nasa.gov/api/citations/20180008456/downloads/20180008456.pdf
[30] https://support.hdfgroup.org/documentation/hdf5/latest/improve_compressed_perf.html
[31] https://diffoscope.org/
[58] https://tech-insider.org/apache-iceberg-vs-delta-lake-vs-hudi-2026/
[61] https://zarr-specs.readthedocs.io/en/latest/v3/core/index.html
[64] https://guide.cloudnativegeo.org/zarr/intro.html
[67] https://parquet.apache.org/docs/file-format/
[74] https://parquet.apache.org/docs/file-format/configurations/
[75] https://medium.com/@liam.su8/details-you-need-to-know-about-apache-parquet-d21ff720465e
[76] https://nbdime.readthedocs.io/
[77] https://wiki.documentfoundation.org/Development/Analyzing_Documents
[78] https://sqlite.org/sqldiff.html
[82] https://nbdime.readthedocs.io/en/latest/merging.html
[91] https://www.kernel.org/pub/software/scm/git/docs/gitattributes.html
[96] https://git-scm.com/book/ms/v2/Customizing-Git-Git-Attributes
[98] https://protobuf.dev/programming-guides/serialization-not-canonical/
[106] https://orc.apache.org/specification/ORCv0/
[108] https://arrow.apache.org/docs/python/ipc.html
[109] https://docs.tiledb.com/main/background/internal-mechanics/writing
[110] https://arrow.apache.org/faq/
[112] https://people.csail.mit.edu/stavrosp/papers/vldb2017/VLDB17_TileDB.pdf
[122] https://docs.lakefs.io/concepts/internals/
[125] https://docs.lakefs.io/resources/faq/
[140] https://zsync.moria.org.uk/paper/ch03s05.html
[142] https://blog.gslin.org/archives/tag/zstd/
[155] https://manpages.ubuntu.com/manpages/bionic/man1/diffoscope.1.html
[166] https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT
[172] https://interfaceware.com/flow/technical/code/zip/spec
[173] https://www.fileformat.info/format/zip/corion.htm
[174] https://docs.fileformat.com/compression/zip/
[184] https://rsync.samba.org/tech_report/node2.html
[187] https://www.jfranken.de/homepages/johannes/vortraege/mirroring/tech_report.pdf
[188] https://zsync.moria.org.uk/paper200501/ch03.html
[195] https://zsync.moria.org.uk/paper/ch03s02.html
[196] https://support.hdfgroup.org/documentation/hdf5/latest/_h5_t_o_o_l__d_f__u_g.html
[198] https://support.hdfgroup.org/documentation/hdf5-docs/advanced_topics/FileSpaceManagement.html
[207] https://docs.h5py.org/_/downloads/en/stable/pdf/
[208] https://support.hdfgroup.org/documentation/hdf5/latest/group___o_c_p_l.html
[211] https://docs.unidata.ucar.edu/netcdf-c/current/faq.html
[214] https://docs.unidata.ucar.edu/nug/current/file_structure_and_performance.html
[228] https://github.com/vslavik/diff-pdf/blob/master/README.md
[229] https://github.com/marketplace/actions/diff-pdf
[240] https://lists.reproducible-builds.org/pipermail/rb-commits/2020-April/005077.html
[248] https://ntrs.nasa.gov/api/citations/20180008456/downloads/20180008456.pdf
[256] https://iceberg.apache.org/spec/
[264] https://hudi.apache.org/docs/concepts/
[266] https://hudi.apache.org/docs/next/table_types/
[275] https://diffoscope.org/
[286] https://projectnessie.org/develop/kernel/
[287] https://projectnessie.org/develop/spec/
[295] https://doc.dvc.org/user-guide/project-structure/internal-files
[297] https://doc.dvc.org/user-guide/project-structure/dvc-files
