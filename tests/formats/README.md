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
