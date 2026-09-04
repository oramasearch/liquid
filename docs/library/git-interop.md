# Git interoperability

**What.** `liquid::import_git` imports a local Git repository with its reachable history and
identity mapping. `liquid::export_git` writes a standalone Git repository from a liquid ref.

**Status.** The verified loose-object and packfile reader (including index v2, OFS_DELTA, and
REF_DELTA) and loose-object writer are implemented. Importer/exporter traversal, command-line
integration, and fixtures follow in this issue.

## Mapping

Git commit names map bidirectionally through repository metadata: `git.commit.<sha1>` stores the
liquid change identifier, and `git.change.<change-id>` stores the Git name. A change also records
`git_commit` in its signed metadata. Git files retain regular, executable, and symbolic-link
modes. Git submodules become nested-repository placeholders.

## Compatibility

Export flattens nested repositories into ordinary Git trees where their content is available. It
adds a `Liquid-Change: blake3:…` trailer to exported Git messages. Conflict values and signatures
have no Git representation and are reported in `export_report::degradations`. Git LFS pointers
and `.gitattributes` are ordinary files.

## Verification

The Git integration tests build a fixture with branches, a merge, a rename, a tag, and packed
deltas using the system Git executable when it is installed. They verify imported history and
snapshots, then run `git fsck` against the export.
