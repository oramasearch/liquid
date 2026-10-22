# liquid

**liquid is the next generation of Git.**

Git got one thing profoundly right: address content by its hash, and integrity and distribution
fall out for free. Then it stopped at source code. It does not know what a build is, cannot hold
a large file without help, cannot merge two repositories, and starts history only when you
remember to commit.

liquid takes the same idea the whole way. One content-addressed store holds source, binaries,
packages, images and search indexes. One graph of pure actions builds, packages and deploys
them. History is not something you create; it is every write you make, signed.

```sh
liquid init .                      # any folder becomes a repository
# ...edit files. Every write is captured as a signed drip. There is no `add`.
liquid cut "parse dates correctly" # name the work: a reviewable change
liquid wrap cmake --build build    # build with a cache that explains its misses
liquid project oci "$change" ...   # ship what you built, with signed provenance
```

**Status:** the current release is **v0.2.0** (2026-10-17). Its
[release notes](docs/releases/v0.2.0.md) say what changed since the first release, `v0.1.0`, whose
[notes](docs/releases/v0.1.0.md) list everything liquid implements. liquid is a C++26 library,
**libliquid**, and a `liquid` command-line binary, on macOS and Linux. The repository is
private and no licence has been chosen yet.

The service image templates use non-root runtime layers; their
[image contract](docs/build/service-images.md) covers state permissions and verification.

## Contents

- [What Git left on the table](#what-git-left-on-the-table)
- [The thesis](#the-thesis)
- [Drips and changes](#drips-and-changes)
- [What you get because one tool owns the whole stack](#what-you-get-because-one-tool-owns-the-whole-stack)
- [Coming from Git](#coming-from-git)
- [Build it and get a first result](#build-it-and-get-a-first-result)
- [A short tour](#a-short-tour)
- [What liquid does today](#what-liquid-does-today)
- [What is new, and what is not there yet](#what-is-new-and-what-is-not-there-yet)
- [The design axioms](#the-design-axioms)
- [Library and binary](#library-and-binary)
- [Where liquid sits](#where-liquid-sits)
- [Documentation](#documentation)
- [Contributing](#contributing)
- [Licence](#licence)

## What Git left on the table

| With Git | With liquid |
|---|---|
| History begins when you commit. Work between commits is unrecorded. | **Every write is a drip.** Capture is automatic and continuous, so there is no step at which history begins. |
| Large files need a second system bolted on. | **One chunk store for everything.** A multi-gigabyte asset and a source file are stored the same way, streamed in bounded memory, and deduplicated across near-identical versions. |
| Two repositories cannot be merged with history intact. Submodules are a workaround. | **Repositories compose.** Nest one inside another, split a subtree out with its signed history, combine two unrelated codebases. |
| A conflict is a broken working tree full of markers. | **A conflict is a value.** It is recorded, listed, and resolved deliberately. |
| A diff is lines of text. | **Diff understands structure**: a function that moved, a formatting-only edit, and the inside of a ZIP, Parquet, SQLite or office file. |
| Signing is optional and rare. | **Every drip and every change is signed** by an identified person or agent, in software or on a hardware token. |
| A secret pasted and then deleted is still in history. | **Scanning runs on the drip**, blocks by default, and history can be redacted with verification. |
| Undo is an expert skill. | **Undo is a command**: the last operation, a named one, or everything a session did. |
| Git knows nothing about building what it stores. | **The same tool builds it**, caches every action, proves the result is reproducible, and signs where it came from. |

## The thesis

**Every tool liquid replaces is independently rediscovering the same three mechanisms, and
each is crippled by owning only one layer of the stack.**

Git content-addresses source but knows nothing about builds. Build systems content-address
actions but know nothing about source history or packaging. Package managers content-address
packages but have no incrementality inside a build. Container images content-address layers so
coarse that the point is lost. Code search must reindex whole repositories because it does not
own the stream of changes.

Each of these is a partial implementation of one idea. liquid's bet is that doing it once,
properly, across all the layers is worth more than the sum of the parts, **because the
boundaries between the layers are where the losses are.**

1. **One content-addressed store, chunked.** Source, artifacts, images, the search corpus and
   encrypted content are all chunks in the same store.
2. **One dependency graph of pure, observed actions.** An action is a function from an input
   tree to outputs. What it depends on is observed by a sandbox, not declared by a person.
3. **Lazy materialisation everywhere.** Nothing is written to disk until it is read: checkouts,
   build outputs, artifacts, container file systems.
4. **The drip.** Every write to disk is a versioned change. This makes the unit of history
   equal to the unit of authoring, which is exactly what an incremental build wants: each drip
   invalidates the minimum part of the graph.

The fourth is what nothing else has, and the reason is that it needs the storage layer, the
build engine and the collaboration layer to be one system. That is the thesis.

The full argument, drawn from 27 research topics, is the
[research synthesis](docs/research/99-synthesis/README.md).

## Drips and changes

| Name | What it is |
|---|---|
| **liquid** | The tool: the command-line binary and its daemon. |
| **drip** | One captured write to disk. The atom of history: automatic, continuous, never explicit, and signed by whoever made it, human or agent. |
| **change** | A named, reviewable group of drips. What Git would call a commit or a pull request. |
| **lake** | Where liquid collects: the hosted service. Its own project. |
| **liquify** | The IDE. Its own project. |

Drips are the record; changes are the narrative. You do not stage, and you do not commit. You
work, and when a piece of work deserves a name you `cut` it.

## What you get because one tool owns the whole stack

Each of these is expensive or impossible for a tool that owns only one layer. For liquid they
are close to free, because the information already exists.

| Capability | Why it is nearly free for liquid |
|---|---|
| **Signed provenance and SBOMs** | liquid already records every input, the toolchain and the outputs of every action. The attestation is the build record. Everyone else infers it or writes it by hand. |
| **A build cache that explains itself** | A miss names the first input that differed: the file, the line range, the drip. |
| **An end to "build the world"** | Outputs are content-addressed, so a rebuild that produces identical bytes stops the cascade instead of invalidating everything downstream. |
| **Rebuilding one function** | The build is per definition, on the same graph the history feeds. Edit a function body and that definition is the only action that runs. |
| **`./configure` answers from a cache** | A probe's result is a function of compiler, sysroot and target, so it is recorded once and reused. |
| **Search inside what you ship** | The registry that takes a `.deb` or a wheel apart for storage also indexes it. |
| **Continuous ABI checking** | One extraction feeds packaging, safe substitution and hot-update classification. |
| **Packages and images from one install tree** | A `.deb`, an `.rpm`, an OCI image and a Helm chart are projections of the same checked tree, byte for byte reproducible. |

## Coming from Git

You do not have to leave Git behind to try liquid.

```sh
liquid import git /path/to/repo   # reachable history and identities come with it
liquid export git /path/to/out    # write a standalone Git repository back out
```

Import preserves reachable local history and maps identities. Export reports exactly what
could not be carried over. See [Git interop](docs/library/git-interop.md).

**Where Git has a command for the same thing, liquid uses Git's name.** Where it differs there
is a reason, and typing the Git name tells you what to use: `liquid commit` answers with
`liquid cut`. The full map, with the reason for every difference, is
[liquid and Git's commands](docs/cli/git-commands.md).

| You know | In liquid |
|---|---|
| `git init`, `git clone` | `liquid init`, `liquid clone` |
| `git add` + `git commit` | Nothing, then `liquid cut "message"` |
| `git log`, `git diff`, `git blame` | `liquid log`, `liquid diff`, `liquid blame` |
| `git push`, `git pull` | `liquid push`, `liquid pull` |
| `git mv`, `git rm` | `liquid mv`, `liquid rm` |
| `git grep` | `liquid search` |
| `git reflog` and a careful reset | `liquid ops`, `liquid undo` |
| Submodules, subtree, filter-repo | `liquid nest`, `liquid split`, `liquid combine` |
| Merge conflicts | `liquid conflicts`, `liquid conflict`, `liquid resolve` |
| `git gc` | `liquid gc` |

## Build it and get a first result

Use a recursive checkout with access to the private submodules. Install Clang 17 or newer
(Homebrew LLVM on macOS), CMake 3.28 or newer, Ninja, OpenSSL and zlib. Linux also needs
libfuse3. Configuration downloads pinned source dependencies. The details are in
[local prerequisites](docs/build/local.md#prerequisites).

From the checkout root, build the binary and record a file in a fresh example directory:

```sh
git submodule update --init --recursive
cmake -S . -B build -G Ninja && cmake --build build -j 6
mkdir build/readme-example
printf 'first line\n' > build/readme-example/notes.txt
(
    export LIQUID_HOME="$PWD/build/readme-identity"
    cd build/readme-example
    ../liquid init .
    ../liquid cut "first version"
    ../liquid log
)
```

`init` imports `notes.txt`. `cut` prints `change=<digest>`. `log` shows that digest and
`first version`. The repository lives in `build/readme-example/` and its local signing
identity in `build/readme-identity/`. The library is `build/libliquid.a` and the binary is
`build/liquid`.

Run the tests with `ctest --test-dir build`.

## A short tour

Each of these is a guide whose commands run as tests, so they stay true.

**Capture an edit, compare, search, and sync**
([your first repository](docs/guides/first-repository.md)):

```sh
liquid init .
initial=$(liquid cut 'initial files')
# ...edit a file; the daemon captures the write as a drip...
changed=$(liquid cut 'add the second line')
liquid log
liquid diff "$initial" "$changed"
liquid search 'second line'
liquid push "http://127.0.0.1:$port"
```

**Wrap a build you already have**
([wrap an existing build](docs/guides/wrap-an-existing-build.md)). Nothing is added to the
project. The second run reuses compile and link results before their tools start, and
`--explain` says why anything missed:

```sh
liquid wrap cmake -S . -B build -G Ninja
liquid wrap --jobs 8 --explain cmake --build build
liquid why build/output.o          # why this output missed the cache
liquid provenance build/output.o   # the signed record of how it was built
```

The wrapper observes what each tool reads through a mount, so it needs libfuse3 on Linux or
macFUSE on macOS.

**Package ordinary files**
([package repository files](docs/guides/project-from-files.md)). No build configuration and no
mount are needed:

```sh
liquid project deb "$change" --name example --version 1.0.0 --arch arm64 --out ../out
liquid project oci "$change" --name example --version 1.0.0 --arch arm64 --out ../image
```

Two more guides cover [keys and encryption](docs/guides/keys-and-encryption.md) and
[resolving packages from declarations](docs/guides/resolve-packages.md). Every command and flag
is in the [generated reference](docs/reference/cli/README.md).

## What liquid does today

This is what the library and binary implement now, not what is planned. The authoritative
record, with the limits of each area, is the
[implementation status](docs/library/status.md), and the
[release notes](docs/releases/v0.1.0.md) list every feature and all 80 commands.

**Version control**

- **A repository from any folder**, with automatic signed drips, named changes, references,
  history, path restore and per-line attribution.
  [Object model](docs/library/object-model.md)
- **Repository composition**: nest, split with history intact, and combine unrelated
  codebases. [Split](docs/library/split.md)
- **Conflicts as values**, and structural merge of concurrently edited source.
  [Conflicts](docs/library/conflicts.md), [structural merge](docs/library/structural-merge.md)
- **Undo** of an operation or a whole session, **exclusive locks** on paths that need them,
  and **private sessions** in which an agent can work speculatively.
  [Multiplayer](docs/library/multiplayer.md), [locks](docs/library/locks.md)
- **Push, pull, clone and a sync server**, and a **drive** that keeps a folder in sync for
  people who do not think in changes. [Sync](docs/library/sync.md), [drive](docs/library/drive.md)
- **Git import and export.** [Git interop](docs/library/git-interop.md)

**One store for everything**

- **A content-addressed chunk store** with bounded-memory streaming for files of any size,
  near-duplicate compression, garbage collection that explains what it keeps, and a
  deterministic crash simulator. [Store](docs/library/store.md), [collection](docs/library/gc.md)
- **Working trees two ways**: a plain directory, or a lazy mount that fetches a file's bytes
  when it is read. [VFS](docs/library/vfs.md)
- **Diff and search inside files**: ZIP, tar, HDF5, Parquet, SQLite, PDF, office documents,
  notebooks, wheels, JARs and more. [Formats](docs/library/formats.md)
- **Search** by ranking, substring or regular expression, with a planner that reports what it
  chose and why. [Search](docs/library/search.md)

**Identity and security**

- **Signed history**: every drip and change carries a TAAP/1 signature, from a key in software
  or on a FIDO2 or PIV token. [Identity](docs/library/identity.md)
- **Encryption that keeps deduplication**: whole objects by trust domain, or just the leaf
  values of a JSON or `.env` file, with key custody by passphrase, recipient key, cloud KMS,
  Shamir shares or a TPM. [Encryption](docs/library/encryption.md),
  [leaf encryption](docs/library/leaf-encryption.md)
- **Secret scanning** that blocks by default, and verified redaction from history.
  [Secret scanning](docs/library/secret-scanning.md)
- **A transparency-log and TUF client**, trusted timestamps, threshold release approval, and
  a **Kubernetes admission** webhook that refuses an image without the evidence a policy asks
  for. [Transparency](docs/library/transparency.md), [admission](docs/library/admission.md)

**Building**

- **`liquid wrap`** turns an unchanged CMake, Autotools or Make build into cached, sandboxed
  actions whose inputs are observed, not declared. [Build engine](docs/library/build-engine.md)
- **Reproducibility is checked, not assumed**: cached actions are re-executed in fresh
  sandboxes, and a result that diverges is quarantined.
- **Provenance, SBOMs and ABI reports** for every build, signed.
  [Provenance](docs/library/provenance.md), [SBOM](docs/library/sbom.md), [ABI](docs/library/abi.md)
- **It explains itself**: `why` for a cache miss, `why-slow` for the critical path, and a
  trace of every build.
- **Remote execution and caching** over REAPI, with liquid's own scheduler and workers. Stock
  Bazel can use liquid as its remote cache. [Distribution](docs/library/distribution.md),
  [REAPI](docs/library/reapi.md)
- **An incremental engine**: a persisted query graph, per-definition C++ builds, structural
  diff and edits, symbols, and hot update of a running process.
  [Incremental engine](docs/library/incremental-engine.md), [hot update](docs/library/hot-update.md)
- **liquid builds liquid**, twice, with a full cache hit the second time and byte-identical
  output. [Self-host](docs/library/self-host.md)

**Packaging and deployment**

- **One install tree, many formats**: `.deb`, `.rpm`, `.apk`, Homebrew bottles, OCI images,
  Helm charts, AppImages, Flatpak, Snaps, macOS packages, MSIX, OSTree commits and bootc
  images, each deterministic and each with signed provenance.
  [Projection](docs/library/projection.md)
- **Dependency resolution** at Spack's level of expressiveness, with liquid's own solver and
  an explanation of any conflict. [Resolution](docs/library/resolution.md)
- **The `.liquid` configuration language**, typed and declarative, and an importer for Spack
  package universes. [Configuration](docs/library/configuration-language.md),
  [Spack import](docs/library/import-spack.md)
- **Atomic installs and rollback**, artifact deltas, and a client that deploys to the
  operator's control plane. [Deploy](docs/library/deploy.md)

## What is new, and what is not there yet

Most of liquid is a careful synthesis of ideas that others have proven. Three things are not,
and they carry the real risk:

1. **First-class repository composition.** Nesting, splitting and merging unrelated
   repositories with history intact. It had to be in the object model from the start.
2. **Version control and the build under one incremental graph.** Each is proven alone; they
   had not been combined. This is what makes building drip by drip possible.
3. **Hot update driven by what the build knows changed.** The honest scope is the lower tiers,
   with a clean fallback to restart.

And what is not there yet, stated plainly, because the specifications describe more than is
built:

- **The build wrapper, verification, replay and lazy mounts need FUSE**: libfuse3 on Linux,
  macFUSE on macOS. Without it those commands report a documented unsupported result.
- **Windows** builds, but returns that unsupported result for the daemon and the build
  wrapper.
- **Two exit demonstrations have not been run**: caching across two machines with attestation
  (Phase 2), and a recompile in the time a save takes (Phase 4).
- **Git is not beaten on merit alone.** It has the network. Import and export are the on-ramp,
  and they have to stay lossless enough to trust.
- **The hosted service and the IDE** are separate projects, lake and liquify.
- **No licence has been chosen.**

Every known limit is in one place in the
[implementation status](docs/library/status.md#known-limits-in-one-place), and the known issues
of this release are in the [release notes](docs/releases/v0.1.0.md#known-issues).

## The design axioms

These hold across every feature. If a proposed feature violates one, the feature is wrong.

1. **One content-addressed chunk store.** Source, binaries, packages, images, search indexes
   and encrypted data are all chunks in one store. There is no "large file mode" because there
   is no distinction to make.
2. **One dependency graph of pure actions.** An action is a pure function from an input
   Merkle tree to outputs, addressed by digest. Builds, packaging, indexing and deployment are
   all actions.
3. **Dependencies are observed, never declared.** The sandbox records what an action actually
   read. Under-declaration becomes impossible; over-declaration becomes unnecessary.
4. **Outputs are content-addressed.** A rebuild that produces identical bytes stops the
   cascade.
5. **Nothing is materialised until it is read.**
6. **Reproducibility is enforced, not aspirational.** Caching, substitution and provenance
   all depend on it.
7. **Provenance is a byproduct.** Every action already records everything an attestation
   needs. Signing is automatic.
8. **Everything works offline, degraded but correct.** Remote cache, remote execution and the
   artifact service are accelerators, never requirements.
9. **Every write to disk is a versioned change.** Drip, drip, drip.
10. **Every change is attributable.** Every drip carries a signature from an identified party,
    human or agent, and history is append-only and tamper-evident.
    [TAAP/1](docs/standards/taap-1-spec-draft.md) is the security model.

The axioms and the commitments they serve are set out in the
[feature index](docs/features/README.md).

## Library and binary

**libliquid is the product; the binary is a client.** Everything under `include/liquid/` is
the public API, and `src/main.cpp` only parses arguments and calls it. Other programs link the
same library: liquify does, and lake compiles it in.

| Target | What it is |
|---|---|
| `liquid::liquid` (`libliquid`) | The library: `libliquid.a`, or a shared library with `-DBUILD_SHARED_LIBS=ON`. `find_package(liquid CONFIG REQUIRED)` works from an installed prefix. |
| `liquid` | The command-line binary. `-DLIQUID_BUILD_CLI=OFF` for library-only consumers. |
| `liquid-tests` | The GoogleTest suite, on by default when liquid is the top-level project. |

Consuming the library is covered in [libliquid](docs/library/libliquid.md), its headers in the
[public API map](docs/library/api-map.md), and its coroutine model in
[runtime](docs/library/runtime.md).

## Where liquid sits

liquid is an Orama project. It is one module of
[liquid-bridge](https://github.com/oramasearch/liquid-bridge), which holds the action plan and
the decisions, and tracks work from its
[master epic](https://github.com/oramasearch/liquid-bridge/issues/7).

| Project | What it is |
|---|---|
| **liquid** (this repository) | The tool and the library. |
| [**lake**](docs/features/18-lake-service.md) | The hosted service: where liquid collects. Repositories, drives, deployment, identity and secrets over one store. It compiles libliquid in. |
| [**liquify**](docs/features/21-liquify-ide.md) | The IDE, built on the same incremental graph as the build. |
| [k0s](https://github.com/oramasearch/k0s) | A private copy of a minimal Kubernetes distribution that liquid deploys to. |

How liquid relates to Orama's wider platform is mapped in
[feature 23](docs/features/23-metis-and-zetta.md).

## Documentation

This page is the introduction. Every section above hands off to a page under `docs/` for the
detail, and the [documentation index](docs/README.md) lists them all.

| I want to… | Read |
|---|---|
| Understand why it is one tool | [Research synthesis](docs/research/99-synthesis/README.md) |
| Read what liquid is specified to do | [Feature specifications](docs/features/README.md) |
| Check what is implemented, and the limits | [Implementation status](docs/library/status.md) |
| See what a release contains | [Releases](docs/releases/README.md) |
| Follow a workflow end to end | [Guides](docs/guides/README.md) |
| Look up a command or flag | [Generated CLI reference](docs/reference/cli/README.md) |
| Configure, test or install liquid | [Local build](docs/build/local.md) |
| Use the library | [libliquid](docs/library/libliquid.md) and the [public API map](docs/library/api-map.md) |
| Understand a subsystem's design | [Library notes](docs/library/README.md) |

## Contributing

Read [`CLAUDE.md`](CLAUDE.md) and [`AGENTS.md`](AGENTS.md) first. They hold the repository's
rules: the vocabulary, the document structure, the checks to run before finishing, how a
release is cut, the recorded decisions, and the conventions inherited from
[`oramasearch/orama-bridge`](https://github.com/oramasearch/orama-bridge).

When liquid gains or loses a capability, update the
[implementation status](docs/library/status.md) first, then the matching line on this page.

## Licence

The open-source licence has not yet been chosen
([release decision](https://github.com/oramasearch/liquid-bridge/issues/2)).
Third-party notices are in [NOTICE](NOTICE).
