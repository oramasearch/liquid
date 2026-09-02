# Metis, Zetta, and where liquid fits

> **A map, not a copy.** Metis's blueprint and competitive material are confidential
> and live in `oramasearch/corporate-intelligence`. This page states how liquid relates
> to them and restates nothing. No Metis material has been or should be sent to any
> external research service.

## The landscape

| | What it is |
|---|---|
| **Metis** | The Enterprise Intelligence OS — Orama's pivot. One belief holds it up: *you cannot have AI, or intelligence, without data availability.* Two sides: the software side is **Zetta**; the execution side is forward-deployed engineering plus the Data-Availability Theory. |
| **Zetta** | The software side. Today a **C++26 equal-peer CRDT-replicated store** with clustering, JSONL storage plus large objects, a Postgres-SQL subset, and headed (SDL3 + Dear ImGui) and headless modes. Turns a plain-language problem into a deployed, running data + AI system **on infrastructure the customer controls**. |
| **liquid / lake / liquify** | This project. The tool, the hosted service, the IDE. |
| **The k8s distro** | [13](13-deployment-runtime.md). Where Zetta and lake run on customer infrastructure. |

## The strategic fit

Metis's claim is that every intelligence instrument is **bounded by the availability of
decision-relevant data**, and that availability decomposes into ten dimensions in three
bands, remediated in order **C → B → A**:

| Band | Question | Dimensions |
|---|---|---|
| **C — Reach** | Can the instrument get the data, lawfully? | Existence · Accessibility · Permission |
| **B — Fidelity** | Can it trust and understand it? | Quality · Currency · Provenance · Meaning |
| **A — Fitness** | Does it serve *this* decision through *this* instrument? | Relevance · Sufficiency · Shape |

**liquid is a Band C and Band B machine.** That is the whole argument, and it is
unusually literal:

| Dimension | What liquid contributes |
|---|---|
| **Existence** | The theory names this the one dimension no later layer can repair, and identifies *deliberate recording of decisions and outcomes* as the most commonly missing data in any company. **Auto-drip records every write, continuously, without anyone deciding to** ([22](22-change-capture.md)). The operation log records the decision; the build graph records the outcome. This is an Existence engine. |
| **Accessibility** | One store, many governed interfaces — OCI, npm, PyPI, Maven, apt, generic ([09](09-artifact-service.md)) — plus search ([12](12-search.md)) and a Drive surface for non-developers ([15](15-consumer-sync-and-web.md)). "Reachable through a governed interface without bespoke extraction" is the definition, and it is the product. |
| **Permission** | Identity, org/team roles, per-folder ACLs, per-subtree encryption, workload identity, audit ([19](19-identity.md), [11](11-secrets-and-encryption.md)). Lawful reach, checked at commit rather than at open. |
| **Quality** | Content addressing makes integrity structural: a chunk's name *is* its checksum, verified on every read ([01](01-object-model-and-storage.md)). |
| **Currency** | The drip fan-out updates indexes and derived state **on write**, not on a nightly batch ([22](22-change-capture.md)). Metis's own "why" names *the agent that acted on stale state* as one of the failures; this is the mechanism against it. |
| **Provenance** | liquid's strongest card. Signed attestations for every action, reproducibility enforced and verified, a transparency log, and chain of custody end to end ([10](10-supply-chain.md)) — produced as a **byproduct**, not a project. |
| **Meaning** | Partial. Structural decomposition of formats ([03](03-diff-and-merge.md)), symbols from the compiler ([12](12-search.md)). Semantics *of the business* is Zetta's and Metis's work, not liquid's. |
| **Relevance · Sufficiency · Shape** | Not liquid. These are decision-indexed and belong to Zetta and the forward-deployed side. |

The division of labour is clean, and it follows the theory's own ordering:

```
liquid + lake + k8s distro     →  manufacture Reach and Fidelity (C, B)
Zetta                          →  turn available data into running instruments
Forward-deployed + theory      →  score decisions, decide what to build (A)
```

**"On infrastructure you control"** is Zetta's stated posture and the k8s distro's
reason to exist ([13](13-deployment-runtime.md)). They are the same requirement.

## The decision that has to be made first

**Zetta and liquid overlap substantially, and pretending otherwise wastes the team's
scarcest resource.**

Both are, today, described as:

- C++26
- a CRDT-replicated store
- clustered / equal-peer
- handling large objects
- built by the same small group

liquid additionally specifies content-defined chunking, a Merkle DAG, an action graph
and lazy materialisation ([01](01-object-model-and-storage.md)). Zetta additionally has
JSONL storage, a Postgres-SQL subset and a GUI. But **the storage and replication core
is the same kind of system, and there should not be two of them.**

There is a second instance of the same question: liquid specifies
[`automerge-cpp`](https://github.com/homer6/automerge-cpp) as its CRDT
([17](17-implementation.md)), while Zetta has its own CRDT replication. Two CRDT
implementations in one organisation is one too many.

Four options, and this needs a deliberate answer rather than drift:

| Option | Shape | Cost |
|---|---|---|
| **A — liquid on Zetta** | Zetta's store becomes liquid's storage and replication layer; liquid adds chunking, the Merkle DAG, the action graph. | Zetta must grow content-defined chunking and content addressing. Couples liquid's schedule to Zetta's. |
| **B — Zetta on liquid** | liquid's chunk store becomes Zetta's large-object and replication substrate; Zetta keeps SQL, JSONL and the GUI above it. | Zetta must migrate its store. liquid must mature first, and it is younger. |
| **C — Converge** | One store, two frontends: liquid's developer surface and Zetta's data surface over a shared C++26 core. | Highest short-term cost, lowest long-term cost. Probably the right answer if both are strategic. |
| **D — Stay separate** | Defined boundary; liquid holds artifacts and code, Zetta holds operational data. | Cheapest now. Two CRDT stores, two replication protocols, two GC problems, forever. |

**Recommendation: C, arrived at via B.** Both products need exactly one excellent
content-addressed replicated store, and liquid's specification of that store is now more
detailed than anything else written down. But liquid is unbuilt and Zetta is running, so
the pragmatic path is to build liquid's chunk store as a component *designed for Zetta
to adopt*, prove it in liquid, then converge — rather than declaring convergence up
front and blocking both.

**Whichever is chosen, choose it before writing the storage layer.** It is the one
decision that is expensive to reverse.

## Go to market

liquid, lake and the distro are the GTM vehicle for Metis because they solve the
problem Metis's own theory says is binding, and they solve it in a form an enterprise
already knows how to buy: source control, artifact management, CI, search, secrets and
deployment.

The sequence that follows:

1. **Land on the developer surface**, which is a budget line every enterprise already
   has, and which liquid can win on measurable grounds — build times, storage cost,
   supply-chain compliance.
2. **The data lands with it.** Adoption puts code, artifacts, datasets, documents and
   their provenance into one governed, searchable, permissioned store. That is Band C
   and Band B, manufactured as a side effect of a purchase made for other reasons.
3. **Score the decisions.** The forward-deployed motion now has an instrumented estate
   to assess rather than a survey.
4. **Zetta builds the instruments** on data that is now demonstrably available.

The honest version of the claim: **liquid does not make Metis true, it makes Metis
buildable at a customer** — because Metis's constraint is availability, and availability
is manufactured by infrastructure, not by analysis.

## Working conventions

- **Confidentiality.** Metis blueprint (`docs/metis/`) and competitor material
  (`docs/competitors/`) are confidential. This repository links and does not restate.
  Nothing from them goes to Perplexity, Firecrawl or any external service.
- **The bridge is the map.** Cross-product context lives in
  [`oramasearch/orama-bridge`](https://github.com/oramasearch/orama-bridge) (cloned
  locally as `orama-work`); per-product detail lives in the sub-bridges. Read a bridge's
  own `CLAUDE.md` before working inside it.
- **Blueprint-first.** Zetta is designed from the wtm `ORAMA-ZETTA` blueprint, projected
  onto the docs site and GitHub issues, both regenerable. If liquid converges with
  Zetta, liquid's design should enter that blueprint rather than staying a parallel
  document set.

## Open questions

- **The storage decision above.** Everything else waits on it.
- **One CRDT or two** — `automerge-cpp` versus Zetta's replication.
- **Licensing posture.** liquid now lives at `oramasearch/liquid` (private), so the
  umbrella question is settled. What is *not* settled is the licence: Metis is noted as
  source-available, and liquid has no stated position.
- **Naming.** liquid / lake / liquify / drip is a coherent family; Orama / Zetta / Metis
  is another. Whether they are presented as one portfolio or two is a positioning
  decision, not a technical one.
