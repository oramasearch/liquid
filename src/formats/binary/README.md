# Linked-binary registry adapters

The existing ELF, Mach-O, and PE inspectors validate headers and expose linked-binary facts.
`codec.cpp` maps those facts to Tier 2 registry entries for sections, exports, and imports. It does
not disassemble or decompile binaries.

Mach-O symbol names are borrowed from the string table before allocation and truncated to 64 KiB.
Each retained name copy and symbol record is charged before allocation, including repeated offsets
and the first 256 names kept for compatibility evidence. The one-argument inspector uses a 16 MiB
symbol budget; its two-argument overload accepts the registry caller's byte budget. Exhaustion
stops symbol collection and preserves the facts already read. Long names and exhausted budgets
set `image::truncated`; `image::symbol_bytes` records the charge. The registry subtracts that
charge before retaining section bytes and export entries and propagates truncation to the tree.
These private image fields and the optional budgeted reader in `codec.cpp` are shared contracts
for linked-binary inspectors. ABI, SBOM, and relocation callers use the default inspector budget.
ABI descriptors persist the truncation flag and comparisons fail closed for incomplete lists.
SBOM evidence records `linked-binary-symbols:truncated`; relocation emits a finding and refuses
approval when inspection was truncated. The collected facts remain available to callers.

PE install, export, and import names are capped at 64 KiB and use one cumulative allowance for
retained names and records: the caller's budget, or 16 MiB by default. Capped names and exhausted
budgets mark partial results truncated. Repeated offsets within each name category are skipped
before scanning or copying (liquid #474).

COFF evidence names retain the 160-byte/256-name bounds and share the PE allowance. The budgeted
PE adapter also charges registry import entries before retaining their paths.

ELF inspection caps every section, symbol, version, dynamic path, and DWARF name or generated
signature at 64 KiB (liquid #479). A longer name stops collection before its copy, preserves earlier
facts, and marks inspection truncated. Each copy, including temporary metadata, repeated offsets,
section output copies, and the corresponding records, spends one cumulative allowance before
allocation. There is no refund for discarded temporary copies. The default is 16 MiB; registry
unpacking supplies its byte budget and subtracts inspection charges before copying section data
or entry paths. Registry recognition, ABI, SBOM, and relocation use the same bounded inspector.
The local limit signal is caught only at the inspector boundary, so nested version and DWARF
parsing cannot continue with an uncharged name. Exact 64 KiB names remain supported.
