# CMake support

`liquidConfig.cmake.in` defines the installed package dependencies.

`Grammars.cmake` reads the canonical tree-sitter lock, enforces archive hashes through
`FetchContent`, applies the optional mirror and subset settings, and generates the private registry
translation unit. `VerifyGrammars.cmake` is the CTest-side per-file digest check. The lock format and
maintenance procedure are documented in [`docs/library/grammars.md`](../docs/library/grammars.md).

`appimage_runtimes.S.in` embeds the vendored, SHA-256-pinned upstream type 2 runtimes as private
read-only data. CMake copies the checked inputs from `third_party/appimage-runtimes/` through
`file://` URLs, so AppImage projection appends its SquashFS without a host runtime tool or a
network request. Provenance and updates are documented in
[`docs/library/projection.md`](../docs/library/projection.md#vendored-runtime-provenance).
