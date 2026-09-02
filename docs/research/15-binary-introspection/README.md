# Binary introspection and decomposition

> Evidence: [`sources.md`](sources.md)
>
> This is the "or decompiled" half of liquid's premise: software changes form in
> **both** directions. liquid should be able to take a built artifact apart and say
> what it is made of, where it came from, and whether it matches its source.

## Three distinct capabilities, often conflated

1. **Introspection** — read the structured metadata a binary already carries.
   Reliable, cheap, and by far the most valuable.
2. **Decomposition** — take a package or image apart into its constituent files and
   packages. Reliable and mechanical.
3. **Decompilation** — recover source-like code from machine code. Genuinely hard,
   lossy, and rarely recompilable.

liquid should invest heavily in (1) and (2), and treat (3) as an analysis feature
rather than a form of the "changes form" pipeline.

## Introspection: what binaries already tell you

**ELF** carries sections, segments, symbol tables, dynamic linkage info, and — the
key one — a **GNU build-id**: a unique hash embedded at link time. **debuginfod**
serves debug info *by build-id* over HTTP, so a stripped binary on a production
machine can have its symbols and even its sources fetched on demand. DWARF 5 and
split debug (`.debug_link`, DWZ compression) keep the shipping binary small.

Mach-O has dSYM bundles; PE has PDB and Microsoft's symbol server. The pattern is
identical: **a content identifier in the binary that maps back to build outputs.**

**Recoverable build provenance** varies a lot by toolchain:

| Toolchain | What's recoverable |
|---|---|
| **GCC** | `.comment` section with compiler version; `.GCC.command.line` with flags when enabled |
| **Clang** | Producer string in DWARF |
| **Go** | Excellent — `go version -m` / `debug/buildinfo` embeds module versions, VCS revision and build settings |
| **Rust** | Poor by comparison; no equivalent standard embedded manifest |

Go's design is the model: **the binary carries a machine-readable manifest of what
went into it.** That is exactly what liquid should emit, universally.

**diffoscope** is the reference tool for deep recursive comparison, covered in
[03-structural-diff-and-formats](../03-structural-diff-and-formats/). For binaries
it compares parsed headers, sections, symbols, relocations, disassembly and
strings, rather than treating an address shift as a total rewrite — which is what
makes it usable for reproducible-build verification.

## Decomposition: packages and images

Extracting `.deb`, `.rpm`, `.apk`, wheels, jars and OCI layers into a normalised
model of files plus metadata is mechanical. **Syft** produces SBOMs from images and
filesystems; **Tern** and **dive** decompose images layer by layer.

This is the same format-registry capability described in
[03](../03-structural-diff-and-formats/), and it should be one implementation
serving diff, storage dedup, SBOM generation and search.

## SBOM from binaries: the accuracy problem

Generating an SBOM from *source* is easy when you own the build. Generating one
from a **binary** is a different and much harder problem, and it is the common case
for third-party artifacts.

The hard part is **statically-linked vendored code**: a static binary containing a
copy of zlib has no package metadata saying so. Detection relies on version-string
matching, function-signature fingerprinting and heuristics (binwalk, ScanCode,
Trivy, Syft). It produces false positives, false negatives, and version guesses.

**This is the strongest argument for liquid's model.** liquid does not need to
*infer* an SBOM, because it already knows every input to every action
([04](../04-build-systems/)). A generated-from-the-build-graph SBOM is complete and
correct by construction, and it is free. Binary SCA is what you resort to when you
do not control the build.

## ABI extraction and comparison

`libabigail`/`abidiff` extracts and compares the actual ABI surface of a shared
library — types, symbols, layouts — and reports whether a change is
backward-compatible. Debian's `dpkg-gensymbols` tracks symbol versions per package.
`abi-compliance-checker` does the same from headers.

This answers the question underneath a large amount of packaging pain
([10](../10-os-packaging/)): *can this rebuilt library replace the installed one?*
It is currently a specialist tool used at release time. It should be continuous.

## Decompilation: honest assessment

Ghidra (with headless mode for automation), IDA/Hex-Rays, Binary Ninja, angr and
retdec are the 2024–2026 landscape, plus a research wave of LLM-assisted
decompilation (LLM4Decompile, DeGPT).

The honest position: decompiler output is useful for *understanding* and for
*security analysis*. It is rarely recompilable into an equivalent binary, types and
names are largely lost, and optimised code does not map back to source structure.
LLM assistance improves readability and naming but does not solve recompilability
and introduces confabulation risk.

**Do not build liquid's "decompile" story on decompilation.** Build it on
introspection and provenance: the reason to want source back from a binary is
almost always to understand what it is, where it came from, or whether it is
trustworthy — and liquid can answer all three *exactly* rather than approximately.

## Implications for liquid

1. **Emit a build-id and an embedded manifest in every artifact liquid produces.**
   Go's `debug/buildinfo` is the model, generalised: a machine-readable record of
   the action digest, the input tree digest, the toolchain, and a pointer to the
   provenance attestation ([13](../13-supply-chain-security/)). Any liquid binary
   found anywhere should be resolvable back to its exact build.

2. **Run a debuginfod-compatible service as part of the artifact service.** liquid
   already stores every intermediate, including debug info, addressed by content
   ([12](../12-artifact-distribution/)). Serving debug info by build-id is nearly
   free and is enormously valuable operationally — a stripped production binary
   becomes fully debuggable on demand. Ship stripped binaries *and* keep full
   symbols, without the usual tradeoff.

3. **`liquid explain <artifact>` as a headline command.** Given any artifact —
   built by liquid or not — decompose it, identify its constituent packages and
   files, extract whatever provenance it carries, and if liquid built it, show the
   exact source, inputs, toolchain and attestation chain. This is the concrete,
   demonstrable form of "software changes form in both directions."

4. **SBOM generated from the build graph, not inferred from bytes.** Complete and
   correct where liquid owns the build. Fall back to binary SCA for foreign
   artifacts, and **label the difference clearly** — an inferred SBOM and a derived
   one should never look alike to a consumer.

5. **Continuous ABI checking.** Extract the ABI surface of every library liquid
   builds and diff it against the previous version automatically. Feed the result
   into package metadata ([10](../10-os-packaging/)), into whether a
   content-addressed substitution is safe ([09](../09-package-managers/)), and into
   whether a running process can be hot-updated
   ([17](../17-runtime-hot-update/)). One extraction, three consumers.

6. **diffoscope-grade artifact comparison as a built-in.** Reproducibility
   verification needs to answer "these two builds differ — *how*?" in structural
   terms. This is the same recursive format registry as
   [03](../03-structural-diff-and-formats/).

7. **Treat decompilation as an optional analysis plugin.** Useful for inspecting
   third-party binaries; not part of the core pipeline, and not something to
   promise recompilability from.
