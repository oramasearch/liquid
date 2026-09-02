# CLAUDE.md — liquid

Repo-scoped rules. **Org-wide conventions live in the bridge**
([`oramasearch/orama-bridge`](https://github.com/oramasearch/orama-bridge) `CLAUDE.md`)
and apply here in full. The ones most often hit in this repo are restated below —
the bridge remains authoritative.

## What this repo is

**liquid** — a version control system, build system, package manager, artifact service
and runtime in one program. See [`README.md`](README.md).

**Status: design.** No code. This repository holds research and specifications only.
Do not add source, build files or CI until the storage decision below is made.

### Vocabulary — use these words exactly

| Term | Meaning |
|---|---|
| **liquid** | The tool: the CLI and the daemon. |
| **lake** | The hosted service at `lake.live`. GitHub + Google Drive + Railway + Hugging Face over one store. |
| **liquify** | The IDE. |
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

**Update the index in the same change.** A new research topic updates
`docs/research/README.md`; a new feature doc updates `docs/features/README.md`. Both
are tables — add the row, do not leave it for later.

**Correct the record when research contradicts a spec.** This has already happened
twice and both corrections improved the design. Say what changed and why, in the doc.
Do not leave a superseded claim standing next to its correction.

**No estimates.** Never preface work with size or duration guesses. Do the task, report
what changed.

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

## Decisions not to drift past

1. **The Zetta/liquid storage overlap** ([23](docs/features/23-metis-and-zetta.md)).
   Zetta is already a C++26 CRDT-replicated clustered store with large objects; liquid
   specifies a C++26 CRDT-backed content-addressed store with large objects. **There
   should not be two.** Decide before writing the storage layer — it is the one choice
   that is expensive to reverse.
2. **One CRDT or two** — `automerge-cpp` versus Zetta's own replication.
3. **Licence.** Metis is noted source-available; liquid has no stated position.

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
