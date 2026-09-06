# Public headers

Installed C++ headers in this directory define libliquid's supported API. `liquid.hpp` is the
umbrella include; individual headers are independently compilable. The detailed status and API
inventory is maintained in `docs/library/api-map.md`.

`abi.hpp` defines synchronous linked-binary extraction and diffing values. Repository-backed ABI
lookup remains asynchronous in `repository.hpp`, and wrapper compatibility events are part of
`build.hpp`. Private ELF, Mach-O, and PE reader details do not enter the installed headers.
