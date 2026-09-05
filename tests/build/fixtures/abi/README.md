# Portable ABI fixtures

The `.hex` files decode to small linked-binary inputs so every host tests all three private readers.
Tests decode them in memory and pin these BLAKE3 identities:

| Fixture | Architecture | BLAKE3 |
|---|---|---|
| `elf.hex` | x86-64 | `9e667685c37374ab7bbd0fd805400186c43a1d253febe33ea738b8829c9a36af` |
| `macho.hex` | arm64 | `642915569438a6fb0410f6e796ba7b90ac1208f9e59095a727dabca36ac836db` |
| `pe.hex` | x86-64 | `4a636f1087f1adbbc990c678d2d0e28f8a583542dfc0a6bc04eea9256ab4c006` |

The ELF file is a freestanding object produced from `abi_fixture_v1.c` with Clang's x86-64 Linux
target. The Mach-O and PE files are minimal deterministic images containing one export, a library
identity, one load dependency, and one section. Hex is used so Git review shows every fixture byte.
The native shared-library targets are rebuilt by each CI platform's toolchain and cover the host's
full linked form, including GNU symbol versions and DWARF on Linux.
