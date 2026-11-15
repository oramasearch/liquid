# Tree-sitter grammar inputs

`grammars.lock` is the canonical, reviewable identity of the bundled Tier 2 grammar set. It is
consumed only through `cmake/Grammars.cmake`; do not add parser sources to this directory.

Use the scripts documented in [`tools/grammars/README.md`](../../../tools/grammars/README.md) to add,
remove, bump, or patch an entry. A locked patch (the Swift scanner's, liquid#462) is applied by
`cmake/Grammars.cmake` to the fetched file and checked by digest; the source still never lives here.
The contributor-facing lock format, trust position, and generated inventory are documented in
[`docs/library/grammars.md`](../../../docs/library/grammars.md).
