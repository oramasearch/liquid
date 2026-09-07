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
at most 32 MiB of resolved delta objects in an LRU, retaining verified small bases when an
oversized object is encountered, including bases resolved inside a delta chain, and verifies every
object SHA-1 before it is used.
The real-world gate measures memory that the process actually owns: `phys_footprint` on macOS and
`RssAnon` on Linux. `import_report::peak_anonymous_bytes` samples that value while history is
recorded. Physical footprint is the macOS gate; RSS is not used because it includes reclaimable
mapped pack and index pages. This avoids treating mapped files as retained import memory.

## Compatibility

Export flattens nested repositories into ordinary Git trees where their content is available. It
adds a `Liquid-Change: blake3:…` trailer to exported Git messages. Conflict values and signatures
have no Git representation and are reported in `export_report::degradations`. Git LFS pointers
and `.gitattributes` are ordinary files. Tree entries use Git's bytewise ordering, comparing a
directory name as though it ends in `/`; this is required for `git fsck --strict`.

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
physical footprint of at most 512 MiB and an import of at most 35 minutes, compares the
`.liquid-store` store to no more than 1.5 times the `.git` size, exports its head ref, runs `git fsck`,
and compares the multiset of every reachable commit tree hash before and after export.

### Real-world measurement

The nlohmann/json checkout measured 5,497 commits. A clean `origin/main` build at
`eb64454a9f2ad12872a6b79dc203f4b4f5855886` exceeded its prior 15-minute ceiling at 1,773
seconds, with a one-minute host load of 6.99. The gate is therefore 2,100 seconds. The earlier
measurement completed in 676 seconds; its `.git` directory was 285,736 KiB and the resulting
`.liquid-store` was 906,672 KiB (3.17 times). At 120 seconds, macOS `footprint` reported 85 MiB
physical footprint; the importer reported a 168,338,496-byte physical-footprint peak. These are
the memory measurements used for the gate.

The earlier 963,832 KiB storage measurement broke down as 896,944 KiB of packs, 42,100 KiB of
indexes, 23,152 KiB of `objects.meta`, and 1,636 KiB of other files.

The checkout changes a roughly 900 KiB header in most commits. Resemblance-selected zstd
dictionary frames now compress near-duplicate chunks against an ordinary digest-addressed base.
Git import scopes a bounded 256-entry/16-MiB dictionary cache by full repository path, avoiding
base reads and base re-hashing while history is recorded. The gate reports the store-to-Git ratio
plus pack, index, and object-catalog sizes so regressions remain attributable. The #133 measurement
that establishes the new bound is recorded in the pull request and replaces the earlier result
above after the gate completes.

`git rev-list --all --count` reported 5,497 commits, while `git rev-list develop --count` reported
5,054. The difference is reachable only from other remote refs, not unreachable packed objects.
Export writes every imported ref, so those commits are retained in the exported repository too.

`liquid import git <path>` imports into a repository rooted at `<path>`; it creates the liquid
repository when needed. `liquid export git <path> [ref]` exports the repository rooted at the
current directory. The client uses the normal command table and prints import counts and every
reported export degradation.
