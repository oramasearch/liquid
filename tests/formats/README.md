# Format codec tests

`test_registry.cpp` exercises built-in and loaded codecs through the public format registry;
`test_json.cpp` and `test_dotenv.cpp` cover structured text decoding.

`FormatRegistry.BoundsHdf5FilterPipelinesToTheChunkMask` builds HDF5 v1/v2 filter pipeline
messages with 0, 1, 31, 32, 33, 64 and 255 filters, in compact and chunked datasets. Valid pipelines
preserve the decoded bytes with zero, high-bit and all-bit masks; counts above 32 must return
`errc::corrupt` before chunk decoding. Run with UBSan on `src/formats/hdf5/hdf5.cpp` to check the
mask shifts (liquid #486).
`test_registry.cpp` checks built-in codecs, malformed boundaries, nesting limits, and the byte
budget. RPM cases use small raw-block zstd fixtures to check truncated cpio names, file bytes,
and alignment padding, including trailer acceptance, alongside valid aligned members (liquid
#439). `test_json.cpp` and `test_dotenv.cpp` cover structured configuration codecs.

Build `liquid-format-tests` and run it through CTest with `--timeout 600`. The fixture inventory
is in [`../fixtures/formats/README.md`](../fixtures/formats/README.md).
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
