# CLAUDE.md — liquid

Repo-scoped rules. **Org-wide conventions live in the bridge**
([`oramasearch/orama-bridge`](https://github.com/oramasearch/orama-bridge) `CLAUDE.md`)
and apply here in full. The ones most often hit in this repo are restated below —
the bridge remains authoritative.

## What this repo is

**liquid** — a version control system, build system, package manager, artifact service
and runtime in one program. See [`README.md`](README.md).

**Status:** The current release is **v0.2.0** (2026-10-17);
[`docs/releases/v0.2.0.md`](docs/releases/v0.2.0.md) says what changed since `v0.1.0`, and
[`docs/releases/v0.1.0.md`](docs/releases/v0.1.0.md) is the full inventory. Lake `v0.2.0` is built
from liquid `v0.1.0` (`f669726`), the same library source.

**Feature status does not live in this file.** The authoritative inventory of what libliquid and
the `liquid` binary implement, by phase, with the known limits, is
[`docs/library/status.md`](docs/library/status.md). A change to what liquid does updates that page
in the same commit. This file records the repository's rules and decisions, and the root
`README.md` is the build and first-result path. Until 2026-10-12 this section also carried about
260 lines of per-issue status. The features it described are in `status.md`, and the issue each
came from is cited on its topic page under `docs/library/`; do not add status back here. The
[reconciliation record](docs/library/readme-history.md) explains the earlier duplicate inventories.

**Shape.** Code is a C++26 **libliquid** (`liquid::liquid`, static or shared, `find_package`
works; see [`docs/library/libliquid.md`](docs/library/libliquid.md)) plus the `liquid` binary as a
thin client of it. liquify links the same library as a pinned submodule, and Lake compiles it from
the commit liquid-bridge pins. liquid never depends on Zetta or on Lake code: the deployment
client uses liquid's own HTTP client and the provisional wire contract in
`contracts/control-plane/openapi.json`.

### Vocabulary — use these words exactly

| Term | Meaning |
|---|---|
| **liquid** | The tool: the CLI and the daemon. |
| **lake** | The hosted service at `lake.live`. GitHub + Google Drive + Railway + Hugging Face over one store. |
| **liquify** | The IDE — native, cross-platform, its own repo `oramasearch/liquify` (private for now, open source eventually; never depends on Zetta). Not an editor extension. |
| **drip** | One captured write to disk. The atom of history — automatic, continuous, never explicit. |
| **change** | A named, reviewable aggregation of drips. What another system calls a commit or a pull request. |

Drips are the record; changes are the narrative. Do not use "commit" for a liquid
change, and do not use "drip" for anything but a captured write.

## Document structure — the rule that matters most

Two trees with **different jobs**. Keeping them separate is what makes the research
checkable and the specs honest.

```
docs/research/NN-topic/
    README.md    curated analysis — written for this project
    sources.md   raw web-grounded research, verbatim
docs/features/NN-name.md
    specifications — what liquid does
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
questions**. State an open question rather than inventing an answer — the open questions
are the most valuable content in the tree.

### `docs/standards/` is vendored, not owned

[TAAP/1](docs/standards/taap-1-spec-draft.md) is **liquid's security model** — every
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
[`docs/build/socket-fixtures.md`](docs/build/socket-fixtures.md) (liquid #305).

### Commands follow Git

Steve, 2026-10-12: *"liquid should really not deviate from git commands without a good reason
(e.g. git mv vs liquid move)"*.

Where Git has a command for the same thing, liquid uses Git's name, arguments and order. A
different name needs a reason recorded in
[`docs/cli/git-commands.md`](docs/cli/git-commands.md), and a hint in `git_hints` in
`src/main.cpp` so that typing the Git name says what to use. `cut` is not `commit` on purpose:
a change names drips that already exist. Every command belongs to exactly one `--help` group;
the build fails if one is missing or listed twice.

### The README is the introduction

Steve, 2026-10-12: *"liquid is a next generation git... the readme should act like it"* and *"the
readme should be a great introduction, with docs/\* as spinoffs for details"*.

The root `README.md` is what someone who has just found liquid reads. It says what liquid is (the
next generation of Git), argues why it is one tool in the voice of the
[research synthesis](docs/research/99-synthesis/README.md), shows a first result, tours the
workflows, and says what works today and what does not. Rules that keep it good:

- **Every section hands off to a `docs/` page** for the detail. The README states the capability
  in a sentence and links; it does not carry per-issue status, option lists or design notes.
- **Each thing appears once.** The page had grown to 614 lines by repeating its own sections
  after merges. Before adding a section, check that it is not already there; when a merge
  conflicts in `README.md`, resolve it by hand and never keep both sides.
- **It claims only what `docs/library/status.md` says is implemented**, and it has a section
  that says plainly what is not there yet.
- **Commands shown are real**: taken from the guides, which run as tests, or checked against
  `docs/reference/cli/`.

### Formatting

`clang-format` with the repository's `.clang-format` (LLVM base, 100 columns, 4-space indents) before
every commit; one statement per line. A function collapsed onto one line is sent back unread.

### Before finishing — run all five

The first two are commands and got run every time; the last two are prose and got
skipped. Treat them as the same checklist, not as an afterthought.

1. **Link check** (above) — must report zero.
2. **Count sync** (above) — recount, never estimate.
3. **Indexes.** A new research topic adds a row to `docs/research/README.md`; a new
   feature doc adds a row to `docs/features/README.md`. Tables — add the row now.
4. **The status record, the docs index, and this file.** Any change to what liquid *is* or
   what it *does* updates `docs/features/README.md`, the implementation status in
   `docs/library/status.md`, and this file. A new top-level directory or a new convention
   updates the root `README.md` and this file. See *The README is the introduction*
   below. A new `docs/` subdirectory that is not in `docs/README.md`
   is a bug.
5. **Proof is local.** Build and run the suite (`cmake --build build && ctest --test-dir build`)
   and record the counts in the pull request. Linux proof is the cluster BuildKit lane
   (liquid-bridge's `liquid-dev` skill). Never add, enable or extend a CI workflow, never wait
   on CI checks, and never cite them as proof: the org rule in the bridge `CLAUDE.md` says no
   CI. `docs/build/ci.md` describes the workflow file that remains in the repository.

**This last one has already been missed.** `docs/standards/` existed in the tree before
it appeared in either file, and TAAP was the security model before this file mentioned
it. If a change is worth a feature doc, it is worth a row in `docs/README.md` and a line in
`docs/library/status.md`.

**Correct the record when research contradicts a spec.** This has already happened
twice and both corrections improved the design. Say what changed and why, in the doc.
Do not leave a superseded claim standing next to its correction.

**No estimates.** Never preface work with size or duration guesses. Do the task, report
what changed.

## Branches: feature branches and pull requests always target `dev`

Steve, 2026-10-12: *"shouldn't you be merging into dev branch? we're not cutting a release yet"*, and
*"feature branches / prs always target dev branch"*.

- **Branch from `origin/dev`**, never from `main`.
- **Open every pull request with `--base dev`**, written out. This repository's default branch on
  GitHub is `main`, so `gh pr create` without `--base` targets `main`.
- **Before merging, read the base back**: `gh pr view <n> --json baseRefName` must say `dev`.
- **Never push, merge or fast-forward to `main`** in ordinary work. `main` is the last release and
  moves only by the release pull request that promotes `dev` to `main`, when Steve names a version
  (*Cutting a release* below).
- Images built for the dev cluster come from `dev`.

The org-wide rule is in the umbrella bridge's `CLAUDE.md`
(https://github.com/oramasearch/orama-bridge/blob/main/CLAUDE.md).

## Cutting a release

A release is a tag on `main`. The pages and their conventions are in
[`docs/releases/`](docs/releases/README.md); read its `README.md` first. `v0.1.0` (2026-10-12) was
the first. The org-wide procedure is the `cut-release` skill; this section adds liquid's specifics.

1. **Steve names the version.** Confirm what Lake is built from: liquid-bridge's pin of
   `modules/liquid` must be an ancestor of `origin/dev`.
2. **On a branch `docs/release-vX.Y.Z` from `origin/dev`**, move the version in all three places:
   `current_version()` in `include/liquid/version.hpp`, `version_string()` in `src/version.cpp`,
   and `project(liquid VERSION ...)` in `CMakeLists.txt`. `Version.StringMatchesStruct` fails when
   the first two disagree, `Version.MatchesProjectVersion` when the code and `CMakeLists.txt`
   disagree, and the ctest `release-notes-page` when `docs/releases/vX.Y.Z.md` is missing. The
   library's `VERSION`/`SOVERSION` properties are the ABI version and move only on an ABI break.
3. **Write `docs/releases/vX.Y.Z.md`** and add it to `docs/releases/README.md`: what the release
   includes, what it does not, known issues with their tracking issues, and verification. The
   feature list follows `docs/library/status.md`; a specification under `docs/features/` is not a
   claim that the feature shipped. Every number is measured.
4. **Prove it**: build, run the full suite, and put the counts in the page. A failure is fixed or
   listed as a known issue with an issue filed, never left out. Run the five finishing checks
   below.
   Linux proof is the cluster BuildKit lane
   (`codex-work/build/liquid-linux-test/run-liquid-linux-tests.sh <worktree> <label>`).
5. **Open the pull request into `dev`** (`--base dev`, read back) and merge it with a merge commit,
   never a squash. Then open the release pull request `--base main --head dev`, write its title and
   description, and merge it with a merge commit and without `--delete-branch` (the head is `dev`).
   This is the only pull request that targets `main`.
6. **Tag the release pull request's merge commit** `vX.Y.Z`
   (`git tag -a vX.Y.Z <merge commit> -m "liquid vX.Y.Z" && git push origin vX.Y.Z`),
   then publish the GitHub release from the page:
   `gh release create vX.Y.Z --repo oramasearch/liquid --verify-tag --title "Release vX.Y.Z" --notes-file <page without its first heading, relative links made absolute at the tag>`.

A release moves no pin. liquid-bridge's pin of `modules/liquid`, and the Lake image built from
it, change in their own steps.

## Related repositories

**Read Zetta only from the umbrella bridge** —
`/Users/stevesperandeo/dev/homer6/orama-work/modules/zetta-bridge/modules/zetta` —
never from a standalone clone. A standalone `~/dev/oramasearch/zetta` was found 1,053
commits behind and missing the whole `include/zetta/api/` HTTP layer; conclusions drawn
from it were wrong. Same for the Orama operator, at
`orama-work/modules/orama-cloud-v1-bridge/modules/orama-cloud-v1-operator` (that
checkout may be on an in-flight branch — cut PR branches from a clean `origin/main`).

This repository is one module of
[`oramasearch/liquid-bridge`](https://github.com/oramasearch/liquid-bridge). The
**action plan and Phase 0 decisions live there**, in `docs/`; this file records the
chosen options once they are made.

| Repository | Relationship |
|---|---|
| [`oramasearch/liquid-bridge`](https://github.com/oramasearch/liquid-bridge) | The bridge: this repo, k0s, lake, liquify and orama-operator as submodules, plus `docs/action-plan.md`, `docs/repositories.md`, `docs/decisions.md`. |
| [`oramasearch/lake`](https://github.com/oramasearch/lake) | The lake binary — the hosted service. Live on AKS (`Lake/prod`): the control surface (REST `/v1`, OpenAPI, MCP) over a Postgres control plane, Zetta as data plane; consumes libliquid for repository data once Phase 1 lands. |
| [`oramasearch/liquify`](https://github.com/oramasearch/liquify) | The IDE — native, cross-platform (SDL3 + Dear ImGui), links libliquid as a pinned submodule. Private for now, open source eventually; never depends on Zetta. |
| [`oramasearch/orama-operator`](https://github.com/oramasearch/orama-operator) | **The Orama operator** (renamed from `orama-cloud-v1-operator`; the Zetta operator). lake is deployed by adding a `Lake` CR to it — there is no separate lake operator. Running in production on AKS. |
| [`oramasearch/k0s`](https://github.com/oramasearch/k0s) | **Private copy** of `k0sproject/k0s`, synced with upstream, modified for drips. `main` is a pristine upstream mirror; our changes live on `liquid`. See its `ORAMA-FORK.md`. |
| [`oramasearch/automerge-cpp`](https://github.com/oramasearch/automerge-cpp) | The MIT-licensed CRDT datastore ([17](docs/features/17-implementation.md)), consumed here as a pinned private submodule. |
| [`oramasearch/oramacore-cpp`](https://github.com/oramasearch/oramacore-cpp) | BM25F tokenizer and scorer ([12](docs/features/12-search.md)), consumed as a private pinned submodule by liquid's local index. |
| [`oramasearch/corosio`](https://github.com/oramasearch/corosio) | **Private patch carrier** for `cppalliance/corosio` — `orama-main` carries an epoll hang-up fix that is **not upstream**. Zetta consumes it. **Consume this, never `cppalliance/corosio` directly.** (An earlier claim here that Zetta had drifted to upstream was wrong — it came from a stale standalone clone.) |
| [`oramasearch/http`](https://github.com/oramasearch/http) | **Private patch carrier** for `cppalliance/http`, branch `zetta-patches`. Same rule: consume this, never upstream directly. |
| `cppalliance/capy` | No private copy. Consume upstream `develop` until there is a patch to carry. |
| [`oramasearch/orama-bridge`](https://github.com/oramasearch/orama-bridge) | The org umbrella bridge. Authoritative for org-wide conventions. |

**Copies of public upstreams are never forks.** `gh repo fork` produces a **public**
repository that cannot be flipped to private. Use
`gh repo create oramasearch/<name> --private` and push the upstream history into it, then
set the `upstream` remote's *push* URL to an invalid value so an accidental push cannot
leak our changes. Verify with `gh repo view <repo> --json visibility` **before** pushing.

## Confidentiality

**Metis material is confidential** — `docs/metis/` and `docs/competitors/` in
`oramasearch/corporate-intelligence`.

- **This repo is a map, not a copy.** [`docs/features/23-metis-and-zetta.md`](docs/features/23-metis-and-zetta.md)
  states how liquid relates to Metis and restates none of it. Follow that pattern.
- **Never send Metis, competitor or customer material to an external service** —
  Perplexity, Firecrawl, or any other. Research queries cover the public landscape only.
- `oramasearch/liquid` is **private**. Never make it public, never fork it publicly.
  Every `oramasearch` repo is private; a public fork cannot be flipped back.

## Research method

Web-grounded research uses the Perplexity MCP server.

- `perplexity_research` for depth. It frequently exceeds the 300 s API timeout on broad
  prompts — when it does, **retry with `perplexity_reason` and a tighter prompt** rather
  than abandoning the topic. Four topics were gathered that way and say so in their
  header.
- Large results are written to a tool-results file rather than returned inline; extract
  with `json.load(...)["response"]`.
- Note the tier used in the `sources.md` header. A `reason`-tier source is still
  web-grounded and cited, but less exhaustive — readers should know which they have.

## Decisions — recorded

These were the Phase 0 gates. Most are now decided; the rest are narrowed.

**Decided:**

1. **liquid does not sit on Zetta. Separate stores, by licence boundary.** liquid and
   liquify will be **open source** (eventually). Zetta is closed / enterprise
   source-available. An open-source tool cannot depend on it, so liquid has its own
   store. The earlier convergence recommendation ([23](docs/features/23-metis-and-zetta.md))
   was made without this constraint and is withdrawn.
2. **lake uses Zetta as much as possible as its data plane (control plane: Postgres for now; bridge decisions §6).** lake is the enterprise product and builds on
   Zetta — including borrowing Zetta's web-serving approach. lake must still host liquid
   repositories, so it consumes liquid's store **as a library** for repository data and
   uses Zetta for its own control plane and operational state.
3. **lake is deployed by the Orama operator, not a separate operator.** `Lake` is a new
   CR on `oramasearch/orama-operator` (renamed from `orama-cloud-v1-operator`), which
   already has its own Zetta as observed-state store — **k8s/etcd is the source of truth
   for desired state (the CRs); Zetta holds observed state**, synced with anti-flap
   discipline. See its `docs/zetta-datastore.md`.
4. **One CRDT — `automerge-cpp`, and it stays open.** MIT-licensed and intended to remain
   open source, currently private, and already vendored by Zetta. It is the shared open component
   between liquid (open) and Zetta (closed). Its canonical repository is
   `oramasearch/automerge-cpp`.
5. **Async stack — Boost.Capy for coroutines, Corosio for networking**, matching Zetta
   and oramacore-cpp. Consume `oramasearch/corosio` (private patch carrier), never
   `cppalliance/corosio` directly. stdexec is not used. Requires **Clang 17+**: Homebrew
   LLVM on macOS, `clang++-NN` on Linux; Apple's Clang and Ubuntu's GCC 13 are too old.
   **libliquid is coroutine-native on Capy in its public API** (Steve, 2026-09-03: "use the
   coroutine library that we have"): every I/O function returns `runtime::task<T>` and Capy is a
   public dependency through the one boundary header `liquid/runtime.hpp`; Corosio does the I/O
   (files, sockets, timers) behind the boundary. No synchronous-first phase, no raw threads.
6. **Licence posture.** liquid and liquify: open source, eventually — licence text not
   yet chosen. lake and the operator: enterprise. Zetta: source-available. k0s: Apache
   2.0 upstream, private copy.

7. **libliquid is the product; the `liquid` binary, liquify and lake are clients of it**
   (2026-09-02). One CMake target `liquid::liquid` (static by default, shared on request,
   hidden visibility with `LIQUID_EXPORT` on the API), installed with a package config so
   `find_package(liquid)` works, and consumable as a submodule with `LIQUID_BUILD_CLI OFF`.
   Everything under `include/liquid/` is public API and must stay free of Zetta and, where
   feasible, of third-party types (Boost stays behind the boundary). The CLI never carries
   behaviour the library lacks. **The surface is `docs/library/api-map.md`** (2026-09-03):
   `result<T>` errors everywhere, opaque handles, plain value records, digests that carry their
   algorithm, and coroutine-native waiting operations; a header not on the map is not public.
   **The object model
   is `docs/library/object-model.md`**: canonical-CBOR records in the store, one small repository
   document plus one automerge document per directory (decided 2026-09-04 by measurement), liquid's own `node_id` (survives moves) and `change_id` (the digest
   of the change record, never automerge's `ChangeHash`), conflicts and symbols reserved in the
   change record from the first commit. Directory checkpoints prefer automerge-cpp compact saves,
   fall back to full bytes when a compact load check fails, and reopen sync-capable; drip metadata
   enters `CommitOptions`, while TAAP remains the attribution authority.
8. **The chunk index is liquid's own storage engine, built from scratch** (Steve, 2026-09-03;
   this replaces the RocksDB choice recorded earlier that day). No third-party key-value store in
   the store: a fixed-width LSM of liquid's own (an in-memory sorted memtable flushed to immutable
   sorted runs with a 256-way fanout and a bloom filter each; size-tiered compaction), always
   rebuildable from the packs, so it is a cache with no write-ahead log of its own. The benchmark
   in `docs/library/chunk-index-benchmark.md` (liquid #45) is the bar: at 10M entries, at least
   RocksDB-tuned's insert and point-lookup rates and no more than its footprint. Design in
   `docs/library/store.md` (liquid #1).
9. **Transport is liquid's own: HTTP/2 with gRPC framing and a protobuf codec on Corosio** (Steve,
   2026-09-05; bridge decision 9). No `grpc++`, no `protobuf`, no code generator. REAPI v2 is spoken
   through liquid's own layers (#155 transport, #156 codec, #16 client and conformance), and the
   same HTTP/2 layer serves liquid's own workers (#63) and lake later. Design:
   `docs/library/distribution.md`.
10. **Agent application-data isolation uses template copies with a reflink fast path** (Steve,
   2026-09-05; liquid-bridge decision 26). Each session owns one branch per declared PostgreSQL
   database, and its sandbox receives the branch URL rather than the base. Promotion replaces the
   base only when a logical fingerprint proves it has not changed since branching; liquid does not
   attempt a data merge. `CREATE DATABASE ... TEMPLATE ...` is the portable baseline. A local,
   quiescent cluster may use APFS `clonefile` or Linux `FICLONE` plus a second server. Details:
   `docs/library/data-branching.md`.
11. **TSA signature verification is liquid's own, verification only** (Steve, 2026-09-05;
    bridge decision 32). RFC 3161 and RFC 4998 verification uses liquid's strict DER, CMS, X.509,
    RSA, and NIST-curve ECDSA readers and verifiers. It does not generate keys or signatures and
    does not link a general-purpose cryptography library for these operations. Design:
    `docs/library/transparency.md`.
12. **Tier 2 tree-sitter grammars are fetched at configure time and pinned by digest** (liquid #72,
    2026-09-05). One canonical lock names an exact public-upstream commit, archive SHA-256,
    generated parser SHA-256, every hand-written scanner SHA-256, tag-query SHA-256, language ABI,
    licence, and extensions. Generated parsers do not live in-tree: the digest-checked bytes have
    the same trust boundary as vendored bytes without adding hundreds of megabytes or an aggregate
    mirror maintenance obligation. `LIQUID_GRAMMAR_MIRROR` remains the explicit URL-root change for
    an organisation or air-gapped mirror. See [`docs/library/grammars.md`](docs/library/grammars.md).
11. **The dependency solver is liquid's own CDCL engine** (Steve, 2026-09-05; liquid-bridge
   decision 20). Do not vendor Clingo. Package rules compile to clauses behind the private
   `src/resolve/concretizer.hpp` interface, while `include/liquid/resolve.hpp` exposes only plain
   records, tasks, results, digests, and stable explanations. Design: `docs/library/resolution.md`.
11. **The runtime node store is embedded liquid, not a containerd snapshotter** (liquid #202;
    bridge decision 23). An image root is a tree digest plus its sync URL. `liquid node-store
    serve` receives only tree and blob records, exposes a reference-counted read-only VFS mount,
    and owns the shared node chunk budget. The k0s #14 Go proxy snapshotter bind-mounts that path
    and owns writable layers; liquid ships neither the proxy nor layers.
12. **Install trees are position-independent by construction** (liquid #73). The fixed virtual
    build root is not an install location, and installed artifacts contain only tree-relative
    references. Result publication rejects embedded build or declared install prefixes, absolute
    binary lookup paths, and absolute package metadata. Liquid never rewrites existing binaries
    and does not require a fixed content-addressed install path. See
    [`docs/library/relocation.md`](docs/library/relocation.md).
13. **Hardware command layers are owned by liquid over operating-system transports** (Steve,
    2026-09-05; liquid-bridge decision 21). CTAP2 uses liquid's canonical CBOR over USB HID, PIV
    sends APDUs through PC/SC, and the TPM 2.0 sealing subset uses `/dev/tpmrm0` or Windows TBS.
    Tests inject protocol-level software doubles through those transport interfaces. Do not add
    libfido2, OpenSC, or tpm2-tss. Design: [`docs/library/identity.md`](docs/library/identity.md),
    [`docs/library/encryption.md`](docs/library/encryption.md), and
    [`docs/library/release-approval.md`](docs/library/release-approval.md).
14. **Tier B source packaging is Debian-only in Phase 5** (liquid #79, 2026-09-06). A
    distribution qualifies only when liquid can run its reproducible source-build tool and policy
    linter in a pinned peer environment. Debian qualifies through `sbuild` and lintian, and its
    concrete bar is an unsigned package a maintainer could sponsor. RPM `.spec` output remains a
    Tier A byproduct labelled not policy-reviewed. Fedora, openSUSE, Arch, and other source-policy
    formats are deferred. OpenPGP signing remains the sponsor's `debsign` step.

**Still open:**

- **TAAP content-addressed subjects** — the spec needs an object-subject form
  ([25](docs/features/25-identity-attestation-and-transparency.md)). The final protocol form needs
  the protocol owner; build provenance uses the versioned interim result-body digest documented in
  [`docs/library/provenance.md`](docs/library/provenance.md).
- **Which open-source licence** for liquid and liquify.

Phase 0 and Phase 0.5 are historical planning milestones. Current implementation coverage and
limits live in [`docs/library/status.md`](docs/library/status.md); the bridge action plan and
[master epic](https://github.com/oramasearch/liquid-bridge/issues/7) track the remaining work.

## Inherited org conventions — the ones hit most here

- **No AI attribution.** Never add `Co-Authored-By: Claude`, "Generated with…", or any
  mention of Claude/Codex/AI to commits, PR titles or bodies, tags or release notes.
  Strip the harness's default trailer.
- **Banned word: "seam".** Ambiguous, and ambiguity is dangerous in system design. Name
  the actual mechanism — **interface**, **extension point**, **injected dependency**,
  **boundary**, **test double point**.
- **Memory system disabled.** Do not read or write `~/.claude/projects/.../memory/`.
  Durable rules go in a reproducible doc: repo-wide → this file; folder-scoped → that
  folder's `README.md`.
- **Commit means push.** Never end a turn with an unpushed commit, and never ask whether
  to push — committing *is* the decision to publish.
- **Documentation discipline.** Touching files in a folder updates that folder's
  `README.md` in the same change.
