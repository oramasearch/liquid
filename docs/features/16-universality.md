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

The generated declaration below is the current executable contract. Its `*` row is the universal
Tier 0 floor; named rows come directly from loaded grammar and compiler frontend modules. The
platform rows state where the language-independent Tier 1 wrapper is available.

<!-- tiers:begin -->
| Language | Extensions | Tier | Mechanism | Notes |
|---|---|---:|---|---|
| `*` | * | 0 | `chunking` | Every language and file type has this floor. |
| `bash` | .bash, .bash_profile, .bashrc, .ebuild, .eclass, .sh | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `c` | .c, .h | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `c#` | .cs | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `c++` | .cc, .cpp, .cxx, .h, .hh, .hpp, .hxx | 3 | `frontend(clang-cpp)` | Compiler frontend module. |
| `cmake` | .cmake, CMakeLists.txt | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `css` | .css | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `dart` | .dart | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `dockerfile` | .container, .Containerfile, .docker, .dockerfile, Dockerfile | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `elixir` | .ex, .exs | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `elm` | .elm | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `erlang` | .app, .app.src, .erl, .escript, .hrl, .rebar.config | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `fortran` | .F90, .f90, .F95, .f95 | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `go` | .go | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `haskell` | .hs, .hs-boot | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `hcl` | .hcl | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `html` | .html | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `java` | .java | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `javascript` | .cjs, .js, .jsx, .mjs | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `json` | .json | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `julia` | .jl | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `kotlin` | .kt, .kts | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `latex` | .tex | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `lua` | .lua | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `make` | .mk, Makefile | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `markdown` | .md | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `nix` | .nix | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `objective-c` | .h, .m, .objc | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `ocaml` | .ml | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `perl` | .pl, .pm, .t | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `php` | .php | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `protocol-buffers` | .proto | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `python` | .py | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `r` | .R, .r | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `ruby` | .rb | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `rust` | .rs | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `scala` | .sbt, .scala | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `scss` | .scss | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `solidity` | .sol | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `sql` | .sql | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `svelte` | .svelte | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `swift` | .swift | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `toml` | .toml | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `tsx` | .tsx | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `typescript` | .ts | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `verilog` | .sv, .svh, .v, .vh | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `vue` | .vue | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `xml` | .rng, .svg, .xml, .xsd, .xsl, .xslt | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `yaml` | .yaml, .yml | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |
| `zig` | .zig | 2 | `grammar(sha256:c6b377e72d6a387ac1c5d883100986eeb00e010eb829b7802c3c5620325f4f64)` | Digest-locked tree-sitter grammar. |

### Declared Tier 1 platforms

| OS | Tier 1 status | Sandbox | VFS | Notes |
|---|---|---|---|---|
| Linux | supported | `namespaces` | `FUSE` | Reference platform; libfuse3 is required. |
| macOS | requires macFUSE | `sandbox-exec` | `macFUSE` | Tier 1 is available when liquid is built with macFUSE. |
| Windows | unsupported | `unsupported` | `unsupported` | Storage and versioning remain available at Tier 0. |
| BSD | unsupported | `unsupported` | `unsupported` | No wrapper backend is registered. |
<!-- tiers:end -->

Tier 2 has a concrete distribution answer: the runtime and 49-grammar set are fetched from exact
public-upstream commits at configure time and pinned by archive and executable-file SHA-256 values
in one canonical lock. The lock digest identifies the complete set in derived records. Generated
parsers stay out of the source tree, and an optional URL-root override serves byte-identical
archives from an organisation or air-gapped mirror. The trust model and inventory are documented in
[`../library/grammars.md`](../library/grammars.md).

The implemented Tier 1 entry point is `liquid wrap`: local observed execution and caching work on
Linux with FUSE and on macOS when macFUSE is installed. Remote execution, artifact provenance,
packaging, Windows, and BSD are still future work, so the complete Tier 1 row remains a product
requirement rather than a claim about the current release.

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
- **Which additional per-language samples** should supplement the generated registry inventory as
  grammar behavior expands beyond #72's one committed parse sample per locked language.
