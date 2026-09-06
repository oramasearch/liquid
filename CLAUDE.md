# CLAUDE.md — liquid

Repo-scoped rules. **Org-wide conventions live in the bridge**
([`oramasearch/orama-bridge`](https://github.com/oramasearch/orama-bridge) `CLAUDE.md`)
and apply here in full. The ones most often hit in this repo are restated below —
the bridge remains authoritative.

## What this repo is

**liquid** — a version control system, build system, package manager, artifact service
and runtime in one program. See [`README.md`](README.md).

**Status: Phase 1 store exit criterion met — the store is inside libliquid.** Phase 0 is decided and Phase 0.5 met (see *Decisions* below).
Code is a C++26 **libliquid** (`liquid::liquid`, static or shared, `find_package` works; see
`docs/library/libliquid.md`) plus the `liquid` binary as a thin client of it; liquify links the
same library as a pinned submodule, lake will. The mapped public headers compile and link; the
error, digest, identity document codec, and signature decoder APIs plus liquid #1's local chunk
store, liquid #199's pre-pack write-through callback and remote-registered local eviction,
liquid #4's TAAP/1 actor identities,
liquid #111's identity-key rotation and chain verification, and signed drips and changes, liquid #7's
trust-domain encrypted objects, liquid #53's versioned data-key rotation, revocation, and repository
readability reports, liquid #8's leased-root garbage collection with pack compaction, and liquid #55's
deterministic crash simulator, liquid #116's bounded-memory file and repository streaming, liquid
#5's local capture, liquid #50's inline secret scanning, liquid #62's registered TAAP attestations,
verified redaction, liquid #52's JSON/`.env` leaf encryption with structural key diff, and liquid
#43's local Git import/export with identity mapping are implemented;
liquid #85 adds persisted actor, directory-document, and repository write buckets, explicit
pre-transaction rejection, peer 429 retry signals, daemon watermarks, and consumer-lag status;
liquid #10 provides local sandboxed action execution through `liquid::build` on Linux. Liquid #11
provides its signed, manifest-validated persistent disk cache with read-only publication by default.
Liquid #12 provides `liquid wrap` for configuration-free CMake, Autotools, Make, and
`compile_commands.json` builds on Linux with FUSE and macOS with macFUSE. Liquid #13 provides
sampled local re-execution, signed `reproducibility/1` attestations, critical divergence alerts,
permanent divergent-result quarantine, and result-bound `slsa_provenance/1` attestations that
verify on cache lookup and publication and export as in-toto/DSSE; operations owned by later issues
return `errc::unsupported`. Liquid #15 provides record-derived `liquid why`, per-wrap explanations,
disk-tier hit rates, miss reasons, and output-byte statistics. Liquid #57 provides versioned action
phase timings, retained build traces, dependency-aware critical paths, Chrome trace export, and
`liquid why-slow`. Liquid #60 adds fail-closed source and toolchain input policy over verified
registered-profile attestations, approved-builder identity quorums, policy-bound cache candidates,
and policy check/explain commands. Liquid #56 adds content-keyed Autotools and CMake configure
facts, fact-bundle export, and lookup-only foreign-target probing through `wrap --target`.
Liquid #58 adds typed ref/result retention, computed reachability, cost-aware size-capped eviction,
and retention explanations. Liquid #61 adds signed `sbom/1` attestations with graph-derived
SPDX/CycloneDX documents or distinctly marked binary inference. Liquid #27 adds ELF, Mach-O, and
PE `abi/1` extraction, automatic result publication, signed compatibility attestations, public
descriptor lookup and diffing, wrapper break reports, and the `liquid abi` command. Liquid #17 adds
transport-neutral remote blob records, local/remote/absent store availability, chunk-range
hydration and on-demand materialisation, remote-byte status, metadata-only remote result recording,
and opt-in Linux read refusal for remote-eligible actions. Liquid #18 adds remote executor scheduling over those shared
blob interfaces, source and toolchain transfer from the store, recorded local/remote/race cost
decisions, short-action batching, and race-loser cancellation; private HTTP/2 and gRPC framing are
implemented. Liquid #16 adds the REAPI client, validated remote cache tier, repository configuration,
and `liquid serve reapi` CAS and ActionCache. Liquid #63 adds liquid's action-digest scheduler,
size-class worker fleet, bounded requeue, TAAP-signed remote results, and deployable control
surfaces. Liquid #156
adds the dependency-free protobuf codec, REAPI v2.3 message set, native tree/action/result
conversions, and BLAKE3/SHA-256 capability selection. Liquid #19 adds explicit local replay of recorded actions and results, byte-level
comparison, and optional verification recording; remote inputs hydrate through the registered
transport-neutral blob source. Liquid #20 adds the persisted demand-driven query graph, canonical
query and input fingerprints, red-green validation, early cutoff, durability-filtered invalidation,
cooperative cancellation, recorded-input retrieval, and `action_result` nodes for direct and
wrapped builds. Liquid #23 adds sampled clean recomputation in fresh graph instances, durable
validation records, divergence reporting and eviction, idle scheduling, and graph-wide CLI
verification. Liquid #65 adds the versioned frontend C ABI, dynamic and static module
registration, tree-sitter C/C++ unit discovery, Clang C++ definition actions, relocatable-object
assembly, and unit-keyed graph invalidation. Liquid #64 adds signed zstd delta delivery with
verified local and registered-remote blob fetch plus chunk fallback. Liquid #21 adds the
macOS/Linux per-checkout daemon, start-on-first-use CLI attachment, capture ownership, ordered
cursor fan-out, status reporting, idle shutdown, graph persistence, and bounded resident graph
resident graph values; Windows reports unsupported until Corosio supplies named-pipe transport.
Liquid #22 adds the content-addressed local BM25F and positional-trigram index, atomic segment
publication, per-drip search fan-out target, public working/ref query API, and `liquid search`.
Liquid #67 adds measured selection among BM25F, verified trigrams, and parallel mapped scans,
cursor-based stale-index exclusion, common located hit records, the public executed-plan API, and
`liquid search --explain`.
Liquid #29 adds the versioned format-codec C ABI and registry; dependency-free ZIP, tar, HDF5,
Parquet, and SQLite readers; deterministic tar output; opt-in container diff; archive search; and
content-addressed retention of decoded Tier 1 and Tier 2 entries.
Liquid #193 adds move-only public build handles and multi-subscriber Capy event streams, daemon
forwarding, stable JSON-lines CLI output, CMake/native test target discovery, cached test actions,
and `liquid test`.
Liquid #200 adds public configured-build plans with complete action inputs, dependency edges and
validated hit state, one-call repository and daemon builds, the current root-tree digest, and
`liquid build --plan`.
Liquid #72 replaces tag-selected tree-sitter inputs with one canonical lock for the runtime and 49
public grammars. Configure-time archives and every compiled parser/scanner and tag query are pinned
by SHA-256; the generated registry, public language metadata, syntax fallback, and grammar-set
provenance all derive from that lock.
The repository object model, internal CRDT metadata document, canonical CBOR codec, and drip
journal are implemented. Liquid #41 adds per-handle session IDs, complete mutation records,
local-intent operation and session undo reports, operation-log CLI output, and historical refs,
locks, and nested pointers. Directory
documents use compact shallow checkpoints, upstream sync framing, checked external application, and
patch-based remote observation. Repositories can use a caller-owned shared store with independent
checkpoint, journal, and lease namespaces. Liquid #160 adds atomic signed repository metadata import/export,
missing-object sessions, the HTTP/1.1 lake client, and CLI push/pull. Liquid #165 adds the public
repository sync server and the `serve sync` HTTP adapter; liquid #177 adds per-change and
change-graph-derived reference authorization; liquid #189 adds per-reference accepted/refused
outcomes and opt-in atomic pushes. Full materialisation works on
every platform; lazy VFS mounts use
libfuse3 on Linux and optional macFUSE on macOS. The CLI covers the Phase 1 repository, capture,
store, VFS, identity, Git import/export, and the Phase 2 build wrapper. Clang 17+, CMake + Ninja,
and GoogleTest are used. Phase 1 (the store) is built inside libliquid. Optional developer
benchmarks live under `bench/` and are excluded unless their build option is enabled.

Liquid #166 adds public tree/change diff plus cursor-paged result, attestation, trace, and SBOM
enumeration used by CLI record views.
Liquid #66 adds content-addressed compiler and syntactic-fallback symbol records derived from
recorded compile actions, graph retrieval,
repository access, and the `liquid symbols` command.
Liquid #174 adds line-hunk-based text anchors that follow `node_id` moves and report shifted,
approximate, or deleted positions across changes.
Liquid #86 adds persisted agent base requirements, caller-stated write bases, path-read sets,
line-hunk stale reports, and signed disjoint or forced re-validation records.
Liquid #210 adds strict DER, CMS, and X.509 parsing plus verification-only RSA and ECDSA for RFC
3161 checkpoint timestamps and RFC 4998 archive-timestamp renewal chains with explicit TSA trust
anchors and historical certificate validation.

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

**Keep counts accurate.** `README.md` states topic and feature counts. Recount, never
estimate:

```sh
ls -d docs/research/*/ | wc -l          # topics
ls docs/research/*/sources.md | wc -l   # with sources
ls docs/features/*.md | grep -v README | wc -l
```

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
4. **Root `README.md` and this file.** Any change to what liquid *is*, what it *does*, a
   new top-level directory, or a new convention updates **both**. A new `docs/`
   subdirectory that appears in neither is a bug.
5. **CI changes.** Run the local proof command before pushing, then wait for every platform
   and sanitizer check and record their results in the pull request.

**This last one has already been missed.** `docs/standards/` existed in the tree before
it appeared in either file, and TAAP was the security model before this file mentioned
it. If a change is worth a feature doc, it is worth a line in the README.

**Correct the record when research contradicts a spec.** This has already happened
twice and both corrections improved the design. Say what changed and why, in the doc.
Do not leave a superseded claim standing next to its correction.

**No estimates.** Never preface work with size or duration guesses. Do the task, report
what changed.

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
   change record from the first commit. Directory checkpoints use automerge-cpp compact saves and
   shallow loads; drip metadata enters `CommitOptions`, while TAAP remains the attribution authority.
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
10. **TSA signature verification is liquid's own, verification only** (Steve, 2026-09-05;
    bridge decision 32). RFC 3161 and RFC 4998 verification uses liquid's strict DER, CMS, X.509,
    RSA, and NIST-curve ECDSA readers and verifiers. It does not generate keys or signatures and
    does not link a general-purpose cryptography library for these operations. Design:
    `docs/library/transparency.md`.
10. **Tier 2 tree-sitter grammars are fetched at configure time and pinned by digest** (liquid #72,
    2026-09-05). One canonical lock names an exact public-upstream commit, archive SHA-256,
    generated parser SHA-256, every hand-written scanner SHA-256, tag-query SHA-256, language ABI,
    licence, and extensions. Generated parsers do not live in-tree: the digest-checked bytes have
    the same trust boundary as vendored bytes without adding hundreds of megabytes or an aggregate
    mirror maintenance obligation. `LIQUID_GRAMMAR_MIRROR` remains the explicit URL-root change for
    an organisation or air-gapped mirror. See [`docs/library/grammars.md`](docs/library/grammars.md).
11. **The runtime node store is embedded liquid, not a containerd snapshotter** (liquid #202;
    bridge decision 23). An image root is a tree digest plus its sync URL. `liquid node-store
    serve` receives only tree and blob records, exposes a reference-counted read-only VFS mount,
    and owns the shared node chunk budget. The k0s #14 Go proxy snapshotter bind-mounts that path
    and owns writable layers; liquid ships neither the proxy nor layers.

**Still open:**

- **TAAP content-addressed subjects** — the spec needs an object-subject form
  ([25](docs/features/25-identity-attestation-and-transparency.md)). The final protocol form needs
  the protocol owner; build provenance uses the versioned interim result-body digest documented in
  [`docs/library/provenance.md`](docs/library/provenance.md).
- **Which open-source licence** for liquid and liquify.

**Phase 0.5 is met** (every module builds and runs; lake serves on AKS). Phase 1 — the store —
is the current work, inside libliquid, per the bridge `docs/action-plan.md` and the issues it
links (liquid #33 is the Phase 1 epic).

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
