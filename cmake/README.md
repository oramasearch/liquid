# CMake support

`liquidConfig.cmake.in` defines the installed package dependencies.

`Grammars.cmake` reads the canonical tree-sitter lock, enforces archive hashes through
`FetchContent`, applies the optional mirror and subset settings, and generates the private registry
translation unit. `VerifyGrammars.cmake` is the CTest-side per-file digest check. The lock format and
maintenance procedure are documented in [`docs/library/grammars.md`](../docs/library/grammars.md).
