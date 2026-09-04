# CLAUDE.md — liquid

Repo-scoped rules. **Org-wide conventions live in the bridge**
([`oramasearch/orama-bridge`](https://github.com/oramasearch/orama-bridge) `CLAUDE.md`)
and apply here in full. The ones most often hit in this repo are restated below —
the bridge remains authoritative.

## What this repo is

**liquid** — a version control system, build system, package manager, artifact service
and runtime in one program. See [`README.md`](README.md).

**Status: Phase 1 — the store, inside libliquid.** Phase 0 is decided and Phase 0.5 met (see *Decisions* below).
Code is a C++26 **libliquid** (`liquid::liquid`, static or shared, `find_package` works; see
`docs/library/libliquid.md`) plus the `liquid` binary as a thin client of it; liquify links the
same library as a pinned submodule, lake will. Clang 17+, CMake + Ninja, GoogleTest. Phase 1
(the store) is built inside libliquid. Optional developer benchmarks live under `bench/` and are
excluded unless their build option is enabled.

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
3. **One open question blocks liquid**: TAAP subjects are key-derived, but documents,
   drips and artifacts are content-addressed. A content-addressed subject form is
   required, not optional. Do not design around the gap — it needs closing in the spec.

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
| [`homer6/automerge-cpp`](https://github.com/homer6/automerge-cpp) | The CRDT datastore ([17](docs/features/17-implementation.md)). Still under `homer6`; whether it moves is undecided. |
| [`oramasearch/oramacore-cpp`](https://github.com/oramasearch/oramacore-cpp) | BM25F search engine ([12](docs/features/12-search.md)). Private; currently a scaffold. |
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
4. **One CRDT — `automerge-cpp`, and it stays open.** MIT-licensed, public, already
   vendored by Zetta. It is the shared open component between liquid (open) and Zetta
   (closed). Where the canonical repo lives is still open, but its licence is settled.
5. **Async stack — Boost.Capy for coroutines, Corosio for networking**, matching Zetta
   and oramacore-cpp. Consume `oramasearch/corosio` (private patch carrier), never
   `cppalliance/corosio` directly. stdexec is not used. Requires **Clang 17+**: Homebrew
   LLVM on macOS, `clang++-NN` on Linux; Apple's Clang and Ubuntu's GCC 13 are too old.
6. **Licence posture.** liquid and liquify: open source, eventually — licence text not
   yet chosen. lake and the operator: enterprise. Zetta: source-available. k0s: Apache
   2.0 upstream, private copy.

7. **libliquid is the product; the `liquid` binary, liquify and lake are clients of it**
   (2026-09-02). One CMake target `liquid::liquid` (static by default, shared on request,
   hidden visibility with `LIQUID_EXPORT` on the API), installed with a package config so
   `find_package(liquid)` works, and consumable as a submodule with `LIQUID_BUILD_CLI OFF`.
   Everything under `include/liquid/` is public API and must stay free of Zetta and, where
   feasible, of third-party types (Boost stays behind the boundary). The CLI never carries
   behaviour the library lacks.

**Still open:**

- **TAAP content-addressed subjects** — the spec needs an object-subject form
  ([25](docs/features/25-identity-attestation-and-transparency.md)). Blocking for
  attribution of documents and artifacts; needs the protocol owner.
- **Which open-source licence** for liquid and liquify.
- **Canonical home of `automerge-cpp`** — `homer6` or `oramasearch`.

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
