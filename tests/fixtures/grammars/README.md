# Grammar fixtures

Each language directory contains the small source sample consumed by `test_grammars.cpp` and
`liquid-grammar-measure`. The registry test requires one sample for every configured lock entry and
rejects any parse containing a tree-sitter `ERROR` node. Required tag-query languages also use
these samples to prove that unit discovery returns a definition or declaration.
