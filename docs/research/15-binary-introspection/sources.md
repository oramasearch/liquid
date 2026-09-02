<!-- Raw research source. Generated via Perplexity `perplexity_research` (web-grounded,
     multi-source) on 2026-09-01 for the liquid project. Lightly reformatted only.
     Treat as evidence, not as liquid design decisions - see README.md in this
     directory for the curated analysis and the implications drawn for liquid. -->

# Deep research: binary artifact introspection and “reverse packaging”  
**Scope: 2024–September 2, 2026**

## Executive conclusion

“Reverse packaging” is practical at three distinct levels:

1. **Lossless container decomposition:** package metadata, payload files, permissions, ownership, timestamps, signatures, layer ordering, and raw archive members can be extracted into a normalized model and repacked—sometimes byte-identically if original compressed streams and ordering are retained.
2. **Binary-semantic reconstruction:** executable structure, symbols, debug information, ABI, dependencies, compiler fingerprints, and probable embedded libraries can be recovered with varying confidence.
3. **Source reconstruction:** generally **not reversible**. Compilation discards names, comments, macros, source layout, many types, inlining boundaries, and alternative source expressions. Decompilers produce an analysis representation or C-like pseudocode, not the original source and usually not a turnkey reproducible build.

The strongest artifact-to-source chain is:

```text
running module
  → immutable module identity (ELF build-id / Mach-O UUID / PE CodeView GUID+age)
  → exact matching debug companion
  → DWARF/PDB line tables and compilation-unit paths
  → archived source snapshot
  → recorded toolchain/build recipe
  → independently reproduced, byte-identical artifact
```

An ELF build-id is an excellent **lookup key**, but not by itself proof of source provenance. The cryptographic verification step is the reproducible-build comparison: the same source, environment, and instructions must recreate bit-for-bit identical specified artifacts. [web:93]

---

# 1. Binary-format introspection

## 1.1 ELF: sections, segments, symbols, and dynamic state

ELF intentionally supplies two overlapping views:

| ELF view | Primary consumer | Important structures | What it reveals |
|---|---|---|---|
| **Linking/analysis view** | Linkers, debuggers, reverse-engineering tools | Section-header table and sections | Code/data organization, symbols, strings, relocations, notes, dynamic-link metadata, unwind and debug data |
| **Execution view** | Kernel loader and runtime linker | Program-header table and segments | Which file ranges are mapped, virtual addresses, alignment, permissions, interpreter, TLS and dynamic-linking data |

Sections hold instructions, data, symbols and relocations, while each program-header entry describes a segment needed to prepare the process image; a segment may contain one or more sections. [web:78][web:88]

Important sections include:

- `.text`, `.rodata`, `.data`, `.bss`
- `.symtab`/`.strtab`: full static symbol table and names, commonly removed by stripping
- `.dynsym`/`.dynstr`: symbols needed for dynamic linking; normally retained
- `.rela.*` or `.rel.*`: relocations
- `.dynamic`, `.got`, `.plt`: runtime-linking state
- `.note.gnu.build-id`: GNU build-id note
- `.eh_frame`/`.eh_frame_hdr`: unwind information, often present even without DWARF
- `.debug_info`, `.debug_abbrev`, `.debug_line`, `.debug_str`, `.debug_rnglists`, `.debug_loclists`, etc.: DWARF
- `.gnu_debuglink`: reference to a separate debug object
- `.GCC.command.line` and `.comment`: optional build clues

A basic inspection sequence is:

```bash
file program
readelf -h program                    # ELF class, ISA, ABI, entry point
readelf -S program                    # sections
readelf -lW program                   # loadable segments and section mapping
readelf -n program                    # notes, including build-id/properties
readelf -Ws program                   # static and dynamic symbols
readelf -d program                    # DT_NEEDED, SONAME, RPATH/RUNPATH
readelf -rW program                   # relocations
readelf --debug-dump=info,line program
objdump -drwC program
```

`readelf` explicitly distinguishes `--sections/-S` from `--segments/-l`. [web:81] A stripped executable may have no useful section table or `.symtab` yet remain executable because the loader primarily needs the ELF and program headers. Conversely, relocatable `.o` files need sections but generally have no loadable program image.

## 1.2 DWARF, split debug, DWARF 5, and DWZ

DWARF can retain:

- source file and line mappings;
- compilation directories;
- function, namespace and variable names;
- lexical scopes and inlining records;
- type graphs and structure layouts;
- location expressions describing where variables live over instruction ranges;
- compiler identity and sometimes command-line-related producer information.

Optimization limits it: a variable can be optimized out, split among registers and stack slots, folded into a constant, or cease to have a unique source-level representation.

### Separate-debug mechanisms

The section commonly called “debug link” is actually **`.gnu_debuglink`**, not `.debug_link`. It stores a debug-file name and a 32-bit CRC. GDB searches next to the executable, in a sibling `.debug` directory and under configured global debug directories. Alternatively, the executable and debug file can carry the same build-id; GDB then searches a path such as:

```text
/usr/lib/debug/.build-id/ab/cdef...debug
```

GDB validates `.gnu_debuglink` through the CRC and build-id debug files through the common build-id. [web:31]

Typical split-after-link workflow:

```bash
objcopy --only-keep-debug app app.debug
strip --strip-debug app
objcopy --add-gnu-debuglink=app.debug app
```

### Split DWARF

Compiler-time split DWARF is different from stripping after link:

- the `.o` retains a small **skeleton compilation unit**;
- most debug information goes into a `.dwo`;
- multiple `.dwo` files may be packaged into a `.dwp`.

DWARF 5 standardized split object/package representations and supplementary object support. [web:36] This reduces linker I/O and keeps production executables small, but a reverse-packaging system must collect the complete `.dwo`/`.dwp` closure, not merely the executable.

### DWZ

`dwz` performs semantic DWARF deduplication rather than ordinary byte compression. It rewrites equivalent representations into smaller forms and can move duplicated DIEs from several ELF debug objects into a shared multifile. References normally appear through `.gnu_debugaltlink`, containing the supplementary filename and its build-id; DWARF 5 also has the standardized `.debug_sup` mechanism. [web:404]

This creates an important dependency graph:

```text
executable → separate .debug file → shared .dwz/supplementary file
                              ↘ .dwo/.dwp files where applicable
```

An archive is not independently debuggable unless that entire graph is retained.

## 1.3 GNU build-id and debuginfod

A GNU build-id is normally stored as an ELF note named `GNU` with type `NT_GNU_BUILD_ID`. Linkers may derive it from build contents, use UUID-style data, or apply another configured policy; consumers should treat it as an opaque binary identity, not assume a particular hash construction.

`debuginfod` indexes executables, DWARF objects, and source associations by build-id. Its conceptual HTTP endpoints are:

```text
/buildid/<hex>/executable
/buildid/<hex>/debuginfo
/buildid/<hex>/source/<compilation-path>
```

The server learns source paths from the corresponding executable/debug information because source files themselves do not carry the executable’s build-id. [web:1] GDB can query servers in `DEBUGINFOD_URLS` and retrieve missing executables, debug information, and source on demand. [web:9]

### Mapping a live process to its exact build

On Linux:

1. Enumerate `/proc/<pid>/maps` and `/proc/<pid>/exe`.
2. Open each mapped ELF file, or read its notes from memory if the path was deleted/replaced.
3. Extract its build-id.
4. Query local `.build-id` storage or debuginfod.
5. Load matching DWARF.
6. Undo ASLR using each mapping’s load bias.
7. Resolve PCs to functions, inline call chains, source files and lines.
8. Retrieve source through debuginfod using the build-id plus DWARF compilation path.

This identifies the exact **binary build represented in the debug index**. It identifies exact source only if the server retained the correct source snapshot and indexed it consistently. Build-id lookup does not independently prove that source produced the binary.

## 1.4 Mach-O and dSYM

Mach-O uses:

- a header and architecture information;
- load commands such as `LC_SEGMENT_64`, `LC_LOAD_DYLIB`, `LC_SYMTAB`, `LC_DYSYMTAB`, `LC_FUNCTION_STARTS`, `LC_CODE_SIGNATURE`, and `LC_UUID`;
- segments such as `__TEXT`, `__DATA`, and `__LINKEDIT`;
- sections nested within segments, for example `__text` and `__cstring`;
- optional universal/fat wrappers containing multiple architecture slices.

Useful commands include:

```bash
file app
lipo -info app
otool -l app
nm -m app
dwarfdump --uuid app
codesign -dvvv app
```

Release-build DWARF is normally collected by `dsymutil` into:

```text
App.dSYM/Contents/Resources/DWARF/App
```

The original Mach-O and its dSYM carry matching build UUIDs; Apple states that a binary and dSYM are compatible only when those UUIDs are identical. [web:21] This UUID plays a role analogous to an ELF build-id for crash symbolication, although Apple symbol distribution infrastructure is not the same protocol as debuginfod.

## 1.5 PE/COFF, PDB, and Microsoft symbol servers

For PE images, inspect:

- DOS and PE/COFF headers;
- section table, e.g. `.text`, `.rdata`, `.data`, `.pdata`, `.rsrc`, `.reloc`;
- import/export tables;
- base relocations;
- exception/unwind data;
- resources and Authenticode data;
- debug-directory entries.

Modern Microsoft toolchains usually place a CodeView `RSDS` record in the PE debug directory containing:

```text
RSDS | PDB GUID | PDB age | original PDB path
```

The matching PDB carries the corresponding GUID/signature and age. Microsoft’s validation API specifically verifies both the PDB GUID and age, where age detects an out-of-sync PDB. [web:488]

Symbol servers index PDBs by a key conceptually equivalent to:

```text
<pdb-name>/<GUID><age>/<pdb-name>
```

The debugger extracts the CodeView identity from the loaded EXE/DLL and retrieves the matching PDB. Microsoft documents that symbol-server clients automatically select the correct symbol file for the target module. [web:484] Private symbol stores should retain source-indexing information or Source Link/source-server streams as well as PDBs; a PDB alone may expose source paths but not source contents.

---

# 2. Recovering build provenance from binaries

## 2.1 What can actually be recovered

| Evidence | Recoverable information | Reliability and caveats |
|---|---|---|
| ELF `.comment` | GCC, Clang, assembler or linker identification strings | Useful hint, but mergeable, removable, forgeable and potentially contains several producer versions |
| DWARF `DW_AT_producer` | Compiler family/version and sometimes enabled options | Strong per-compilation-unit evidence when DWARF survives; not necessarily a complete driver command |
| `.GCC.command.line` | Recorded compiler invocation/options | Present only when recording was enabled; may contain multiple NUL-terminated invocations |
| ELF notes/properties | ISA level, CET/BTI/PAC and some ABI properties | Describes generated artifact properties, not necessarily the exact original spelling of flags |
| Imports/code patterns | Stack protector, sanitizers, LTO, C++ runtime, optimization idioms | Inference only; absence of a recognizable pattern is not proof a flag was absent |
| Linker metadata | `DT_NEEDED`, SONAME, RPATH/RUNPATH, interpreter | Direct, but lists dynamic dependencies rather than statically linked components |
| Debug paths | source paths, build directories, generated files | Direct but privacy-sensitive and commonly remapped or stripped |
| Language-specific embedded data | Go modules/settings; optional Rust audit data | Potentially high-confidence, but ecosystem and build-mode dependent |
| Package metadata | package name/version, dependencies, architecture, build date/commit | Strong for package provenance, not proof that payload was built from that declared source |

### GCC

GCC normally emits producer text into `.comment` unless identification is disabled or removed. Exact switches are **not normally recoverable**. With `-frecord-gcc-switches`, supported targets record command-line options in a target-dependent section, conventionally `.GCC.command.line`. [web:62]

```bash
readelf -p .comment app
readelf -p .GCC.command.line app
readelf --debug-dump=info app | grep DW_AT_producer
```

The final linked section can contain records from multiple object files, so do not collapse it into one fictitious global command.

### Clang

On ELF, Clang supports:

```text
-frecord-command-line
-frecord-gcc-switches
```

and writes `.GCC.command.line`. Clang documents that linked output may contain multiple NUL-terminated command lines and that its escaping/serialization differs from GCC’s format. [web:67] Clang version strings may also appear in `.comment`, DWARF producer attributes, LTO bitcode or linker version records.

### Go

Go provides the most useful standardized post-build metadata among the languages in this comparison:

```bash
go version -m ./program
```

Programmatically:

```go
info, err := buildinfo.ReadFile(path) // package debug/buildinfo
```

Embedded information can include:

- Go toolchain version;
- main module path and version;
- dependency modules and replacement modules;
- build mode, compiler, tags and selected linker/compiler settings;
- `GOOS`, `GOARCH`, cgo state;
- VCS revision, timestamp and modified status when build-VCS recording is available.

The `debug/buildinfo` package reads this data directly from binaries and documents both the toolchain version and modules used in module mode. [web:46]

### Rust

A normal Cargo/rustc executable has **no Go-equivalent, stable, comprehensive dependency/build-info record**. It may leak useful evidence through symbols, panic locations, crate disambiguators, `.comment`, DWARF, strings or platform resources, but that is incomplete and not a supported dependency manifest.

`cargo-auditable` addresses this by embedding a compressed JSON dependency tree in a dedicated linker section; it must be enabled at build time. [web:120] Without that metadata, even Rust-aware binary auditing can miss embedded C libraries and many crates; Debian’s cargo-audit description estimates that panic-message recovery can miss roughly half of Rust dependencies. [web:125]

## 2.2 The binary-matching problem

To prove that candidate source corresponds to a released binary:

```text
candidate source + exact dependencies + exact toolchain
+ exact environment + exact build instructions
               ↓
             rebuild
               ↓
byte-for-byte comparison against the released artifact
```

The formal reproducible-build definition requires bit-identical specified artifacts given the same source, environment and instructions. [web:93]

Common mismatch causes include:

- timestamps, archive member ordering and filesystem traversal order;
- build paths embedded in DWARF or macros;
- locale, timezone and environment variables;
- randomly generated UUIDs/build-ids;
- compiler, assembler or linker patch-level differences;
- nondeterministic parallel linking;
- host CPU feature detection;
- dependency-resolution drift;
- ZIP/JAR timestamps and compression differences;
- signatures added after compilation.

A matching build-id is useful but weaker than matching the complete artifact hash. Conversely, a nonmatching build-id may be the *only* difference if the build-id itself was nondeterministic, so diagnosis requires structural comparison before concluding that machine code differs.

### diffoscope

`diffoscope` recursively opens supported containers and converts binary formats into more meaningful textual views before comparison. It understands, among many others, APK, Debian packages, RPM, ELF, PE, Mach-O, JAR/class files, ZIP, filesystems and compressed archives. [web:106]

```bash
diffoscope released.deb rebuilt.deb
diffoscope --html report.html released.tar rebuilt.tar
```

Use it to find the first semantic divergence—such as DWARF paths, archive timestamps, ELF notes or ZIP member metadata. It is a **diagnostic tool**, not a substitute for the final byte/hash equality criterion.

---

# 3. Decompilation state of the art, 2024–2026

## 3.1 Tool landscape

| Tool | 2024–2026 position | Output and automation | Practical assessment |
|---|---|---|---|
| **Ghidra** | Open-source full reverse-engineering suite. The 11.x line covered late 2023–2025; 11.4.2 added official Dockerized entry points including headless and PyGhidra. [web:177] | Native decompiler produces C-like output from Ghidra’s p-code analysis. `analyzeHeadless` imports/analyzes binaries and runs pre/post scripts recursively. [web:165] | Excellent automation and extensibility; output is pseudocode, not guaranteed compilable C. Types, signatures and structure improve greatly with symbols/DWARF and analyst annotations. |
| **IDA Pro + Hex-Rays** | Mature commercial benchmark; IDA 9.x expanded ARM64, PPC, RISC-V and embedded-architecture support. IDA 9.3 added V850 decompilation and control-flow improvements. [web:139] | Interactive and batch decompilation, microcode API, rich signature/type ecosystem | Often strong on compiler idioms and difficult commercial targets; per-architecture decompilers are licensed products. Still subject to all information-loss limits. |
| **Binary Ninja** | Commercial RE platform centered on LLIL → MLIL → HLIL | HLIL is the decompiler output, with optional Pseudo-C rendering and a Python/C++ API; commercial editions support headless library use. [web:461][web:142] | Excellent programmatic data-flow and SSA access. Its documentation explicitly says HLIL is not intended to be literal C. [web:462] |
| **angr** | Python binary-analysis platform combining lifting, CFG recovery, symbolic execution and decompilation | Lifts into AIL, recovers variables/types and control structures, then generates code | Particularly valuable where decompilation must feed program analyses. Its decompiler pipeline includes CFG recovery, calling-convention recovery, variable/type inference, structuring and code generation. [web:151] |
| **RetDec** | LLVM-based retargetable decompiler | Command-line/library pipeline; LLVM bitcode and C-like output | Useful open pipeline and signatures, but by 2026 the project explicitly described itself as being in limited-maintenance mode. [web:152] |
| **LLM-assisted systems** | Rapid research expansion: LLM4Decompile, DeGPT/DecGPT, ReF Decompile, SALT4Decompile, Decompile-Bench and others | Assembly-to-source generation, or refinement/renaming/commenting of traditional decompiler output | Best viewed as a probabilistic refinement/recovery layer, not a trusted semantics-preserving replacement. |

Ghidra 11.0 also introduced initial Rust mangling/calling-convention support. [web:448] As of the 2026 snapshot, 11.x is historically important but is no longer the newest branch; Ghidra 12.x succeeded it.

## 3.2 Accuracy and recompilability

Decompilation quality has several separate dimensions:

1. **Syntactic recompilability:** generated text is accepted by a compiler.
2. **Test re-executability:** recompiled output passes available tests.
3. **Semantic equivalence:** all externally observable behavior is equivalent.
4. **Readability:** humans understand the result.
5. **Source fidelity:** recovered constructs resemble the original source.

Only the first two are routinely measured, and passing a finite test suite does not prove semantic equivalence.

### LLM4Decompile

The original LLM4Decompile work introduced evaluation based on recompilation and execution against tests. The early 6B model reported about **21%** all-tests-passing re-executability. [web:180] A later EMNLP result reported **45.4% on HumanEval and 18.0% on ExeBench** for LLM4Decompile-End-6.7B, substantially ahead of its compared Ghidra/GPT-4o baselines in that setup. [web:182]

These numbers are benchmark-, compiler-, architecture- and optimization-dependent. The research scope was primarily C targeting x86, not arbitrary C++, Rust, kernels, obfuscated malware or whole multi-module applications. [web:203]

### DeGPT and related refinement systems

DeGPT does not primarily replace the conventional decompiler; it improves Ghidra output through variable renaming, structure simplification and generated comments. Its study reported a **24.4% reduction in measured cognitive burden**, while **62.9% of generated comments** supplied practically useful semantics. [web:210]

DecGPT used iterative compiler feedback to raise recompilation success from a 45% baseline to 75% on its selected 300-case experiment. [web:213] That is valuable for creating analyzable scaffolding, but compilation success alone says little about behavioral correctness.

A 2025 real-world benchmark found that LLM methods improved understandability while exhibiting **52.2% lower functional correctness** than the industrial-strength tools being compared. [web:198] This captures the central trade-off: an LLM can produce persuasive names and clean control flow while silently inventing semantics.

## 3.3 Why exact source recovery remains impossible in general

Machine code does not uniquely determine source. Compilation can erase or transform:

- local names and comments;
- typedefs and many signedness distinctions;
- macros, templates and generics;
- loop syntax and source control-flow structure;
- inlined function boundaries;
- unused branches, variables and whole functions;
- structure field names;
- constant expressions and source literals;
- undefined-behavior-dependent intent;
- exception abstractions and high-level ownership models.

Additional obstacles include stripping, LTO, identical-code folding, tail-call optimization, custom calling conventions, packed/encrypted code, anti-disassembly, JIT generation and self-modification.

Therefore:

- Ghidra/IDA/Binary Ninja output should be preserved as an **analysis IR plus annotations**, not represented as recovered original source.
- Recompiled decompiler output will almost never be byte-identical to the input.
- For rebuildability, create explicit stubs for unresolved externals, recovered data blobs, linker scripts, ABI shims and test harnesses.
- Validate reconstructed functions with differential tests, emulation, fuzzing and symbolic equivalence where feasible.

---

# 4. Programmatic package unpacking and normalization

## 4.1 Format-specific decomposition

| Format | Physical structure | Metadata to extract | Payload/rebuild notes |
|---|---|---|---|
| **Debian `.deb`** | `ar` archive containing ordered `debian-binary`, `control.tar.*`, and `data.tar.*` members | `control`, conffiles, triggers, maintainer scripts, checksums, package/version/arch/dependencies | `data.tar.*` is the filesystem tree. Preserve ar member order, compression stream, owners, modes, mtimes, links and control scripts. Debian documents the three-part structure. [web:246] |
| **RPM** | Lead, signature header, main header and compressed CPIO payload | NEVRA, dependencies/provides, file list, digests, scripts, triggers, capabilities, signatures, source RPM/build host/time | Parse with librpm rather than assuming fixed tags. RPM’s four logical sections and CPIO payload are documented by rpm.org. [web:240] |
| **Alpine APK v2** | Three concatenated gzip streams/tar sections: signature, control, data | `.PKGINFO`, install scripts, package signature, `datahash`, origin, commit, dependencies | Generic gzip libraries may stop after the first member. Parse every concatenated stream and preserve PAX per-file checksums. [web:431][web:433] |
| **Alpine APK v3** | New apk-tools container/schema rather than assuming v2 tar behavior | Package contents, metadata and signatures through apk-tools’ v3 APIs/specification | Dispatch by format version and use libapk/apk-tools-compatible parsing; do not run a v2-only extractor. |
| **Android `.apk`** | Signed ZIP containing binary manifest/resources, DEX, assets and optional native `.so` files | Package/version/SDK/permissions/components, signing certificates, DEX/classes, resource table, native ABIs | Distinguish from Alpine APK by magic/structure. `apkanalyzer` separately analyzes manifest and DEX content. [web:416] Preserve the signing block and original ZIP if signature-valid round-trip is required. |
| **Python wheel** | ZIP plus `{distribution}-{version}.dist-info/` | `METADATA`, `WHEEL`, tags, licenses, entry points, direct URL and `RECORD` | `RECORD` lists nearly all files and strong hashes; regenerate it after modifications. [web:255] |
| **JAR/WAR/EAR** | ZIP with Java classes/resources and `META-INF` | `MANIFEST.MF`, Maven `pom.properties`/POM, module info, services, signatures, multi-release entries | Preserve duplicate entries, ordering and signature files. Java recognizes `META-INF/MANIFEST.MF` as package-related metadata. [web:258] |
| **OCI image** | Index/manifest, config JSON and content-addressed layer blobs | platform, config, environment, entry point, history, diff IDs, annotations, layer digests | Apply base-to-top tar changesets. `.wh.<name>` deletes a lower-layer path; opaque-directory whiteouts hide lower contents. OCI layers and removals are formally specified. [web:231] |

Useful native entry points include:

```bash
ar x package.deb
dpkg-deb --raw-extract package.deb out/

rpm -qp --qf ... package.rpm
rpm2cpio package.rpm | cpio -idmv

python -m zipfile -e package.whl out/
jar tf package.jar
unzip package.apk

skopeo copy docker://repo/image:tag oci:image-layout:tag
```

For untrusted artifacts, run decompression and package-script parsing in a sandbox. Never execute maintainer scripts merely to inspect them.

## 4.2 A normalized reverse-package model

A useful representation separates immutable observations from interpretations:

```text
Artifact
 ├─ identity: digest, size, media type, format/version
 ├─ raw_components[]: byte offset, length, original bytes/digest
 ├─ metadata[]: namespace, key, typed value, raw encoding, provenance
 ├─ filesystem_nodes[]
 │    ├─ path as raw bytes and normalized display path
 │    ├─ type, mode, uid/gid, mtime
 │    ├─ content digest/blob
 │    ├─ xattrs, capabilities, ACLs, link target
 │    └─ package/layer ownership
 ├─ relationships[]: depends/provides/contains/derived-from/overlays
 ├─ executable_facts[]: format, build-id/UUID/PDB identity, ABI, symbols
 ├─ signatures[]: covered byte ranges, certificate/key, validation state
 ├─ confidence[]: asserted / directly observed / inferred / heuristic
 └─ rebuild_recipe: ordering, compression, alignment and serialization rules
```

Two rebuild modes should be distinguished:

- **Canonical rebuild:** deterministic ordering, timestamps and compression; semantically equivalent but not necessarily byte-identical to the original.
- **Forensic round-trip:** retain original member streams, padding, unknown fields and signatures; unchanged artifacts can be emitted byte-for-byte.

## 4.3 Syft, Tern and dive

- **Syft** catalogs container images, filesystems, archives and individual files and can emit standard SBOM formats. Its `--scope all-layers` mode includes packages that existed in earlier layers but were later deleted. [web:278]
- **Tern** analyzes a container layer by layer, identifies its distribution/package manager, executes known inspection methods in a controlled filesystem context and attributes packages to layers. [web:286]
- **dive** is primarily a visual/filesystem decomposition tool. It shows additions, modifications and removals per layer and estimates space wasted by duplicated or later-deleted files. [web:300]

These are complementary: Syft is strongest as a catalog/SBOM engine, Tern emphasizes package attribution and installation history, and dive emphasizes layer/file visualization rather than complete software composition.

---

# 5. SBOM generation from binaries

## 5.1 Evidence hierarchy

Binary-derived SBOMs should report evidence and confidence rather than present all detections as equally certain:

| Confidence | Evidence | Example |
|---|---|---|
| **Very high** | Signed package database or explicit embedded manifest | RPM/dpkg/APK database; Go build info; cargo-auditable dependency section |
| **High** | Archive-native metadata with content hashes | wheel `METADATA`/`RECORD`; Maven POM inside JAR |
| **Medium-high** | Exact build-id mapped to trusted build/debug repository | ELF maps to distribution package and archived source |
| **Medium** | Dynamic imports, SONAMEs, symbol versions | `libssl.so.3`, `GLIBC_2.34` |
| **Medium-low** | Unique version strings, copyright text, symbol clusters | `OpenSSL 3.0.x`, named zlib functions |
| **Low/heuristic** | Function fingerprints, constants or ML similarity | suspected statically linked and optimized vendored library |

A dynamic dependency list is not an SBOM: it misses static linkage, code copied directly into the project, header-only libraries, generated code and libraries eliminated by optimization.

## 5.2 Detecting statically linked vendored libraries

For a suspected vendored zlib in a static stripped binary:

1. Search preserved symbols:
   ```bash
   nm -C app | grep -E 'inflate|deflate|crc32'
   ```
2. Search version/copyright strings.
3. Identify zlib’s characteristic constant tables.
4. Compare normalized functions against reference builds compiled for the same architecture, compiler and optimization levels.
5. Correlate call graphs and sets of related functions; do not decide from one short common function.
6. Record version as a range if several releases share the fingerprint.

Zlib is unusually amenable to this: version-specific constant-table bit patterns can identify likely static copies independent of compiler and, except for endianness, architecture. [web:354]

Optimizers complicate generic detection through inlining, dead-code elimination, constant folding, function splitting/merging and LTO. Forks and backports also break the assumption that “matching version X” implies pristine upstream version X.

## 5.3 Tool capabilities and limitations

| Tool | Binary-derived strengths | Important limitation |
|---|---|---|
| **Syft** | OS package databases, Java archives, wheels, Go binaries, cargo-auditable, PE/.NET and binary classifiers; works over images, archives and filesystems | Coverage varies strongly by ecosystem and available embedded metadata |
| **Trivy** | Image/rootfs/package inventory, Go and metadata-enabled Rust binaries, SBOM generation and vulnerability correlation | Its documentation says generic third-party/self-compiled packages and binaries are not generally supported; vendor packages and explicit embedded data are much stronger. [web:316] |
| **ScanCode Toolkit** | Package/license/copyright inventory; optional compiled-binary scan for structured package/dependency metadata in ELF, PE and Mach-O | Structured metadata search is not arbitrary native static-library identification; enabling it is slower. [web:326] |
| **binwalk** | Signature scanning, carving and recursive extraction of embedded archives/filesystems/firmware components | Finds embedded objects and magic patterns, not a complete dependency graph or reliable package version by itself. Binwalk describes its core function as identifying and optionally extracting embedded files/data. [web:330] |
| **Tern** | Layer-by-layer package inventory and attribution | Focused on containers and package-manager-visible components |
| **dive** | Files and history by image layer | Not principally an SBOM or vulnerability-resolution engine |

## 5.4 Accuracy problems in binary SCA

Major false-negative causes:

- stripped symbols and absent debug information;
- static linking and LTO;
- inlining or elimination of recognizable functions;
- packed/encrypted code;
- uncommon architecture or compiler;
- incomplete reference-signature corpus;
- code copied from a library rather than linked as a unit;
- embedded language runtimes that obscure dependency boundaries.

Major false-positive causes:

- shared utility functions or compiler-generated routines;
- forks sharing most upstream code;
- signatures too short to be discriminative;
- version strings retained despite code being unused;
- backported patches carrying an old public version;
- multiple versions embedded simultaneously.

The problem remains materially imperfect. A 2026 study of C/C++ binary third-party-library detectors reported average recall below 60% and precision around 75%. [web:351] A stronger research system, BinaryAI, reported 85.84% precision but only 64.98% recall in its evaluation, illustrating that substantial components can still be missed even when precision improves. [web:345]

Accordingly, binary SBOM entries should carry:

```text
component, candidate version/range, evidence type,
matched functions/strings/constants, confidence,
tool/database version, target compiler/ISA assumptions
```

Do not automatically convert a low-confidence fingerprint into a vulnerability assertion.

---

# 6. Symbol and ABI extraction/comparison

## 6.1 What constitutes an ABI surface

At minimum:

- exported symbol names, versions, binding and visibility;
- function calling conventions and parameter/return types;
- public variable types and sizes;
- C++ mangled names, vtables, RTTI and exception ABI;
- public class/structure size, alignment, base classes and field offsets;
- enum underlying values;
- symbol-version nodes;
- SONAME/install-name;
- architecture-specific register and aggregate-passing rules.

A symbol-only comparison detects additions/removals but cannot reliably find layout changes behind an unchanged symbol. Comprehensive C/C++ ABI analysis usually requires public headers, DWARF/CTF/BTF, or both.

## 6.2 Tool comparison

| Tool | Extraction model | Diff model | Best use and limitation |
|---|---|---|---|
| **libabigail: `abidw`, `abidiff`, `abipkgdiff`** | Reads ELF plus DWARF by default, falling back to CTF/BTF or symbols; constructs an ABI corpus from externally visible functions/variables and reachable types | Reports changes to symbols, functions, variables, types, field offsets, sizes and layouts | Strong binary-first ELF ABI analysis. Without debug/type information, it degrades largely to symbol additions/removals. [web:375][web:378] |
| **ABI Dumper + ABI Compliance Checker** | ABI Dumper extracts ELF/DWARF into a dump; the checker can also work from descriptors containing headers and shared objects | Compares old/new ABI dumps and produces detailed binary/source compatibility reports | Effective for release gates when public headers and suitable debug builds exist. Typical flow is `abi-dumper OLD.so`, `abi-dumper NEW.so`, then `abi-compliance-checker`. [web:386] |
| **Debian `dpkg-gensymbols`** | Scans shared libraries in a package build tree and creates/updates Debian symbols files | Diffs generated exported-symbol sets against the maintainer’s template; associates each symbol with the minimum package version that supplied it | Excellent for Debian dependency precision and symbol-removal checks, but not a full type/layout ABI verifier. [web:373] |

Examples:

```bash
# libabigail
abidw libold.so > old.abi
abidw libnew.so > new.abi
abidiff libold.so libnew.so

# ABI Compliance Checker
abi-dumper libold.so -o old.dump -lver 1
abi-dumper libnew.so -o new.dump -lver 2
abi-compliance-checker -l mylib -old old.dump -new new.dump

# Debian packaging
dpkg-gensymbols -plibmylib1 -Pdebian/tmp -c4
```

`dpkg-gensymbols` records the minimum package version associated with each exported symbol, normally the first version that supplied it, and can fail the build when generated symbols differ too much from the reference. [web:360] It therefore answers “what package version must a consumer depend on?” rather than “did a field move inside an exported C++ class?”

---

# Recommended architecture for a reverse-packaging system

A robust implementation should use a staged, recursive pipeline:

```text
1. Hash and identify outer artifact
2. Parse container without executing embedded code
3. Preserve raw byte ranges and unknown metadata
4. Extract filesystem graph and package metadata
5. Recursively classify nested archives and executables
6. Extract binary identities, symbols, DWARF/PDB/dSYM and ABI
7. Query trusted symbol/debug/source services
8. Generate evidence-scored component inventory/SBOM
9. Produce decompiler projects and optional source-like scaffolding
10. Emit canonical and forensic rebuild recipes
11. Repack, reparse and verify invariants
12. If source is claimed, independently rebuild and compare bytes
```

The output should explicitly distinguish:

- **observed fact:** “ELF contains build-id `…`”;
- **repository assertion:** “debuginfod maps it to package/source `…`”;
- **inference:** “function fingerprints resemble zlib 1.2.11–1.2.13”;
- **reconstruction:** “generated C-like implementation passes these tests”;
- **verification:** “independent build is byte-for-byte identical.”

That separation is the key to making reverse packaging useful without overstating what a compiled artifact can reveal.

Citations:
[31] https://www.sourceware.org/gdb/current/onlinedocs/gdb.html/Separate-Debug-Files.html
[32] https://sourceware.org/pipermail/gdb-patches/2023-January/195929.html
[33] https://sourceware.org/pipermail/gdb-patches/2023-January/195927.html
[34] https://stackoverflow.com/questions/16408893/gnu-ld-gdb-separate-debug-files-how-to-produce-the-debug-file-when-theres-too
[35] https://lore.kernel.org/lkml/87si6pfwz4.fsf@secretsauce.net/T/
[36] https://dwarfstd.org/dwarf5std.html
[37] https://sourceware.org/gdb/current/onlinedocs/gdb.html/Debuginfod-Settings.html
[38] https://totalview.io/sites/totalview/files/pdfs/white-paper-totalview-saving-time-space-split-dwarf.pdf
[39] https://dwarfstd.org/issues/130313.4.html
[40] https://inbox.sourceware.org/gdb/87blohl3an.fsf@redhat.com/T/
[41] https://www.ibm.com/docs/en/cpp-oel-zos/3.0.0?topic=options-gsplit-dwarf
[42] https://sparkles-docs.pages.dev/research/autological-artifacts/debug-info-and-indexes
[43] https://github.com/libunwind/libunwind/issues/600
[44] https://gnu.wildebeest.org/~mark/perftools-toronto-2018/SmallSplitDWARF5.pdf
[45] https://developers.redhat.com/articles/2022/01/10/gdb-developers-gnu-debugger-tutorial-part-2-all-about-debuginfo
[16] https://blog.s-schoener.com/2025-01-27-dbghelp-symbols/
[17] https://gist.github.com/0blu/ee3663ed65e8a279873b33c9448376dc
[18] https://community.osr.com/t/pdb-age-discrepancy/44339
[19] https://stackoverflow.com/questions/74622676/check-if-mac-executable-has-debug-info
[20] https://gist.github.com/Barakat/e20ec993748c6ab382d727f08eaa523e
[21] https://developer.apple.com/documentation/xcode/building-your-app-to-include-debugging-information
[22] https://karol-mazurek.medium.com/snake-apple-i-mach-o-a8eda4b87263
[23] https://developer.apple.com/forums/thread/84942
[24] https://docs.sentry.io/platforms/apple/guides/macos/data-management/debug-files/file-formats/
[25] https://learn.microsoft.com/en-us/visualstudio/debugger/debug-interface-access/idiadatasourceex-validatepdb?view=visualstudio
[26] https://stackoverflow.com/questions/744870/how-can-you-change-an-age-mismatched-pdb-to-match-properly
[27] https://developer.apple.com/forums/thread/775650
[28] https://reverseengineering.stackexchange.com/questions/15627/obtain-exe-dll-sys-for-a-given-pdb-file
[29] https://community.osr.com/t/how-to-get-windows-version-from-symbol-server-file-directory/52636
[30] https://learn.microsoft.com/ka-ge/visualstudio/debugger/debug-interface-access/idiadatasourceex-validatepdb?view=vs-2022
[1] https://developers.redhat.com/blog/2019/10/14/introducing-debuginfod-the-elfutils-debuginfo-server
[2] https://deepwiki.com/sailfishos-mirror/elfutils/7.1-debuginfod-server:-indexing-database-and-http-api
[3] https://manpages.debian.org/experimental/libdebuginfod-dev/debuginfod_find_source.3.en.html
[4] https://manpages.debian.org/unstable/libdebuginfod-dev/debuginfod_find_debuginfo.3.en.html
[5] https://www.redhat.com/en/blog/elf-section-retrieval-debuginfod
[6] https://www.sourceware.org/gdb/current/onlinedocs/gdb.html/Separate-Debug-Files.html
[7] https://manpages.debian.org/testing/debuginfod/debuginfod-find.1.en.html
[8] https://www.mankier.com/8/debuginfod
[9] https://www.sourceware.org/gdb/current/onlinedocs/gdb.html/Debuginfod.html
[10] https://manpages.debian.org/experimental/debuginfod/debuginfod-find.1.en.html
[11] https://github.com/getsentry/symbolicator/issues/445
[12] https://www.archcloudlabs.com/projects/debuginfod/
[13] https://developers.redhat.com/articles/2022/01/10/gdb-developers-gnu-debugger-tutorial-part-2-all-about-debuginfo
[14] https://manpages.debian.org/unstable/debuginfod/debuginfod-find.1.en.html
[15] https://docs.rs/debuginfod/latest/debuginfod/enum.BuildId.html
[76] https://refspecs.linuxfoundation.org/elf/x86_64-abi-0.95.pdf
[77] https://refspecs.linuxbase.org/elf/gabi4+/ch4.sheader.html
[78] https://refspecs.linuxfoundation.org/elf/gabi41.pdf
[79] https://refspecs.linuxfoundation.org/elf/x86_64-abi-0.98.pdf
[80] https://refspecs.linuxfoundation.org/elf/gabi4+/contents.html
[81] https://man7.org/linux/man-pages/man1/readelf.1.html
[82] https://refspecs.linuxbase.org/elf/gabi4+/ch4.intro.html
[83] https://wiki.osdev.org/ELF
[84] https://man7.org/linux/man-pages/man5/elf.5.html
[85] https://docs.oracle.com/cd/E19253-01/816-1681/elf-11185/index.html
[86] https://refspecs.linuxfoundation.org/elf/elf.pdf
[87] https://gabi.xinuos.com/v42/elf.pdf
[88] https://refspecs.linuxbase.org/elf/gabi4+/ch5.pheader.html
[89] https://docs.oracle.com/cd/E19120-01/open.solaris/816-1681/elf-11185/index.html
[90] https://dev.to/cangulmez/elf-executable-analysis-in-detail-3p5i
[61] https://reviews.llvm.org/D54489
[62] https://gcc.gnu.org/wiki/Record_GCC_command_line_switches_in_object_files
[63] https://stackoverflow.com/questions/33407016/how-to-tell-a-library-was-compiled-using-c11
[64] https://github.com/samtools/samtools/issues/515
[65] https://medium.com/@fixitblog/solved-finding-out-if-a-binary-file-has-been-compiled-with-stack-smashing-protection-469f25d510ea
[66] https://gcc.gnu.org/onlinedocs/gcc/Option-Summary.html
[67] https://clang.llvm.org/docs/ClangCommandLineReference.html
[68] https://stackoverflow.com/questions/12112338/get-the-compiler-options-from-a-compiled-executable
[69] https://discourse.nixos.org/t/customizing-cflags-rebuilds-too-much/60506
[70] https://jundolssite.wordpress.com/2013/02/01/gcc-default-flags-%ED%99%95%EC%9D%B8%ED%95%98%EA%B8%B0/
[71] https://book.crifan.org/books/exec_file_format_elf/website/elf_overview/section/
[72] https://unix.stackexchange.com/questions/65625/can-i-read-what-ccflags-were-used-for-building-a-binary
[73] https://stackoverflow.com/questions/78349041/how-to-generate-assembly-code-with-the-clang-invocation-recorded-in-the-output
[74] https://www.phoronix.com/news/GCC-Record-Command-Line-Info
[75] https://fragglet.github.io/dos-help-files/clang.hlp/comment.html
[46] https://pkg.go.dev/debug/buildinfo
[47] https://go.dev/src/debug/buildinfo/buildinfo.go
[48] https://groups.google.com/g/golang-checkins/c/oQzaX-GiKXs
[49] https://www.piotrbelina.com/blog/go-build-info-debug-readbuildinfo-ldflags/
[50] https://forum.golangbridge.org/t/debug-buildinfo-main-version-devel/26068
[51] https://pkg.go.dev/github.com/shogo82148/std/debug/buildinfo
[52] https://groups.google.com/g/golang-codereviews/c/304wHhywnM8
[53] https://pkg.go.dev/cmd/go/internal/version
[54] https://pkg.go.dev/runtime/debug
[55] https://go.dev/src/runtime/debug/mod.go
[56] https://go.dev/src/debug/buildinfo/buildinfo_test.go
[57] https://pkg.go.dev/github.com/mandiant/GoReSym/buildinfo
[58] https://shibumi.dev/posts/go-18-feature/
[59] https://users.rust-lang.org/t/does-exe-file-contain-version-information/85096
[60] https://github.com/golang/go/issues/67745
[120] https://github.com/rust-secure-code/cargo-auditable
[121] https://github.com/rust-secure-code/cargo-auditable/blob/master/README.md
[122] https://ubuntu.com/project/docs/contributors/language-specific/rust/cargo-auditable/
[123] https://www.reddit.com/r/rust/comments/y4oiyw/introducing_cargoauditable_audit_rust_binaries/
[124] https://anchore.com/blog/beyond-cargo-audit-securing-your-rust-crates-in-container-images/
[125] https://packages.debian.org/sid/cargo-audit
[126] https://images.chainguard.dev/directory/image/rust/overview
[127] https://tessl.io/registry/testland/cargo-audit-rust/1.2.8
[128] https://rustsec.org/
[129] https://docs.rs/auditable/latest/src/auditable/lib.rs.html
[130] https://github.com/rust-secure-code/cargo-auditable/discussions/127
[131] https://doc.rust-lang.org/cargo/commands/cargo.html
[132] https://users.rust-lang.org/t/specifying-cargo-binary-tools-ex-audit-in-cargo-toml/83485
[133] https://blog.rust-lang.org/inside-rust/2023/09/04/keeping-secure-with-cargo-audit-0.18/
[134] https://www.reddit.com/r/rust/comments/awlqfi/rustaudit_making_production_rust_binaries/
[91] https://www.arxiv.org/pdf/2505.21642
[92] https://reproducible-builds.org/docs/plans/
[93] https://reproducible-builds.org/docs/definition/
[94] https://nesbitt.io/2026/02/24/reproducible-builds-in-language-package-managers.html
[95] https://en.wikipedia.org/wiki/Reproducible_builds
[96] https://havenmessenger.com/blog/posts/reproducible-builds-explained/
[97] https://link.springer.com/article/10.1007/s11219-022-09607-z
[98] https://blogs.vmware.com/opensource/2022/07/12/what-makes-a-build-reproducible-part-1/
[99] https://reproducible-builds.org/events/berlin2016/reproduciblebuildsdefinition/
[100] https://safeguard.sh/resources/blog/what-is-reproducible-builds
[101] https://www.computer.org/csdl/magazine/so/2022/02/09403390/1sLHasmdbXi
[102] https://aws.amazon.com/blogs/web3/establishing-verifiable-security-reproducible-builds-and-aws-nitro-enclaves/
[103] https://www.ssdnodes.com/learn/reproducible-builds-explained
[104] https://fossa.com/glossary/reproducible-builds/
[105] https://ar5iv.labs.arxiv.org/html/2104.06020
[106] https://diffoscope.org/
[107] https://www.mankier.com/1/diffoscope
[108] https://www.diva-portal.org/smash/get/diva2:1877032/FULLTEXT01.pdf
[109] https://try.diffoscope.org/
[110] https://lutpub.lut.fi/bitstream/handle/10024/135304/MariaGlukhova_ToolsForEnsuringReproducibleBuildsForOpenSourceSoftware.pdf?sequence=2
[111] https://linuxcommandlibrary.com/man/diffoscope
[112] https://reproducible-builds.org/tools/
[113] https://reproducible-builds.org/_lfs/releases/diffoscope/
[114] https://fedoramagazine.org/use-diffoscope-in-packager-workflows/
[115] https://layers.openembedded.org/layerindex/recipe/120174/
[116] https://www.x-cmd.com/install/diffoscope/
[117] https://users.rust-lang.org/t/how-to-perform-reproducible-builds-on-windows/133356
[118] https://news.ycombinator.com/item?id=39962303
[119] https://askubuntu.com/questions/899774/recursively-compare-counts-of-specific-files-between-two-directories
[165] https://www.ghidradocs.com/10.2_PUBLIC/help/Base/help/topics/HeadlessAnalyzer/HeadlessAnalyzer.htm
[166] https://www.ghidradocs.com/9.0.4_PUBLIC/help/Base/help/topics/HeadlessAnalyzer/HeadlessAnalyzer.htm
[167] https://www.ghidradocs.com/11.1_PUBLIC/docs/GhidraClass/Intermediate/HeadlessAnalyzer.html
[168] https://www.ghidradocs.com/11.0_PUBLIC/support/analyzeHeadlessREADME.html
[169] https://www.ghidradocs.com/11.2_PUBLIC/support/analyzeHeadlessREADME.html
[170] https://ghidra.re/ghidra_docs/api/ghidra/app/util/headless/HeadlessAnalyzer.html
[171] https://ghidra.re/ghidra_docs/api/ghidra/app/util/headless/AnalyzeHeadless.html
[172] https://www.ghidradocs.com/
[173] https://github.com/NationalSecurityAgency/ghidra/blob/master/Ghidra/RuntimeScripts/Windows/support/analyzeHeadless.bat
[174] https://github.com/NationalSecurityAgency/ghidra/blob/master/Ghidra/Features/Base/src/main/java/ghidra/app/util/headless/HeadlessAnalyzer.java
[175] https://github.com/NationalSecurityAgency/ghidra/blob/master/Ghidra/RuntimeScripts/Common/support/analyzeHeadlessREADME.html
[176] https://github.com/NationalSecurityAgency/ghidra/blob/master/Ghidra/Features/Base/src/main/java/ghidra/app/util/headless/AnalyzeHeadless.java
[177] https://www.ghidradocs.com/11.4.2_PUBLIC/docs/ChangeHistory.html
[178] https://ghidra.re/ghidra_docs/api/ghidra/app/util/headless/HeadlessScript.html
[179] https://ghidradocs.com/11.1_PUBLIC/docs/WhatsNew.html
[135] https://hex-rays.com/decompiler
[136] https://hex-rays.com/blog/2025-product-vision-and-goals
[137] https://docs.hex-rays.com/
[138] https://hex-rays.com/blog/ida-9.1
[139] https://hex-rays.com/blog/ida-9.3-release-expanded-architecture-support-faster-ui-and-more
[140] https://docs.hex-rays.com/release-notes/7_5
[141] https://german-sales.com/IDA_Pro_Disassembler_9.0.pdf
[142] https://docs.binary.ninja/dev/cookbook.html
[143] https://hex-rays.com/blog/igors-tip-of-the-week-40-decompiler-basics
[144] https://docs.hex-rays.com/8.5/user-guide/decompiler
[145] https://docs.hex-rays.com/release-notes/9_4
[146] https://hex-rays.co.il/hex-rays-decompiler/
[147] https://hex-rays.com/blog/unveiling-ida-pro-9-0-the-new-nanomips-disassembler-and-decompiler
[148] https://en.wikipedia.org/wiki/Interactive_Disassembler
[149] https://api.binary.ninja/_modules/binaryninja/highlevelil.html
[150] https://github.com/avast/retdec/releases
[151] https://docs.angr.io/en/latest/analyses/decompiler.html
[152] https://github.com/avast/retdec
[153] https://docs.angr.io/en/latest/_modules/angr/analyses/decompiler/structured_codegen/c.html
[154] https://github.com/avast/retdec/wiki/Decompiler-outputs
[155] https://angr.github.io/api-doc/angr.html
[156] https://github.com/avast/retdec/blob/master/README.md
[157] https://docs.angr.io/en/v9.2.104/analyses/decompiler.html
[158] https://docs.angr.io/en/v9.2.60/_modules/angr/analyses/decompiler/decompiler.html
[159] https://github.com/toryren/retargetable-decompiler
[160] https://github.com/gbraad/retargetable-decompiler
[161] https://docs.angr.io/en/v9.2.90/analyses/decompiler.html
[162] https://docs.angr.io/en/v9.2.78/_modules/angr/analyses/decompiler/structured_codegen/c.html
[163] https://www.en-na.com/tool/retdec
[164] https://docs.angr.io/en/stable/api/angr.analyses.decompiler.decompiler.html
[180] https://arxiv.org/html/2403.05286v1
[181] https://github.com/albertan017/LLM4Decompile
[182] https://aclanthology.org/2024.emnlp-main.203.pdf
[183] https://www.emergentmind.com/topics/llm4decompile
[184] https://papers.nips.cc/paper_files/paper/2025/file/079cf13ae174c31f148207d94d213bdc-Paper-Datasets_and_Benchmarks_Track.pdf
[185] https://arxiv.org/html/2502.12221v1
[186] https://hub.baai.ac.cn/paper/b288709e-a769-4766-94f7-13c5bf0d36ac
[187] https://github.com/albertan017/LLM4Decompile/blob/main/README.md
[188] https://academy.dair.ai/papers/llm4decompile
[189] https://blog.csdn.net/liferecords/article/details/136809698
[190] https://github.com/DeusMaveriX/LLM4Decompile-Decompiling-Binary-Code-
[191] https://www.marktechpost.com/2024/03/23/llm4decompile-open-source-large-language-models-for-decompilation-with-emphasis-on-code-executability-and-recompilability/
[192] https://arxiv.org/html/2509.14646v1
[193] https://news.ycombinator.com/item?id=39733275
[194] https://www.cs.purdue.edu/homes/lintan/publications/resym-ccs24.pdf
[210] https://www.ndss-symposium.org/ndss-paper/degpt-optimizing-decompiler-output-with-llm/
[211] https://www.ndss-symposium.org/wp-content/uploads/2024-401-paper.pdf
[212] https://aclanthology.org/2025.findings-acl.1194.pdf
[213] https://arxiv.org/html/2310.06530v2
[214] https://arxiv.org/html/2403.05286v1
[215] https://openreview.net/pdf?id=m4MhSqtaPC
[216] https://www.ndss-symposium.org/wp-content/uploads/2024-401-slides.pdf
[217] https://arxiv.org/html/2502.12221v1
[218] https://openreview.net/pdf/7d06c614c04644f3924ecb75b44d3244ef9ce51d.pdf
[219] https://papers.nips.cc/paper_files/paper/2025/file/079cf13ae174c31f148207d94d213bdc-Paper-Datasets_and_Benchmarks_Track.pdf
[220] https://zhangyuqun.github.io/publications/emnlp2024.pdf
[221] https://web3.arxiv.org/pdf/2510.19615
[222] https://x.com/alexjplaskett/status/1762481158560878844
[223] https://blog.csdn.net/m0_73736695/article/details/136568621
[224] https://www.diva-portal.org/smash/get/diva2:1864948/FULLTEXT01.pdf
[195] https://arxiv.org/html/2606.16162v1
[196] https://www.emergentmind.com/topics/llm4decompile
[197] https://www.emergentmind.com/papers/2505.12668
[198] https://aclanthology.org/2025.findings-acl.1194.pdf
[199] https://conf.researchr.org/details/issta-2025/issta-2025-papers/80/DecLLM-LLM-Augmented-Recompilable-Decompilation-for-Enabling-Programmatic-Use-of-Dec
[200] https://proceedings.iclr.cc/paper_files/paper/2026/file/9132583dd869acb0cafeb0ed25eb031a-Paper-Conference.pdf
[201] https://arxiv.org/html/2511.01763v2
[202] https://openreview.net/pdf/60e51859702be0dbb70455ca4bdb2bba48f9f208.pdf
[203] https://arxiv.org/html/2403.05286v3
[204] https://arxiv.org/html/2505.11340v1
[205] https://arxiv.org/abs/2505.19240
[206] https://aclanthology.org/2026.acl-long.527.pdf
[207] https://neurips.cc/media/neurips-2025/Slides/121402.pdf
[208] https://www.emergentmind.com/topics/decompile-bench-eval
[209] https://web3.arxiv.org/pdf/2403.05286v1
[240] https://rpm.org/docs/4.19.x/manual/format.html
[241] https://rpm.org/docs/4.20.x/manual/format_v4.html
[242] https://mikecoats.com/debian-packaging-first-principles-part-1-simple/
[243] https://linux.die.net/man/5/deb
[244] https://en.wikipedia.org/wiki/Deb_(file_format)
[245] http://www.fifi.org/doc/libapt-pkg-doc/dpkg-tech.html/ch2.html
[246] https://debian-handbook.info/browse/stable/packaging-system.html
[247] https://github.com/alpinelinux/apk-tools/blob/master/doc/apk-v3.5.scd
[248] https://unix.stackexchange.com/questions/30303/how-to-create-a-deb-file-manually
[249] http://archive.debian.org/debian/dists/rex/doc/FAQ/debian-faq-6.html
[250] https://whatap.io/en/blog/deb-file-structure-in-debian
[251] https://refspecs.linuxbase.org/LSB_4.1.0/LSB-Core-generic/LSB-Core-generic/pkgformat.html
[252] https://refspecs.linuxbase.org/LSB_3.1.0/LSB-Core-generic/LSB-Core-generic/pkgformat.html
[253] https://refspecs.linuxbase.org/LSB_4.0.0/LSB-Core-generic/LSB-Core-generic/pkgformat.html
[254] https://wiki.debian.org/SimplePackagingTutorial
[255] https://packaging.python.org/specifications/binary-distribution-format/
[256] https://peps.python.org/pep-0427/
[257] https://packaging.python.org/en/latest/specifications/binary-distribution-format/
[258] https://docs.oracle.com/en/java/javase/22/docs/specs/jar/jar.html
[259] https://docs.oracle.com/en/java/javase/17/docs/specs/jar/jar.html
[260] https://packaging.pythonlang.cn/en/latest/specifications/binary-distribution-format/
[261] https://deepwiki.com/pypa/wheel/2-wheel-distribution-system
[262] https://docs.oracle.com/javase/tutorial/deployment/jar/defman.html
[263] https://docs.oracle.com/javase/8/docs/technotes/guides/jar/jar.html
[264] https://docs.oracle.com/en/java/javase/11/docs/specs/jar/jar.html
[265] https://docs.oracle.com/en/java/javase/23/docs/specs/jar/jar.html
[266] https://peps.pythonlang.cn/pep-0427/
[267] http://www.cse.yorku.ca/tech/other/jdk1.2.1/docs/guide/jar/manifest.html
[268] https://packaging.python.org/en/latest/discussions/package-formats/
[269] https://www.cs.cmu.edu/afs/cs/academic/class/15612-s98/projects/NextGen/BDK/doc/jar.html
[225] https://oci-playground.github.io/specs-latest/specs/image/v1.1.0-rc3/oci-image-spec.pdf
[226] https://oci-playground.github.io/specs-latest/specs/image/v1.1.0-rc2/oci-image-spec.pdf
[227] https://oci-playground.github.io/specs-latest/specs/image/v1.0.2/oci-image-spec.pdf
[228] https://github.com/opencontainers/image-spec/blob/master/manifest.md
[229] https://oci-playground.github.io/specs-latest/specs/image/v1.1.0-rc4/oci-image-spec.html
[230] https://djdongjin.github.io/oci-spec
[231] https://github.com/opencontainers/image-spec/blob/master/layer.md
[232] https://labs.iximiuz.com/challenges/inspect-oci-image-layout
[233] https://github.com/opencontainers/image-spec/blob/master/image-layout.md
[234] https://www.kenmuse.com/blog/building-oci-images-without-using-docker/
[235] https://oneuptime.com/blog/post/2026-02-08-how-to-understand-oci-image-and-runtime-specifications/view
[236] https://github.com/opencontainers/image-spec/blob/main/image-layout.md
[237] https://blog.quarkslab.com/digging-into-the-oci-image-specification.html
[238] https://github.com/opencontainers/image-spec/blob/v1.0.1/manifest.md
[239] https://www.alibabacloud.com/blog/594397
[270] https://oss.anchore.com/docs/reference/syft/cli/
[271] https://github.com/anchore/syft
[272] https://oss.anchore.com/docs/guides/sbom/
[273] https://oss.anchore.com/docs/architecture/syft/
[274] https://pkg.go.dev/github.com/anchore/syft/syft
[275] https://github.com/anchore/syft/wiki/package-cataloger-selection
[276] https://oss.anchore.com/docs/guides/sbom/catalogers/
[277] https://github.com/anchore/syft/wiki
[278] https://github.com/anchore/syft/blob/main/README.md
[279] https://github.com/glenkeane-94/syft
[280] https://docs.redhat.com/en/documentation/red_hat_trusted_profile_analyzer/1/html/reference_guide/creating-an-sbom-manifest-file_ref
[281] https://images.chainguard.dev/directory/image/syft/overview
[282] https://anchore.com/syft/
[283] https://anchore.com/blog/how-syft-scans-software-to-generate-sboms/
[284] https://anchore.com/blog/syft-reaches-v1-0/
[285] https://github.com/tern-tools/tern
[286] https://github.com/tern-tools/tern/blob/main/README.md
[287] https://github.com/tern-tools/tern/blob/main/docs/architecture.md
[288] https://github.com/pombredanne/tern-1
[289] https://github.com/tern-tools/tern/blob/main/setup.cfg
[290] https://blogs.vmware.com/opensource/2018/03/13/tern-software-makes-containers-compliant/
[291] https://github.com/tern-tools?language=python
[292] https://blogs.vmware.com/opensource/2018/12/06/tern-joins-act-linux-foundation-initiative/
[293] https://blogs.vmware.com/opensource/2018/12/13/overcoming-containers-compliance-challenge/
[294] https://github.com/tern-tools/tern/wiki/Home/95f7900060eac274c91eeabf6be66b204c196b46
[295] https://blogs.vmware.com/opensource/2018/04/10/tern-makes-compliance-chores-interesting/
[296] https://www.enisa.europa.eu/sites/default/files/2025-12/SBOM%20Analysis%20-%20Towards%20an%20Implementation%20Guide_v1.20-Published.pdf
[297] https://blogs.vmware.com/opensource/2021/05/25/a-tern-update-steady-progress-on-helping-secure-the-software-supply-chain/
[298] https://safeguard.sh/resources/blog/tern-sbom-generation-walkthrough-2026
[299] https://www.slideshare.net/slideshow/container-bom-inspection-with-tern/146309177
[300] https://github.com/wagoodman/dive
[301] https://github.com/wagoodman/dive/blob/main/README.md
[302] https://github.com/steven-halaka/dive
[303] https://github.com/Maddog2050/dive
[304] https://github.com/darktohka/dive
[305] https://www.x-cmd.com/pkg/dive/
[306] https://wikitwist.com/dive-into-docker-images-explore-layers-shrink-your-image-size-with-dive/
[307] https://docs.wakemeops.com/packages/dive/
[308] https://github.com/wagoodman/dive/blob/main/dive/image/docker/image_archive.go
[309] https://tompilooo.medium.com/deep-dive-in-docker-image-8591671ec03e
[310] https://hub.docker.com/r/wagoodman/dive
[311] https://github.com/marketplace/actions/dive-action
[312] https://github.com/cucker0/docker/blob/main/md/docker.md
[313] https://deepwiki.com/wagoodman/dive
[314] https://gochronicles.com/dive/
[315] https://trivy.dev/docs/latest/supply-chain/sbom/
[316] https://trivy.dev/docs/dev/scanner/vulnerability/
[317] https://scancode-toolkit.readthedocs.io/en/stable/getting-started/home.html
[318] https://trivy.dev/docs/latest/guide/target/sbom/
[319] https://scancode-toolkit.readthedocs.io/en/latest/getting-started/home.html
[320] https://trivy.dev/docs/latest/target/sbom/
[321] https://scancode-toolkit.readthedocs.io/en/latest/reference/scancode-cli/index.html
[322] https://trivy.dev/docs/dev/docs/target/sbom/
[323] http://trivy.dev/docs/dev/guide/target/sbom/
[324] https://trivy.dev/docs/latest/target/filesystem/
[325] https://trivy.dev/docs/dev/supply-chain/attestation/rekor/
[326] https://scancode-toolkit.readthedocs.io/en/latest/tutorials/configuring-scan-detection.html
[327] https://trivy.dev/docs/v0.51/supply-chain/sbom/
[328] https://trivy.dev/docs/v0.58/guide/scanner/vulnerability/
[329] https://github.com/aquasecurity/trivy/discussions/6457
[330] https://github.com/refirmlabs/binwalk
[331] https://github-wiki-see.page/m/ReFirmLabs/binwalk/wiki/Supported-Signatures
[332] https://github.com/ReFirmLabs/binwalk/pull/533
[333] https://github.com/BeeHiveSafety/KnowledgeBase/blob/web/tools/reverse-engineering/binwalk.md
[334] https://manpages.opensuse.org/Tumbleweed/binwalk/binwalk.1.en.html
[335] https://nicholasr512.github.io/Linux_and_Kali_Linux_Guide/Kali%20Tools/10%20-%20Digital%20Forensics/Binwalk/
[336] https://packages.gentoo.org/packages/app-misc/binwalk
[337] https://manpages.ubuntu.com/manpages/bionic/man1/binwalk.1.html
[338] https://manpages.ubuntu.com/manpages/focal/man1/binwalk.1.html
[339] https://sergioprado.blog/reverse-engineering-router-firmware-with-binwalk/
[340] https://altbox.dev/tool/binwalk/
[341] https://www.hardbreak.wiki/hardware-hacking/basics/tools/software-tools/binwalk
[342] https://hackerhub.me/tools/binwalk
[343] https://www.scribd.com/document/805812505/2-forensics-23-binwalk
[344] https://github.com/ReFirmLabs/binwalk/issues/605
[345] https://arxiv.org/html/2401.11161v2
[346] https://repositorio.ufscar.br/items/2c443de3-65a2-4e59-a97e-28b9649efb3b
[347] https://www.politesi.polimi.it/handle/10589/219657
[348] https://webdiis.unizar.es/~ricardo/files/papers/CarrilloR-FGCS-25.pdf
[349] http://arxiv.org/abs/1703.00298
[350] https://objects.lib.uidaho.edu/etd/pdf/Shrivastava_idaho_0089N_12624.pdf
[351] https://conf.researchr.org/details/fse-2026/fse-2026-research-papers/2/Understanding-the-Limitations-of-C-C-Binary-Third-Party-Library-Detection-Tool-An-
[352] https://inria.hal.science/hal-01648996v1/document
[353] https://www.politesi.polimi.it/retrieve/baa2d7d9-d7b9-44e7-ab79-01036632bd42/2024_04_Rizzi_Thesis_01.pdf
[354] https://www.enyo.de/fw/security/zlib-fingerprint/
[355] http://github.com/Keysight/Library-Identification
[356] https://github.com/maroueneboubakri/lscan
[357] https://www.netrise.io/blog/why-scanners-miss-vulnerabilities
[358] https://wpcdn.web.wsu.edu/wp-vcea/uploads/sites/3267/2021/11/CyserSeminarOctober25_2021.pdf
[359] https://ftp.cs.wisc.edu/pub/paradyn/papers/Harris05WBIA.pdf
[375] https://sourceware.org/libabigail/manual/abidiff.html
[376] https://sourceware.org/libabigail/manual/abi-changes.html
[377] https://sourceware.org/libabigail/manual/abipkgdiff.html
[378] https://manpages.debian.org/testing/abigail-doc/libabigail.7.en.html
[379] https://www.devdoc.net/linux/man7.org-20170728/man1/not.1.html
[380] https://sourceware.org/libabigail/apidoc/namespaceabigail_1_1tools__utils.html
[381] https://rseng.github.io/rseng/software/libabigail
[382] https://sourceware.org/pipermail/libabigail/2022q4/004911.html
[383] https://fedoraproject.org/wiki/How_to_check_for_ABI_changes_with_abidiff
[384] https://sourceware.org/libabigail/apidoc/abg-dwarf-reader_8cc_source.html
[385] https://developers.redhat.com/articles/2022/01/06/whats-new-libabigail-20
[386] https://lvc.github.io/abi-compliance-checker/
[387] https://github.com/openSUSE/abi-compliance-checker
[388] https://github.com/lvc/abi-dumper
[389] https://fedoraproject.org/wiki/How_to_check_for_ABI_changes_with_abi_compliance_checker
[390] https://stackoverflow.com/questions/33775838/check-if-shared-libraries-are-compatible-with-a-binary
[391] https://android.googlesource.com/platform/development/+/master/vndk/tools/header-checker/README.md
[392] https://euroquis.nl/kde/2021/03/03/abi.html
[393] https://blog.csdn.net/canpool/article/details/118380060
[394] https://github.com/lvc/abi-compliance-checker/releases
[395] https://www.onworks.net/ja/programs/abi-compliance-checker-online
[396] https://screenshots.debian.net/package/abi-compliance-checker
[397] https://android.googlesource.com/platform/external/abi-compliance-checker/+/62cb45aec7075e026f4f3fcdc5191ea982b414b7/abi-compliance-checker.pl
[398] https://blog.csdn.net/canpool/article/details/118651115
[399] https://stackoverflow.com/questions/39866223/what-is-the-reason-for-error-while-using-abi-compliance-checker
[400] https://archlinux.org/packages/extra/any/abi-compliance-checker/
[360] https://manpages.ubuntu.com/manpages/focal/man1/dpkg-gensymbols.1.html
[361] https://manpages.ubuntu.com/manpages/trusty/en/man1/dpkg-gensymbols.1.html
[362] https://wiki.debian.org/UsingSymbolsFiles
[363] https://manpages.ubuntu.com/manpages/xenial/man1/dpkg-gensymbols.1.html
[364] https://www.debian.org/doc/manuals/maint-guide/advanced.en.html
[365] https://manpages.debian.org/jessie/dpkg-dev/dpkg-gensymbols.1.ja.html
[366] https://man7.org/linux/man-pages/man5/deb-symbols.5.html
[367] https://www.debian.org/doc/manuals/maint-guide/advanced.zh-tw.html
[368] https://www.debian.org/doc/manuals/maint-guide/advanced.ca.html
[369] https://manpages.debian.org/jessie/dpkg-dev/dpkg-gensymbols.1.pl.html
[370] https://wiki.ubuntu.com/stefanlsd/dpkg-gensymbols
[371] https://manpages.debian.org/wheezy/dpkg-dev/dpkg-gensymbols.1.ja.html
[372] https://qt-kde-team.pages.debian.net/symbolfiles.html
[373] https://manpages.ubuntu.com/manpages/bionic//en/man1/dpkg-gensymbols.1.html
[374] https://github.com/mwhudson/dpkg/blob/master/scripts/dpkg-gensymbols.pl
[401] https://manpages.debian.org/bookworm/dwz/dwz.1.en.html
[402] https://man7.org/linux/man-pages/man1/dh_dwz.1.html
[403] https://sourceware.org/dwz/
[404] https://manpages.debian.org/testing/dwz/dwz.1.en.html
[405] https://docs.undo.io/DebuggingInformation.html
[406] https://docs.redhat.com/en/documentation/red_hat_developer_toolset/10/html/user_guide/chap-dwz
[407] https://gcc.gnu.org/legacy-ml/gcc-patches/2018-12/msg00639.html
[408] https://sourceware.org/pipermail/gdb-patches/2021-February/176508.html
[409] https://gcc.gnu.org/legacy-ml/gcc-patches/2018-11/msg01091.html
[410] https://sourceware.org/pipermail/gdb-patches/2020-November/173307.html
[411] https://interrupt.memfault.com/blog/dealing-with-large-symbol-files
[412] https://deepwiki.com/sailfishos-mirror/elfutils/3.5-split-dwarf-alternate-debug-links-and-dwp-packages
[413] https://fedoraproject.org/wiki/Features/DwarfCompressor
[414] https://sourceware.org/pipermail/gdb-patches/2020-November/173406.html
[415] https://bugzilla.redhat.com/show_bug.cgi?id=971411
[431] https://wiki.alpinelinux.org/wiki/Apk_spec
[432] https://stackoverflow.com/questions/59684341/alpine-linux-create-a-apk-package
[433] https://man.archlinux.org/man/apk-v2.5.en
[434] https://wiki.alpinelinux.org/w/index.php?title=Apkindex_format
[435] https://www.hydrogen18.com/blog/apk-the-strangest-format.html
[436] https://github.com/kaniini/alpine-policy/blob/master/source/ch-binary.rst
[437] https://stackoverflow.com/a/78829048
[438] https://docs.alpinelinux.org/user-handbook/0.1a/Working/apk.html
[439] https://stackoverflow.com/questions/38837679/alpine-apk-package-repositories-how-are-the-checksums-calculated
[440] https://unix.stackexchange.com/questions/717275/identify-apk-packages-based-on-pkginfo-depend-entry
[441] https://github.com/jordansissel/fpm/issues/2131
[442] https://www.pantsbuild.org/dev/reference/targets/nfpm_apk_package
[443] https://gist.github.com/stokito/4901a4b76834a8d1ec33f70d70785e67
[444] https://wiki.alpinelinux.org/wiki/Apk
[445] https://github.com/jirutka/alpkit
[416] https://developer.android.com/tools/apkanalyzer
[417] https://file-extensions.com/docs/apk
[418] https://developer.android.com/studio/debug/apk-analyzer
[419] https://cleanor.app/reference/apk-internals
[420] https://www.nutrient.io/blog/inspecting-apk-files/
[421] https://en.wikipedia.org/wiki/Apk_(file_format)
[422] https://play.google.com/store/apps/details?id=com.app.apkanalyzer
[423] https://developer.android.google.cn/tools/apkanalyzer?hl=ar
[424] https://www.decompiler.com/apk
[425] https://www.emergentmind.com/topics/android-application-packages-apks
[426] https://medium.com/androiddevelopers/smallerapk-part-1-anatomy-of-an-apk-da83c25e7003
[427] https://medium.com/androiddevelopers/making-the-most-of-the-apk-analyzer-c066cb871ea2
[428] https://www.oreateai.com/blog/introduction-to-android-apk-installation-files/63bd68e5ac5bd1f3c2a41b9a796e9f00
[429] https://developer.android.com.acc.wang/studio/debug/apk-analyzer?authuser=1&hl=ja
[430] https://mas.owasp.org/MASTG/techniques/android/MASTG-TECH-0007/
[446] https://www.ghidradocs.com/
[447] https://www.ghidradocs.com/12.0_PUBLIC/docs/ChangeHistory.html
[448] https://www.ghidradocs.com/11.4.2_PUBLIC/docs/ChangeHistory.html
[449] https://www.ghidradocs.com/12.0.3_PUBLIC/docs/ChangeHistory.html
[450] https://www.ghidradocs.com/11.4.3_PUBLIC/docs/ChangeHistory.html
[451] https://ghidradocs.com/11.1_PUBLIC/docs/ChangeHistory.html
[452] https://ghidradocs.com/11.0_PUBLIC/docs/GhidraClass/Intermediate/VersionTracking.html
[453] https://ghidradocs.com/11.4.2_PUBLIC/help/Base/help/topics/VersionControl/project_repository.htm
[454] https://ghidradocs.com/12.1.2_PUBLIC/docs/ChangeHistory.html
[455] https://www.ghidradocs.com/11.3.2_PUBLIC/docs/ChangeHistory.html
[456] https://www.ghidradocs.com/11.0.2_PUBLIC/help/VersionTracking/help/topics/VersionTrackingPlugin/Version_Tracking_Intro.html
[457] https://www.ghidradocs.com/11.1.2_PUBLIC/docs/WhatsNew.html
[458] https://www.ghidradocs.com/11.4.2_PUBLIC/Ghidra/Features/PyGhidra/pypkg/README.html
[459] https://ghidradocs.com/11.4.1_PUBLIC/GettingStarted.html
[460] https://www.ghidradocs.com/10.1_PUBLIC/help/VersionTracking/help/topics/VersionTrackingPlugin/Version_Tracking_Intro.html
[461] https://docs.binary.ninja/dev/bnil-hlil.html
[462] https://docs.binary.ninja/guide/index.html
[463] https://docs.binary.ninja/dev/cookbook.html
[464] https://docs.binary.ninja/dev/bnil-modifying.html
[465] https://docs.binary.ninja/dev/outlining.html
[466] https://docs.binary.ninja/getting-started.html
[467] https://docs.binary.ninja/dev/bnil-overview.html
[468] https://docs.binary.ninja/guide/migration/ghidra/index.html
[469] https://api.binary.ninja/_modules/binaryninja/highlevelil.html
[470] https://docs.binary.ninja/guide/migration/migrationguideghidra.html
[471] https://docs.binary.ninja/dev/uidf.html
[472] https://docs.binary.ninja/index.html
[473] https://binary.ninja/2024/07/15/debug-visualizations.html
[474] https://api.binary.ninja/binaryninja.highlevelil-module.html
[475] https://docs.binary.ninja/guide/types/type.html
[476] https://learn.microsoft.com/en-us/windows/win32/api/dbghelp/ns-dbghelp-symsrv_index_info
[477] https://learn.microsoft.com/en-us/windows/win32/api/dbghelp/ns-dbghelp-symsrv_index_infow
[478] https://learn.microsoft.com/zh-tw/previous-versions/ff797950(v=vs.85)
[479] https://learn.microsoft.com/en-us/visualstudio/debugger/specify-symbol-dot-pdb-and-source-files-in-the-visual-studio-debugger?view=visualstudio
[480] https://learn.microsoft.com/ja-jp/dotnet/api/microsoft.visualstudio.debugger.symbols.dkmpdbfileid.create?view=visualstudiosdk-2022
[481] https://stacksandbreaks.com/forcing-symbols-to-match-in-windbg/
[482] https://blog.s-schoener.com/2025-01-27-dbghelp-symbols/
[483] https://learn.microsoft.com/en-us/windows/win32/dxtecharts/debugging-with-symbols
[484] https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/symbols
[485] https://stackoverflow.com/questions/40704720/loading-pdb-symbols-from-symbol-servers-correct-path
[486] https://learn.microsoft.com/en-us/windows/win32/api/dbghelp/nf-dbghelp-symfindfileinpath?redirectedfrom=MSDN
[487] https://gist.github.com/0blu/ee3663ed65e8a279873b33c9448376dc
[488] https://learn.microsoft.com/en-us/visualstudio/debugger/debug-interface-access/idiadatasourceex-validatepdb?view=visualstudio
[489] https://learn.microsoft.com/sr-cyrl-rs/visualstudio/debugger/debug-interface-access/idiadatasource-loadandvalidatedatafrompdb?view=vs-2019
[490] https://learn.microsoft.com/he-il/previous-versions/visualstudio/visual-studio-2012/f0756hat(v=vs.110)
