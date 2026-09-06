# Format readers and codecs

This directory contains private, dependency-free readers and writers used by libliquid. Public
record and command contracts belong under `include/liquid/` and `docs/library/`; callers do not
include these headers.

`binary/elf.cpp`, `binary/macho.cpp`, and `binary/pe.cpp` validate and decode linked-binary headers,
sections, exported symbols, library identity, and needed libraries into the plain value types in
`binary/elf.hpp`. The ELF reader also decodes GNU symbol-version tables and the documented DWARF
signature subset. SBOM inference consumes the readers' compatibility evidence view, while ABI
extraction consumes the structured linked-binary view. Relocation checking consumes file-offset
ELF `DT_RPATH`/`DT_RUNPATH` and Mach-O `LC_RPATH` references from the same validated images.

`json.cpp` and `dotenv.cpp` implement strict structured-text codecs used by repository features.

Each built-in container or data-format codec owns one directory beneath this folder. Its
implementation exposes a C ABI v1 descriptor to `builtin.hpp`; `registry.cpp` registers that
descriptor through the same `format_codec::linked` path used by statically linked external modules.

A codec must bound every size and offset before reading or allocating, honour the registry entry,
depth, and byte limits, and report unsupported objects explicitly. It may not add a format vendor's
library or expose third-party types. Synchronous codec work called from a coroutine runs through
`runtime::offload`.

Every new codec adds a real fixture and passes the common cases in `tests/formats/`: recognition,
unpack, canonicalisation, logical comparison, every truncation boundary, recursion limits, and a
byte-identical repack assertion whenever recognition reports Tier 1. Supported and unsupported
subsets belong in `docs/library/formats.md` in the same change.

`deb/`, `rpm/`, and `apk/` contain the Tier A writers and bounded round-trip readers. Issue #80
adds each format's four native lifecycle-script locations and exposes payload bytes and modes for
generation installation.
`package/` owns common hashes, deterministic zstd, tar, and cpio construction. `deflate/` owns the
deterministic RFC 1951 and gzip write path; zlib is used only for bounded read-side inflation.
These package readers register as semantic-verbatim codecs. Their projection rules are documented
in `docs/library/projection.md`.

`ostree/` owns issue #78's dependency-free GVariant encoder and OSTree repository writer. It emits
canonical file, dirtree, dirmeta, commit, and summary bytes, retains regular-file and symbolic-link
metadata, and writes both `archive-z2` and `bare-user` object stores. `tar/` also writes deterministic
hard-link and symbolic-link entries used by bootc's `bare-split-xattrs` encapsulation.
