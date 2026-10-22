# Format tests

`test_macho.cpp` constructs portable Mach-O symbol tables with repeated string offsets. It
checks both nlist widths and byte orders, exported/local/undefined/debugging symbol names,
64 KiB and UTF-8 truncation boundaries, cumulative charges before copies, the 256-name sample boundary,
the default 16 MiB budget, registry caller budgets, partial ABI extraction and comparison,
and SBOM/relocation truncation reporting. Inputs stay
small even when the unfixed inspector would amplify their names.

The JSON, dotenv, and registry tests cover the other codecs and common format contracts.

`test_pe.cpp` builds directly with the PE inspector as `liquid-pe-tests`. It checks capped names,
repeated and overlapping name offsets, one caller/default allocation budget, and partial-result
accounting through `image::truncated` and `image::symbol_bytes` (liquid #474). Its name tables are
constructed in memory from the pinned [`../build/fixtures/abi/`](../build/fixtures/abi/) PE bytes.

`test_pe_registry.cpp` checks registry budget dispatch and truncation propagation. Both PE test
files use `pe_fixture.hpp` for their table builder. Inspector cases also cover PE32, UTF-8 cutoff
boundaries, and the fixed-width/long COFF evidence-name paths.

`test_elf.cpp` builds directly with the ELF inspector as `liquid-elf-tests`. Its in-memory builder
in `elf_fixture.hpp` covers ELF32/ELF64 in both byte orders; section and symbol names; version
definitions/requirements; dynamic library/search paths; inline, indirect, line, and indexed DWARF
strings; repeated/overlapping offsets; exact length limits; and cumulative copy budgets (liquid #479).
`test_elf_registry.cpp` checks caller-budget dispatch, registry truncation, and fail-closed ABI
comparison for a truncated ELF descriptor.

`test_structural_limits.cpp` checks JSON array/object/mixed depth and wide value and key work,
XML paired/self-closing depth, sibling/attribute work, and namespace inheritance without
per-child copies, and Parquet list/set/map/struct depth (including nested map keys), flat
schemas, pre-reservation work limits, and a shared footer/page-header work allowance
(liquid #434). It also builds SQLite interior-page chains and HDF5 group, group B-tree, and chunk
B-tree chains on either side of the depth limit. Notebook recognition and unpacking exercise the
same JSON limit. `test_elf.cpp` also checks
DWARF indirect-form limits for both ELF classes and byte orders, the iterative parent stack,
and shared work across compilation units including null DIEs.
