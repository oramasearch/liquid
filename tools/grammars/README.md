# Grammar lock maintenance

Run the lock tool from the repository root with a full upstream commit:

```sh
tools/grammars/add.sh <language> <https://github.com/owner/repository> <40-character-commit>
```

The tool downloads the commit archive, hashes the archive and selected `parser.c`, discovers and
hashes a sibling `scanner.c`, `scanner.cc`, or `scanner.cpp`, records the tree-sitter language ABI,
licence, extensions, exported C symbol, and `queries/tags.scm` digest, sorts the canonical JSON lock,
and regenerates the inventory in [`docs/library/grammars.md`](../../docs/library/grammars.md).

For an existing language, the same command bumps or replaces its entry. Remove an entry with
`tools/grammars/remove.sh <language>`. Both commands rewrite the sorted lock and generated table;
include the removal reason in the pull request. Never edit a digest by hand.

The pull request attests the reviewed upstream repository and commit. Its checklist is complete only
when the archive and file-digest CTest passes, the generated table has no diff after regeneration,
every selected sample parses without an `ERROR` node, and the licence remains acceptable.
