# Linked-binary registry adapters

The existing ELF, Mach-O, and PE inspectors validate headers and expose linked-binary facts.
`codec.cpp` maps those facts to Tier 2 registry entries for sections, exports, and imports. It does
not disassemble or decompile binaries.
