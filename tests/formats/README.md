# Format tests

`test_macho.cpp` constructs portable Mach-O symbol tables with repeated string offsets. It
checks both nlist widths and byte orders, exported/local/undefined/debugging symbol names,
64 KiB and UTF-8 truncation boundaries, cumulative charges before copies, the 256-name sample boundary,
the default 16 MiB budget, registry caller budgets, partial ABI extraction and comparison,
and SBOM/relocation truncation reporting. Inputs stay
small even when the unfixed inspector would amplify their names.

The JSON, dotenv, and registry tests cover the other codecs and common format contracts.
