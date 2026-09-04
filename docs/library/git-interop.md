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
modes. Git submodules become nested-repository placeholders. Loose, packed, and symbolic refs are
resolved before the reachable graph is walked. Pack offset-to-object lookups use a bounded 32 MiB
index cache; the same cache resolves known object names without reopening an index file. Larger
indexes are scanned without retaining their entries. A bounded eight-file pack-stream cache avoids
reopening the active pack for each object while preserving streamed zlib inflation.

## Import design

Import builds the reachable graph in parent-before-child topological order without replaying
historical edits through directory documents. A Git blob is written once through the content store,
then a liquid blob record is retained in an LRU memo keyed by Git SHA-1. Git trees are converted
bottom-up to liquid tree records and are likewise memoized by Git SHA-1. Both import memos hold at
most 1,000,000 entries; an evicted tree is rebuilt only when another commit requires it. Each
change is recorded from its stored root tree with `record_from_tree`, its mapped parents, Git
author and timestamp, and signed `git_commit` metadata.

The private store catalog batches at most 64 imported commits. Blob and tree bytes and their index
entries are still written as each object arrives; batching avoids rewriting the complete object
catalog for every imported record. If an interruption leaves a metadata mapping ahead of its
catalog entry, the resumed importer verifies the mapped change snapshot and rebuilds that record
when it is absent.

After the history and refs are recorded, import applies only the selected Git head tree to the
working copy and checkpoints it. This keeps history construction independent of directory-document
transactions while leaving the repository ready for ordinary local edits. The pack reader retains
at most 32 MiB of resolved delta objects, retains verified small bases when an oversized object is
encountered, including bases resolved inside a delta chain, and verifies every object SHA-1 before
it is used.
The real-world gate measures memory that the process actually owns: `phys_footprint` on macOS and
`RssAnon` on Linux. `import_report::peak_anonymous_bytes` samples that value while history is
recorded. This avoids treating reclaimable allocator reservations and mapped files as retained
import memory.

## Compatibility

Export flattens nested repositories into ordinary Git trees where their content is available. It
adds a `Liquid-Change: blake3:…` trailer to exported Git messages. Conflict values and signatures
have no Git representation and are reported in `export_report::degradations`. Git LFS pointers
and `.gitattributes` are ordinary files.

## Verification

The Git integration tests build a 50-change fixture with branches, a merge, a rename, a tag, and
packed deltas using the system Git executable when it is installed. They check the bidirectional
mapping, full history, snapshots, rename node identity, resumable import, corrupt-object refusal,
and `git fsck` against the export. Delta instructions are size-checked before reconstruction. The private object-reader sources are compiled into that
fixture too, so this verification also runs with a shared `libliquid`.
The public-header fixture compiles `liquid/git.hpp` independently.

The real-world check is excluded from ordinary `ctest` runs. Configure with
`-DLIQUID_REALWORLD_TESTS=ON -DLIQUID_REALWORLD_GIT_DIRECTORY=/path/to/checkout` and run
`ctest -L realworld --output-on-failure`. It imports the supplied checkout, requires a reported
peak anonymous memory below 1 GiB, compares the `.liquid-store` store to three times the `.git` size, exports its
head ref, and runs `git fsck`.

`liquid import git <path>` imports into a repository rooted at `<path>`; it creates the liquid
repository when needed. `liquid export git <path> [ref]` exports the repository rooted at the
current directory. The client uses the normal command table and prints import counts and every
reported export degradation.
