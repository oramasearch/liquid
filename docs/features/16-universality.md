# Universality

> **Requirement:** support all programming languages, all platforms, all operating
> systems.
>
> This document exists to state what that commitment actually costs, because it is
> the easiest promise to make and the hardest to keep.

## The tiered honesty model

"Supports every language" is meaningless without saying at what level. liquid
declares a tier per language, and the tier is documented rather than implied.

| Tier | What liquid provides | How |
|---|---|---|
| **0 — Storage and versioning** | Version, diff, merge, sync, search, share | Content-defined chunking is language-agnostic. **Every** language and file type gets this on day one, including binary formats and languages that do not exist yet. |
| **1 — Cached, distributed, attested builds** | Sandboxed execution, observed dependencies, caching, remote execution, provenance, packaging | `liquid wrap <any build command>`. Works for anything that runs as a process. **No language support required.** |
| **2 — Structural understanding** | Structural diff and merge, symbol search, precise navigation | A tree-sitter grammar. ~50 languages available immediately. |
| **3 — Fine-grained incrementality** | Per-definition rebuild, compile-as-you-type, hot-update classification | A compiler frontend integration. Small number of languages, growing slowly. |

**Tier 1 is the important claim.** Because dependencies are *observed* rather than
declared ([04](04-build-engine.md)), liquid does not need to understand a language to
build it correctly, cache it, distribute it and attest it. That is what makes
universality tractable rather than aspirational.

Tier 3 is where the marketing pressure will be, and it is where liquid must not
overpromise.

## Platforms and operating systems

| | Tier 0/1 | Virtual filesystem | Sandbox | Notes |
|---|---|---|---|---|
| **Linux** (glibc, musl) | Full | FUSE | namespaces, seccomp, cgroups | Strongest primitives. Reference platform. |
| **macOS** | Full | File Provider | Sandbox profiles | Kernel extensions deprecated; File Provider is the only path. Notarisation required for distribution. |
| **Windows** | Full | Cloud Files API | Job objects, restricted tokens | Weakest sandboxing primitives; path semantics and length limits differ. |
| **BSD** | Full | FUSE | jails | `automerge-cpp` already has FreeBSD CI. |

Architectures: x86-64, ARM64 as first class; RISC-V and others follow, and the
toolchain-as-input model ([06](06-distributed-execution.md)) makes cross-compilation
the normal case rather than a special one.

## What universality actually requires

The commitment is not "add more language plugins." It is four specific pieces of
discipline:

1. **Nothing language-specific in the core.** The object model, chunk store, action
   graph, cache and sync engine must contain no language knowledge whatsoever.
   Language support is exclusively plugins across a stable ABI
   ([17](17-implementation.md)).

2. **Observed dependencies, always.** The moment liquid requires declared
   dependencies for a language, it stops supporting languages it has not been taught.

3. **Graceful degradation, stated explicitly.** An unknown file type still chunks,
   dedups, versions and syncs. An unknown language still builds under `liquid wrap`.
   The user is told which tier they are getting, not left to infer it from behaviour.

4. **Platform differences encapsulated once.** Filesystem watching, virtual
   filesystems, sandboxing, process control and path semantics differ substantially
   across the three platforms. Each gets one abstraction with three implementations —
   not conditional code scattered through the system.

## The honest limits

Stating these prevents the promise from becoming a lie:

- **Sandboxing on Windows and macOS is weaker than on Linux.** Hermeticity guarantees
  are correspondingly weaker, and liquid should say which platform a given attestation
  came from.
- **Tier 3 requires per-language compiler work** and will never cover everything.
- **Some formats cannot be deterministically repacked** ([03](03-diff-and-merge.md)),
  so Tier 0 fidelity varies by format and is declared per format.
- **Reproducibility depends on the toolchain**, and some toolchains are not
  deterministic regardless of what liquid does.
- **Linux desktop integration has no standard** ([15](15-consumer-sync-and-web.md)).

## Open questions

- **Which languages get Tier 3 first**, and on what evidence — C and C++ are the
  hardest and the most valuable.
- **Whether Tier 2 grammars ship in-tree or are fetched**, which is a supply-chain
  question as much as a distribution one.
- **How to test universality** without an unbounded matrix.
