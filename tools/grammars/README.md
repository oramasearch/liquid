# Grammar lock maintenance

Run the lock tool from the repository root with a full upstream commit:

```sh
tools/grammars/add.sh <language> <https://github.com/owner/repository> <40-character-commit>
```

The tool downloads the commit archive, hashes the archive and selected `parser.c`, discovers and
hashes a sibling `scanner.c`, `scanner.cc`, or `scanner.cpp`, records the tree-sitter language ABI,
licence, extensions, exported C symbol, and `queries/tags.scm` digest, sorts the canonical JSON lock,
and regenerates the inventory in [`docs/library/grammars.md`](../../docs/library/grammars.md).

For an existing language, the same command bumps or replaces its entry. A bump carries the entry's
locked patches forward and stops if one no longer applies; `--no-patches` drops them once the new
upstream commit carries the fix.

To fix a locked grammar's fetched source in place (liquid#462), add an exact-text patch at its
locked commit:

```sh
python3 tools/grammars/manage.py patch <language> <path> <find> <replace> <reference>
```

The tool downloads the locked archive, checks its digest, applies the entry's patches and the new
one (`find` must occur exactly once), and records each file's digest before and after. The build
applies the patch only to a file with the recorded upstream digest. The rules are in
[`docs/library/grammars.md`](../../docs/library/grammars.md), *Locked patches*. Remove an entry with
`tools/grammars/remove.sh <language>`. Both commands rewrite the sorted lock and generated table;
include the removal reason in the pull request. Never edit a digest by hand.

The pull request attests the reviewed upstream repository and commit. Its checklist is complete only
when the archive and file-digest CTest passes, the generated table has no diff after regeneration,
every selected sample parses without an `ERROR` node, and the licence remains acceptable.
