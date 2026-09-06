# Format readers

This directory contains private, dependency-free readers and writers used by libliquid. Public
record and command contracts belong under `include/liquid/` and `docs/library/`; callers do not
include these headers.

`binary/elf.cpp`, `binary/macho.cpp`, and `binary/pe.cpp` validate and decode linked-binary headers,
sections, exported symbols, library identity, and needed libraries into the plain value types in
`binary/elf.hpp`. The ELF reader also decodes GNU symbol-version tables and the documented DWARF
signature subset. SBOM inference consumes the readers' compatibility evidence view, while ABI
extraction consumes the structured linked-binary view. Registry codec adapters remain issue #70.

`json.cpp` and `dotenv.cpp` implement strict structured-text codecs used by repository features.
