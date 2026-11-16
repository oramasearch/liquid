# CLAUDE.md: liquid

The rules for anyone, person or coding agent, working in this repository. `AGENTS.md` points here.

## What this repo is

**liquid**: a version control system, build system, package manager, artifact service and runtime
in one program. See [`README.md`](README.md).

**Status:** the releases are in [`docs/releases/`](docs/releases/README.md); each page says what
changed, what is not included and the known issues.

**Feature status does not live in this file.** The authoritative inventory of what libliquid and
the `liquid` binary implement, by phase, with the known limits, is
[`docs/library/status.md`](docs/library/status.md). A change to what liquid does updates that page
in the same commit. This file records the repository's rules and decisions, and the root
`README.md` is the build and first-result path. Do not add per-feature status here. The
[reconciliation record](docs/library/readme-history.md) explains the earlier duplicate inventories.
Scanner work bounds and their regression coverage are documented in
[`docs/library/secret-scanning.md`](docs/library/secret-scanning.md).

**Shape.** Code is a C++26 **libliquid** (`liquid::liquid`, static or shared, `find_package`
works; see [`docs/library/libliquid.md`](docs/library/libliquid.md)) plus the `liquid` binary as a
thin client of it. The liquify IDE and the Lake service link the same library. liquid never
depends on Zetta or on Lake code: the deployment client uses liquid's own HTTP client and the
provisional wire contract in `contracts/control-plane/openapi.json`.

### Vocabulary: use these words exactly

| Term | Meaning |
|---|---|
| **liquid** | The tool: the CLI and the daemon. |
| **lake** | The hosted service built on libliquid: source hosting, file storage, deployment and model hosting over one store. |
| **liquify** | The IDE: native, cross-platform, its own repository, links libliquid and never depends on Zetta. Not an editor extension. |
| **drip** | One captured write to disk. The atom of history: automatic, continuous, never explicit. |
| **change** | A named, reviewable aggregation of drips. What another system calls a commit or a pull request. |

Drips are the record; changes are the narrative. Do not use "commit" for a liquid
change, and do not use "drip" for anything but a captured write.

## Build and test

Prerequisites are in [`docs/build/local.md`](docs/build/local.md#prerequisites): Clang 17 or newer
(Homebrew LLVM on macOS; Apple's Clang and Ubuntu's GCC 13 are too old), CMake 3.28 or newer, Ninja,
OpenSSL, zlib, and libfuse3 on Linux. Configuration downloads pinned source dependencies.

```sh
git submodule update --init --recursive
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

Build options, install and the liquid-on-liquid build are in [`docs/build/`](docs/build/README.md).

## Document structure: the rule that matters most

Two trees with **different jobs**. Keeping them separate is what makes the research
checkable and the specs honest.

```
docs/research/NN-topic/
    README.md    curated analysis, written for this project
    sources.md   raw web-grounded research, verbatim
docs/features/NN-name.md
    specifications: what liquid does
docs/library/<topic>.md
    implementation and benchmark notes for libliquid contributors
docs/build/<topic>.md
    local and continuous-integration build verification
docs/standards/
    external standards liquid depends on, vendored
docs/reference/cli/
    generated command and flag reference; edit the CLI table, not these pages
docs/guides/
    executable user workflows whose marked shell blocks run in CTest
docs/releases/
    one page per tagged release: what it includes, what it does not, known issues
third_party/appimage-runtimes/
    vendored, hash-pinned AppImage type 2 runtime inputs and their upstream licence
```

### `sources.md` is evidence

- **Verbatim.** Lightly reformatted only. Carries a provenance header stating the tool,
  tier and date.
- **Never edited to fit a conclusion.** If a source contradicts a spec, the spec is
  wrong or the spec needs to say why it disagrees. Do not quietly adjust the evidence.
- **Not liquid's opinion.** Where a source recommends an architecture, that is the
  source's view of the existing landscape, not a design commitment.
- Two topics deliberately have no `sources.md` (`17-runtime-hot-update`,
  `99-synthesis`); both say so.

### `README.md` in a research folder is analysis

How the incumbent works · what is genuinely good · what is broken · what people love and
hate · and a closing **Implications for liquid** section. Cite specifics and numbers from
`sources.md`; a claim with no evidence behind it does not belong here.

### `docs/features/*.md` are specifications

Each states what liquid does, links the research it rests on, and **ends with Open
questions**. State an open question rather than inventing an answer: the open questions
are the most valuable content in the tree.

### `docs/standards/` is vendored, not owned

[TAAP/1](docs/standards/taap-1-spec-draft.md) is **liquid's security model**: every
claim about a user, agent, document, build or artifact is a TAAP attestation
([`docs/features/25-identity-attestation-and-transparency.md`](docs/features/25-identity-attestation-and-transparency.md)).

Three things to know before touching anything that depends on it:

1. **It is a draft**, and liquid would be its first significant implementation. The
   protocol may change under us.
2. **The copy here is a holding position.** TAAP is transport-agnostic and much wider in
   scope than liquid; it should get its own repository. When it does, record the version
   this copy corresponds to and stop treating the vendored file as authoritative.
3. **One open question remains for liquid**: TAAP subjects are key-derived, but documents,
   drips and artifacts are content-addressed. A protocol content-addressed subject form is
   required. Build provenance uses the versioned interim result-body digest in
   [`docs/library/provenance.md`](docs/library/provenance.md) until the spec defines that form.

Do not introduce a second attestation or signing framework. in-toto predicates and SBOM
formats ride *inside* TAAP as profile bodies; they do not get their own envelope.

## Working rules for this repo

Git object readers must preserve the shared byte and delta-depth budgets across offset and
reference bases, including bases in other packs or loose storage. The limits and import
compatibility contract live in [`docs/library/git-interop.md`](docs/library/git-interop.md).

**Check links before finishing.** Every internal link must resolve:

```sh
python3 - <<'PY'
import os,re,glob
bad=[]
for f in glob.glob('docs/**/*.md',recursive=True)+['README.md']:
    d=os.path.dirname(f)
    for m in re.finditer(r'\[[^\]]*\]\(([^)#][^)]*)\)', open(f).read()):
        t=m.group(1).split('#')[0]
        if t.startswith(('http','mailto:')) or not t: continue
        p=os.path.normpath(os.path.join(d,t))
        if not os.path.exists(p) and not os.path.exists(p+'.md'): bad.append(f"{f} -> {t}")
print("broken links:",len(bad)); [print(" ",b) for b in bad]
PY
```

**Keep counts accurate.** `docs/README.md` states topic and feature counts. Recount, never
estimate:

```sh
ls -d docs/research/*/ | wc -l          # topics
ls docs/research/*/sources.md | wc -l   # with sources
ls docs/features/*.md | grep -v README | wc -l
```

Unix-domain socket fixtures use the scoped `tests/socket_fixture.hpp` helper, independently of
repository paths. Native byte limits, temporary-root fallback, and cleanup are documented in
[`docs/build/socket-fixtures.md`](docs/build/socket-fixtures.md).

### Commands follow Git

liquid does not deviate from Git's commands without a good reason (for example `git mv` versus
`liquid move`).

Where Git has a command for the same thing, liquid uses Git's name, arguments and order. A
different name needs a reason recorded in
[`docs/cli/git-commands.md`](docs/cli/git-commands.md), and a hint in `git_hints` in
`src/main.cpp` so that typing the Git name says what to use. `cut` is not `commit` on purpose:
a change names drips that already exist. Every command belongs to exactly one `--help` group;
the build fails if one is missing or listed twice.

### The README is the introduction

The root `README.md` is what someone who has just found liquid reads, with `docs/` pages as the
detail. It says what liquid is (the next generation of Git), argues why it is one tool in the voice
of the [research synthesis](docs/research/99-synthesis/README.md), shows a first result, tours the
workflows, and says what works today and what does not. Rules that keep it good:

- **Every section hands off to a `docs/` page** for the detail. The README states the capability
  in a sentence and links; it does not carry per-issue status, option lists or design notes.
- **Each thing appears once.** Before adding a section, check that it is not already there; when
  a merge conflicts in `README.md`, resolve it by hand and never keep both sides.
- **It claims only what `docs/library/status.md` says is implemented**, and it has a section
  that says plainly what is not there yet.
- **Commands shown are real**: taken from the guides, which run as tests, or checked against
  `docs/reference/cli/`.

### Formatting

`clang-format` with the repository's `.clang-format` (LLVM base, 100 columns, 4-space indents) before
every commit; one statement per line. A function collapsed onto one line is sent back unread.

### Before finishing: run all five
When changing linked-binary symbol inspection, preserve the retained-byte and truncation contract
When changing linked-binary name inspection, preserve the retained-byte and truncation contract
When changing linked-binary symbol inspection, preserve the retained-byte and truncation contract
When changing linked-binary name inspection, preserve the retained-byte and truncation contract
in [`src/formats/binary/README.md`](src/formats/binary/README.md) across inspector and registry callers,
including ELF section, symbol, version, dynamic-path, and DWARF name copies.

### Before finishing — run all five

The first two are commands; the last three are easy to skip. Treat them as one checklist.

1. **Link check** (above): must report zero.
2. **Count sync** (above): recount, never estimate.
3. **Indexes.** A new research topic adds a row to `docs/research/README.md`; a new
   feature doc adds a row to `docs/features/README.md`.
4. **The status record, the docs index, and this file.** Any change to what liquid *is* or
   what it *does* updates `docs/features/README.md`, the implementation status in
   `docs/library/status.md`, and this file. A new top-level directory or a new convention
   updates the root `README.md` and this file. A new `docs/` subdirectory that is not in
   `docs/README.md` is a bug.
5. **Proof is local.** Build and run the suite (`cmake --build build && ctest --test-dir build`)
   and record the counts in the pull request. Do not rely on hosted CI as proof;
   `docs/build/ci.md` describes the workflow file in the repository.

If a change is worth a feature doc, it is worth a row in `docs/README.md` and a line in
`docs/library/status.md`.

**Correct the record when research contradicts a spec.** Say what changed and why, in the doc.
Do not leave a superseded claim standing next to its correction.

**No estimates.** Never preface work with size or duration guesses. Do the task, report
what changed.

## Branches

- **Branch from `dev`** and open every pull request against `dev`. The default branch is `main`, so
  name the base explicitly (`gh pr create --base dev`).
- `main` is the last release. It moves only by the release pull request that promotes `dev` to
  `main`.
- Merge with a merge commit, never a squash, so every commit of a pull request stays in history.

## Cutting a release

A release is a tag on `main`. The pages and their conventions are in
[`docs/releases/`](docs/releases/README.md); read its `README.md` first.

1. **Move the version in all three places**, on a branch from `dev`: `current_version()` in
   `include/liquid/version.hpp`, `version_string()` in `src/version.cpp`, and
   `project(liquid VERSION ...)` in `CMakeLists.txt`. `Version.StringMatchesStruct` fails when
   the first two disagree, `Version.MatchesProjectVersion` when the code and `CMakeLists.txt`
   disagree, and the ctest `release-notes-page` when `docs/releases/vX.Y.Z.md` is missing. The
   library's `VERSION`/`SOVERSION` properties are the ABI version and move only on an ABI break.
2. **Write `docs/releases/vX.Y.Z.md`** and add it to `docs/releases/README.md`: what the release
   includes, what it does not, known issues, and verification. The feature list follows
   `docs/library/status.md`; a specification under `docs/features/` is not a claim that the
   feature shipped. Every number is measured.
3. **Prove it**: build, run the full suite on macOS and Linux, and put the counts in the page. A
   failure is fixed or listed as a known issue, never left out. Run the five finishing checks
   above.
4. **Merge into `dev`, then promote `dev` to `main`** with a release pull request
   (`--base main --head dev`), merged with a merge commit.
5. **Tag the release pull request's merge commit** `vX.Y.Z`
   (`git tag -a vX.Y.Z <merge commit> -m "liquid vX.Y.Z"`), push the tag, and publish the release
   from the page.

## Dependencies

liquid's C++ dependencies are pinned as submodules under `modules/` or fetched at configure time
and pinned by digest:

| Dependency | Use |
|---|---|
| `automerge-cpp` | The MIT-licensed CRDT ([17](docs/features/17-implementation.md)), one document per directory. |
| `oramacore-cpp` | BM25F tokenizer and scorer ([12](docs/features/12-search.md)) for the local index. |
| `corosio` | Networking on Boost.Capy. The pinned copy carries an epoll hang-up fix that is not upstream: consume the pinned submodule, never upstream directly. |
| `http` | HTTP on Corosio, pinned with its patches; same rule. |
| Boost.Capy | Coroutines. Consumed upstream. |

Never add a dependency on Zetta or on Lake.

## Decisions: recorded

1. **liquid does not sit on Zetta.** liquid has its own store, so it stays usable on its own;
   liquify and Lake consume it, never the other way round.
2. **Lake uses Zetta as its data plane and liquid as a library** for repository data.
3. **One CRDT: `automerge-cpp`** (MIT), shared with Zetta.
4. **Async stack: Boost.Capy for coroutines, Corosio for networking.** stdexec is not used.
   Requires **Clang 17+**. **libliquid is coroutine-native on Capy in its public API**: every
   I/O function returns `runtime::task<T>` and Capy is a public dependency through the one
   boundary header `liquid/runtime.hpp`; Corosio does the I/O (files, sockets, timers) behind
   the boundary. No synchronous-first phase, no raw threads.
5. **libliquid is the product; the `liquid` binary, liquify and lake are clients of it.** One
   CMake target `liquid::liquid` (static by default, shared on request, hidden visibility with
   `LIQUID_EXPORT` on the API), installed with a package config so `find_package(liquid)` works,
   and consumable as a submodule with `LIQUID_BUILD_CLI OFF`. Everything under `include/liquid/`
   is public API and must stay free of Zetta and, where feasible, of third-party types (Boost stays
   behind the boundary). The CLI never carries behaviour the library lacks. **The surface is
   `docs/library/api-map.md`**: `result<T>` errors everywhere, opaque handles, plain value records,
   digests that carry their algorithm, and coroutine-native waiting operations; a header not on the
   map is not public. **The object model is `docs/library/object-model.md`**: canonical-CBOR
   records in the store, one small repository document plus one automerge document per directory
   (chosen by measurement), liquid's own `node_id` (survives moves) and `change_id` (the digest of
   the change record, never automerge's `ChangeHash`), conflicts and symbols reserved in the
   change record from the first commit. Directory checkpoints prefer automerge-cpp compact saves,
   fall back to full bytes when a compact load check fails, and reopen sync-capable; drip metadata
   enters `CommitOptions`, while TAAP remains the attribution authority.
6. **The chunk index is liquid's own storage engine, built from scratch.** No third-party
   key-value store in the store: a fixed-width LSM of liquid's own (an in-memory sorted memtable
   flushed to immutable sorted runs with a 256-way fanout and a bloom filter each; size-tiered
   compaction), always rebuildable from the packs, so it is a cache with no write-ahead log of its
   own. The benchmark in `docs/library/chunk-index-benchmark.md` is the bar: at 10M entries, at
   least RocksDB-tuned's insert and point-lookup rates and no more than its footprint. Design in
   `docs/library/store.md`.
7. **Transport is liquid's own: HTTP/2 with gRPC framing and a protobuf codec on Corosio.** No
   `grpc++`, no `protobuf`, no code generator. REAPI v2 is spoken through liquid's own layers, and
   the same HTTP/2 layer serves liquid's own workers. Design: `docs/library/distribution.md`.
8. **Agent application-data isolation uses template copies with a reflink fast path.** Each
   session owns one branch per declared PostgreSQL database, and its sandbox receives the branch
   URL rather than the base. Promotion replaces the base only when a logical fingerprint proves it
   has not changed since branching; liquid does not attempt a data merge.
   `CREATE DATABASE ... TEMPLATE ...` is the portable baseline. A local, quiescent cluster may use
   APFS `clonefile` or Linux `FICLONE` plus a second server. Details:
   `docs/library/data-branching.md`.
9. **TSA signature verification is liquid's own, verification only.** RFC 3161 and RFC 4998
   verification uses liquid's strict DER, CMS, X.509, RSA, and NIST-curve ECDSA readers and
   verifiers. It does not generate keys or signatures and does not link a general-purpose
   cryptography library for these operations. Design: `docs/library/transparency.md`.
10. **Tier 2 tree-sitter grammars are fetched at configure time and pinned by digest.** One
    canonical lock names an exact public-upstream commit, archive SHA-256, generated parser
    SHA-256, every hand-written scanner SHA-256, tag-query SHA-256, language ABI, licence, and
    extensions. Generated parsers do not live in-tree. `LIQUID_GRAMMAR_MIRROR` names an
    organisation or air-gapped mirror. See [`docs/library/grammars.md`](docs/library/grammars.md).
11. **The dependency solver is liquid's own CDCL engine.** Do not vendor Clingo. Package rules
    compile to clauses behind the private `src/resolve/concretizer.hpp` interface, while
    `include/liquid/resolve.hpp` exposes only plain records, tasks, results, digests, and stable
    explanations. Design: `docs/library/resolution.md`.
12. **The runtime node store is embedded liquid, not a containerd snapshotter.** An image root is
    a tree digest plus its sync URL. `liquid node-store serve` receives only tree and blob records,
    exposes a reference-counted read-only VFS mount, and owns the shared node chunk budget. A
    separate proxy snapshotter bind-mounts that path and owns writable layers; liquid ships neither
    the proxy nor layers.
13. **Install trees are position-independent by construction.** The fixed virtual build root is
    not an install location, and installed artifacts contain only tree-relative references. Result
    publication rejects embedded build or declared install prefixes, absolute binary lookup paths,
    and absolute package metadata. Liquid never rewrites existing binaries and does not require a
    fixed content-addressed install path. See [`docs/library/relocation.md`](docs/library/relocation.md).
14. **Hardware command layers are owned by liquid over operating-system transports.** CTAP2 uses
    liquid's canonical CBOR over USB HID, PIV sends APDUs through PC/SC, and the TPM 2.0 sealing
    subset uses `/dev/tpmrm0` or Windows TBS. Tests inject protocol-level software doubles through
    those transport interfaces. Do not add libfido2, OpenSC, or tpm2-tss. Design:
    [`docs/library/identity.md`](docs/library/identity.md),
    [`docs/library/encryption.md`](docs/library/encryption.md), and
    [`docs/library/release-approval.md`](docs/library/release-approval.md).
15. **Tier B source packaging is Debian-only in Phase 5.** A distribution qualifies only when
    liquid can run its reproducible source-build tool and policy linter in a pinned peer
    environment. Debian qualifies through `sbuild` and lintian, and its concrete bar is an unsigned
    package a maintainer could sponsor. RPM `.spec` output remains a Tier A byproduct labelled not
    policy-reviewed. Other source-policy formats are deferred. OpenPGP signing remains the
    sponsor's `debsign` step.

**Still open:**

- **TAAP content-addressed subjects**: the spec needs an object-subject form
  ([25](docs/features/25-identity-attestation-and-transparency.md)). Build provenance uses the
  versioned interim result-body digest documented in
  [`docs/library/provenance.md`](docs/library/provenance.md).

Current implementation coverage and limits live in [`docs/library/status.md`](docs/library/status.md).

## Research method

Research in `docs/research/` is web-grounded and cited. Note the tool and tier used in the
`sources.md` header, so readers know how exhaustive a source is. Research queries cover the public
landscape only.

## Conventions

- **No AI attribution.** Never credit an AI or coding assistant as an author or co-author
  (no `Co-Authored-By` trailer, no "Generated with"), and never mention one in commits, pull
  request titles or bodies, tags or release notes.
- **Banned word: "seam".** Ambiguous, and ambiguity is dangerous in system design. Name
  the actual mechanism: **interface**, **extension point**, **injected dependency**,
  **boundary**, **test double point**.
- **Documentation discipline.** Every change documents itself in `docs/` in the same commit, and
  touching files in a folder updates that folder's `README.md` in the same change.
- **Conventional Commits** for commit messages and pull request titles.
