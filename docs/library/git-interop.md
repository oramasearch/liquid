# Git interoperability

**What.** `liquid::import_git` imports a local Git repository with its reachable history and
identity mapping. `liquid::export_git` writes a standalone Git repository from a liquid ref.

**Status.** The verified loose-object and packfile reader (including index v2, OFS_DELTA, and
REF_DELTA), importer, loose-object exporter, and command-line integration are implemented.
Complete fixture coverage follows in this issue.

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

The Git integration tests build a 50-change fixture with branches, a merge, a rename, a tag, and
packed deltas using the system Git executable when it is installed. They check the bidirectional
mapping, full history, snapshots, rename node identity, resumable import, corrupt-object refusal,
and `git fsck` against the export. The private object-reader sources are compiled into that
fixture too, so this verification also runs with a shared `libliquid`.
The public-header fixture compiles `liquid/git.hpp` independently.

`liquid import git <path>` imports into a repository rooted at `<path>`; it creates the liquid
repository when needed. `liquid export git <path> [ref]` exports the repository rooted at the
current directory. The client prints import counts and every reported export degradation.
