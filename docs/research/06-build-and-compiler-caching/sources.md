<!-- Raw research source. Generated via Perplexity `perplexity_research` (web-grounded,
     multi-source) on 2026-09-01 for the liquid project. Lightly reformatted only.
     Treat as evidence, not as liquid design decisions - see README.md in this
     directory for the curated analysis and the implications drawn for liquid. -->

# Build and compiler caching, 2024–2026: an engineering survey

## Executive summary

A build cache is correct only if its key represents **every input capable of changing the output**: source and generated files, transitive headers, compiler/toolchain binaries, flags, relevant environment variables, target platform, working-directory/path semantics, sandbox properties, and sometimes external-process results. Missing a real input creates a **false hit**—a correctness or supply-chain failure. Including irrelevant state creates a **false miss**—safe, but slow.

The major systems differ mainly in cache granularity and how they discover inputs:

| System | Cached unit | Input-discovery/key model | Stored value |
|---|---|---|---|
| ccache | One C-family compilation | Direct header manifest, preprocessed translation unit, or compiler dependency list | Object plus diagnostics and auxiliary outputs |
| sccache | One compiler invocation/crate | Compiler-specific parsing plus content hashes; C/C++ generally preprocesses | Compiler outputs |
| Cargo | A Rust compilation unit | Saved fingerprint plus filesystem freshness checks | Existing target-directory artifacts; not normally a portable shared result |
| Gradle Build Cache | One cacheable task | Declared task implementation, inputs and output-property identities | Task output archive |
| Gradle Configuration Cache | Configured build/task graph | Build logic and configuration-time environment | Serialized configuration state |
| Maven extension | One Maven module/lifecycle result | Source files, dependencies and effective POM/plugin configuration | Module outputs |
| Turborepo / Nx | One monorepo task | Declared/default file sets, dependency graph, environment and command/runtime state | Declared outputs plus logs |
| Bazel | One action | Action metadata, command, environment, platform and Merkle input tree | `ActionResult` in AC; byte blobs in CAS |

The practical lesson is that **remote caching is a distributed trust system, not merely an optimization**. In 2025, the CREEP vulnerability demonstrated that a pull-request build with shared write credentials could populate the same key later trusted by a protected branch; encryption and content checksums do not help if the malicious bytes were validly constructed and uploaded under the expected key. [web:151][web:152]

---

# 1. ccache

## 1.1 Hashing pipeline

Current ccache uses a **160-bit BLAKE3 input hash**. Cached records additionally carry a **128-bit XXH3 checksum** for corruption detection; that checksum is integrity detection, not authentication. Internally, direct mode hashes `ccache-input-c + ccache-input-d`, while preprocessor mode hashes `ccache-input-c + ccache-input-p`. [web:425]

The common portion conceptually includes:

- compiler identity, normally compiler mtime and size;
- compiler name/type and relevant plugin identity;
- invocation options that affect code generation;
- source language;
- relevant environment and locale state;
- output-kind information;
- working directory where it can affect generated output.

### Direct mode

Direct mode first computes a **manifest key** from the common information plus:

- the raw input source file;
- compiler options, including include-path and macro options.

It then looks up a manifest under that key. A manifest logically contains:

1. one or more prior result keys;
2. the paths of headers read by each prior compilation;
3. the content hashes of those headers.

ccache rehashes the currently named headers and compares them with each manifest entry. If one set matches, the corresponding result is a direct hit—no preprocessor or compiler process is needed. If none matches, normal mode falls back to preprocessor mode. [web:372][web:373]

A manifest can hold several results because identical source text and command options can select different headers—for example, after include-path or conditional-include changes. The format is a ccache-internal serialized cache entry, not a user-facing make-style dependency file; `--inspect`/dump facilities should be used rather than treating it as a stable textual format.

### Preprocessor mode

On a direct miss, ccache ordinarily runs the compiler’s preprocessor with `-E`. Its result key hashes:

- the complete preprocessor output;
- preprocessor standard error;
- compiler options other than options such as `-I`, `-include` and `-D` whose effects should already be represented in the preprocessed bytes.

That key identifies the final cached result. The preprocessor output is also parsed to discover headers, and the resulting paths/hashes are added to the direct-mode manifest so the next build can avoid preprocessing. [web:374]

This distinction explains the performance difference:

- **Direct hit:** hash source and known headers, then read result.
- **Preprocessed hit:** start a compiler/preprocessor process, read the whole translation unit, hash it, then read result.
- **Miss:** preprocess and compile, then construct both result and manifest records.

ccache’s own benchmark shows direct warm hits as much as roughly **145×** faster than uncached compilation in a small synthetic case, versus roughly **28×** for preprocessor hits; large real projects are dominated by other I/O and linking costs, so these are not expected end-to-end speedups. [web:76]

## 1.2 Result and cache-directory format

Since ccache 4, all material for one compilation result is packed into a single **result file**, instead of the older collection of as many as seven files. A result can contain:

- object code;
- stdout and stderr;
- dependency files;
- coverage, stack-usage or other compiler-generated side outputs;
- metadata required to recreate those files. [web:397][web:402]

The local cache is hash-sharded. Operational accounting and automatic cleanup are divided across the first **16 hash buckets**, reducing lock contention. Older configurable layouts used one to eight directory levels, conventionally two; remote file/HTTP layouts now expose `flat` or `subdirs`, where `subdirs` uses 256 directories. [web:398][web:400][web:405]

Do not copy only “object-looking” files: manifests and result records are separate keyed objects, and statistics/configuration live alongside them.

## 1.3 Why ccache hits or misses

| Factor | Engineering effect | Mitigation |
|---|---|---|
| `__TIME__`, `__DATE__`, `__TIMESTAMP__` | Current values differ even when source bytes do not. ccache normally forces preprocessor mode, where the expanded value changes the key. | Prefer reproducible constants such as `SOURCE_DATE_EPOCH`; `sloppiness=time_macros` increases hits but deliberately permits stale macro values. [web:392] |
| Absolute source/include paths | Paths can appear in `__FILE__`, preprocessor line markers, diagnostics, dependency files and object metadata. Different checkout roots therefore change keys or outputs. | Use stable workspace roots, relative paths, `base_dir`, and compiler prefix-map options. |
| `-g` debug information | Compilers often embed the compilation directory and source paths. ccache therefore hashes CWD by default when generating debug data. | Use `-fdebug-prefix-map`, `-ffile-prefix-map` or `-fdebug-compilation-dir` appropriately; only then consider disabling `hash_dir`. [web:367] |
| Compiler upgrade | Different compilers may emit different object code or metadata. Default `compiler_check=mtime` hashes compiler size and mtime, so replacement misses. | Pin toolchains. `compiler_check=content` handles copied/bootstrap compilers more accurately; `none` is unsafe unless equivalence is externally guaranteed. [web:305] |
| Compiler plugins | A plugin can alter generated code. | ccache incorporates identified compiler plugins; ensure unusual plugin loading is visible in the invocation. |
| Generated/new headers | If an included file has an mtime equal to or newer than ccache invocation time, concurrent generation could race hashing. | Correct build dependencies first. Relaxing include mtime/ctime checks is a deliberate correctness trade-off. [web:306] |
| Different flags or target | Optimization, ABI, warning-as-error, architecture, sanitizer, PCH and language-mode changes alter either key or cacheability. | Normalize and pin invocation flags. |
| PCH/modules | PCH timestamps, hidden state and macro interactions are difficult to validate. | Follow ccache’s PCH-specific options; for Clang, `-fno-pch-timestamp` helps reproducibility. [web:392] |
| Cache too small | Useful records are evicted before reuse. | Size using reuse distance, not simply project output size. |
| Different compiler path or symlink behavior | Depending on compiler-check configuration, nominally equivalent toolchains can fail to share. | Install at stable paths or use content-based checks. |

### `base_dir` versus `hash_dir`

- **`base_dir`** converts matching absolute paths to paths relative to the current working directory before hashing and, where needed, before passing them to the compiler. This permits two checkouts in different parent directories to share entries, but it can alter path spelling in dependency files and `__FILE__`. [web:305]
- **`hash_dir=true`**, the default, adds CWD when debug information could depend on it. Setting it false improves sharing only if compiler prefix mapping has already made output independent of CWD. [web:309]

`base_dir` is therefore a path-normalization mechanism; `hash_dir` is a guard against debug-output collisions. They are related but not interchangeable.

## 1.4 Depend mode and inode cache

With `depend_mode=true`, ccache never runs the preprocessor separately. The key is based on direct-mode inputs plus headers reported by:

- `/showIncludes` for MSVC;
- a compiler-generated dependency file from `-MD` or `-MMD` elsewhere.

On a miss, it runs the real compiler, obtains the dependency list, hashes those files and records the result/manifest. `-MD` includes system headers, improving correctness across system-header changes at greater hashing cost. `-MMD` excludes them, so system-header upgrades may incorrectly reuse an entry unless another key component distinguishes the toolchain/sysroot. [web:408][web:418]

The **inode cache** memoizes source/header hashes by device, inode and timestamps. It reduces repeated content reads during large direct-mode builds, but assumes a local filesystem with reliable inode and timestamp semantics; ccache requires its temporary directory to be on a supported local filesystem, and Windows support has been described as experimental. [web:306] It is a hashing accelerator, not an output cache.

## 1.5 Local and secondary/remote storage

`remote_storage`—formerly `secondary_storage`—supports:

| Backend | Form and operational notes |
|---|---|
| File | `file:/...`; suitable for NFS/shared disks. Supports flat or 256-subdirectory layout. |
| HTTP | `http://` or `https://`; cache keys map to URL paths. Authentication/TLS depend on server and configured headers/credentials. |
| Redis | `redis://[[user:]password@]host[:port][/db]`; ccache does not clean Redis, so server-side TTL or `allkeys-lru`/another memory policy is required. [web:135][web:188] |

Lookup is normally **local first, then configured remotes**. `remote_only=true` disables local result storage, although local statistics can remain. Backends can be read-only, and multiple backends can be queried. [web:378][web:387]

A local L1 is usually preferable: it suppresses repeated WAN downloads and absorbs temporary remote-cache outages. Remote-only is appropriate for disk-constrained ephemeral workers or when local persistence has no expected reuse.

## 1.6 ccache eviction and diagnostics

Local automatic cleanup begins after `max_size` or `max_files` is exceeded. It updates mtimes on hits and evicts in **approximate LRU** order from a subset of the cache; `ccache --cleanup` processes the complete cache, while age-based eviction is separately available. Redis cleanup remains the Redis operator’s responsibility. [web:286]

Useful counters must be separated:

- cacheable versus uncacheable compiler calls;
- direct hits;
- preprocessed hits;
- misses;
- local versus remote hits;
- errors and forced recaches.

For unexplained misses, enable `CCACHE_DEBUG`, repeat the compilation and compare the generated `ccache-input-*` and log files. [web:286]

---

# 2. sccache

## 2.1 Language and compiler coverage

Current sccache supports compilation of assembler, C/C++, Rust, CUDA and AMD HIP, with compiler implementations including GCC, Clang, MSVC, `rustc`, NVCC, NVC++, `hipcc` and Wind River Diab. [web:212]

For C/C++, the normal key includes a BLAKE3 digest of preprocessed output plus compiler-specific state. For Rust it hashes considerably more structured state, including:

- source/file digests;
- parsed rustc arguments;
- rustc executable path;
- rustc host triple;
- sysroot path;
- shared libraries under the sysroot;
- dependency `rlib` information;
- relevant environment.

For C-family compilers it also accounts for compiler-binary hash, language, dependency/preprocessor/architecture arguments, extra input files, colour mode and environment variables. [web:214][web:428]

This is why “same compiler version string” is insufficient: two sysroots or compiler builds with the same display version may have different code-generation behavior.

## 2.2 Storage backends

sccache defaults to a local disk cache: approximately `~/.cache/sccache` on Linux, the platform-local application-data directory on Windows, and `~/Library/Caches/Mozilla.sccache` on macOS. Its documented default maximum is **10 GiB**, configurable with `SCCACHE_CACHE_SIZE`. [web:297]

Supported remote integrations include:

| Backend | Notes |
|---|---|
| Amazon S3 and compatible services | Bucket/region, custom endpoint, virtual-host mode, TLS and server-side encryption options. [web:290] |
| Google Cloud Storage | Bucket and cloud credentials/configuration. |
| Redis | Single-node or cluster endpoints. [web:292] |
| Azure Blob Storage | Existing container plus connection string; optional key prefix for isolation. [web:296] |
| Memcached | Low-latency shared key/value cache; eviction/durability are server concerns. |
| GitHub Actions cache | Useful in GitHub CI, but archive/network overhead can dominate. |
| WebDAV, Alibaba OSS, Cloudflare R2 and local disk | Availability depends on build features/version. [web:287][web:288] |

Recent sccache also supports hierarchical/multi-level caching with backfill, allowing an L1 local cache in front of remote storage. [web:287]

Unlike ccache, sccache’s shared storage backends and `sccache-dist` are separate concepts:

- a storage backend shares **already compiled results**;
- `sccache-dist` sends an actual cache miss to another machine to compile.

## 2.3 `sccache-dist`

Distributed sccache has three roles:

| Role | Function |
|---|---|
| Client | Wraps the compiler, checks cache and submits eligible misses. |
| Scheduler | Authenticates participants, tracks available build servers and assigns jobs. |
| Build server | Receives toolchain and inputs, executes the compiler in a sandbox, and returns outputs. |

The scheduler is control plane, not the compiler worker. The same `sccache-dist` executable can operate as scheduler or server. The project explicitly identifies three security axes: scheduler trust in servers, authorization of clients to submit work, and protection of traffic from observation/modification. [web:213]

### Toolchains

Before assigning a job, the client/scheduler determine whether the build server already has the required toolchain. If not, the client uploads a packaged toolchain. Automatic packaging is available for supported C/C++ toolchains; custom toolchains can be configured as archives with a mapping from the local compiler executable to its path inside the archive. [web:3][web:225]

Operational implications:

- Toolchain package identity must be content-stable.
- Packages should include runtime libraries, linker/compiler helpers and configuration files that influence output.
- Build servers need matching kernels/architectures and sandbox support; the quick-start documentation requires Bubblewrap on Linux. [web:225]
- A malicious or compromised build server can return malicious outputs unless results are independently reproduced or attested.

## 2.4 Why Rust is difficult

Rust has two different reuse mechanisms:

1. **Cargo freshness/fingerprints** decide whether to invoke `rustc`.
2. **rustc incremental compilation** reuses internal query/object state inside an incremental directory.
3. **sccache** can only help after Cargo invokes `rustc`, and expects to cache the invocation’s final portable output set.

An invocation containing `-C incremental` is not cacheable by sccache because rustc produces and mutates a directory of coupled intermediate artifacts rather than one self-contained result archive. Debug and test Cargo profiles commonly enable incremental compilation for workspace/path crates, while dependencies are more likely to be nonincremental and cacheable. [web:317][web:320]

For CI and cross-workspace reuse, use:

```bash
export RUSTC_WRAPPER=sccache
export CARGO_INCREMENTAL=0
```

or configure `build.rustc-wrapper`; for a developer repeatedly editing one crate, rustc incremental compilation can outperform remote whole-crate retrieval. The right choice depends on whether reuse is **within one checkout** or **across clean machines**.

Cargo also salts artifact names/fingerprints with compilation context. Feature sets, profile flags, target triples, Rust versions, `RUSTFLAGS`, build-script output and dependency metadata can cause Cargo either not to invoke the same command or to invoke rustc with a different sccache key. Thus a poor sccache hit rate may originate above sccache in Cargo’s unit graph.

---

# 3. Build-cache implementations above the compiler

## 3.1 Cargo fingerprints

A Cargo **Unit** is fresh only when both its saved fingerprint and expected outputs are valid. The fingerprint is stored under `.fingerprint` and recursively incorporates dependency fingerprints, so dirtiness propagates up the crate graph. Cargo additionally performs mtime checks:

1. output mtimes must not precede dependency outputs;
2. source-file mtimes must not be newer than the unit’s dep-info anchor;
3. missing source, output or fingerprint data makes the unit dirty.

The unstable checksum-freshness mode instead compares size and then a recorded checksum, avoiding dependence on timestamps. [web:60]

Known false-invalidation or inconsistent-freshness causes include:

- CI cache extraction that does not preserve mtimes;
- timestamp rounding, low-resolution filesystems, clock drift, WSL and network/encrypted filesystems;
- source files touched without content changes;
- background `cargo check`, rust-analyzer or concurrent builds sharing `target`;
- switching feature, profile, target, compiler or flag sets in the same output directory;
- build scripts with overly broad or changing `rerun-if-changed`/environment outputs;
- missing artifacts restored without their matching `.fingerprint` and dep-info records.

Cargo’s checksum-freshness project was motivated explicitly by CI, where restored mtimes often bear no relation to cached content. [web:197][web:206] Diagnose with:

```bash
CARGO_LOG=cargo::core::compiler::fingerprint=info cargo build
```

which reports the freshness reason rather than treating every rebuild as a compiler-cache failure. [web:203]

## 3.2 Gradle Build Cache

Gradle’s Build Cache reuses outputs of **individual cacheable tasks**, locally or remotely. A task key includes:

- task type and implementation/action classpaths;
- names of output properties;
- names and values of declared scalar and file inputs;
- relevant Gradle, `buildSrc`, plugin and build-script implementation state. [web:338]

A task is safely cacheable only if:

1. all output-affecting inputs are declared;
2. all produced outputs are declared;
3. the same declared inputs deterministically produce equivalent outputs;
4. outputs do not contain unnormalized absolute workspace paths;
5. inputs use correct path sensitivity and normalization;
6. the task does not depend on undeclared machine, network or clock state.

Custom task types normally opt in with `@CacheableTask`; declaring inputs alone does not prove cacheability.

Lookup normally checks the local cache and then remote cache. Developer machines should generally **read** shared entries, while trusted CI is the designated remote writer. Local entries survive `clean` and can serve different workspaces on the same machine. [web:282]

Relocatability is a frequent failure: if `/home/alice/repo` enters an input fingerprint or generated archive and `/builds/ci/repo` does not normalize to the same logical path, sharing fails. Gradle defines a relocatable task as one whose result can be reused at another location; absolute-path inputs are the usual defect. [web:137]

For misses:

```bash
./gradlew --build-cache --scan …
```

Use the scan’s task execution reason and task-input comparison; locally, `--info` and `-Dorg.gradle.caching.debug=true` expose key material. Develocity can compare matching task paths between two scans and identify changed inputs. [web:141][web:273]

Local unused build-cache entries default to approximately **seven days** retention in current Gradle documentation and can be adjusted through cache-cleanup configuration. [web:276]

## 3.3 Gradle Configuration Cache

This is not an artifact cache. It serializes:

- the selected task graph;
- each task’s configured state;
- dependency information and reachable supported object state.

A hit skips project configuration and loads the graph directly. Its validity tracks Gradle/JVM environment, init scripts, `buildSrc` and included build logic, build/settings scripts, version catalogs and lock files, files or filesystem state read during configuration, `ValueSource` results, system properties, and environment variables read during configuration. [web:332][web:334]

Configuration-cache compatibility requires build logic not to retain live `Project` objects or unsupported resolved model objects into task execution, not to perform undeclared configuration-time I/O, and to use providers/lazy values correctly. It remains a **local cache**, not a developer/CI-shared remote cache. [web:139][web:150]

Use both Gradle caches: Configuration Cache removes configuration work; Build Cache removes task execution.

## 3.4 Apache Maven Build Cache Extension

The Apache extension computes a module key using a HashTree-like structure over:

- configured/discovered source files;
- dependencies;
- effective POM;
- plugin parameters and selected plugin dependencies.

All component hashes contribute to the final key. [web:259]

On a hit it restores module outputs transparently to Maven core. It supports local storage and optional remote storage through Maven Resolver-compatible transports; remote reading and writing can be controlled separately, with remote writes normally reserved for CI. [web:32][web:35]

The difficult part is Maven plugin modeling: a plugin may read undeclared directories, environment, system properties or external services. The extension therefore exposes source globs, effective-POM exclusions, plugin dependency controls and runtime reconciliation. Over-excluding a volatile plugin property improves hit rate but can create false hits; including cosmetic/session-specific properties creates false misses. [web:256][web:262]

## 3.5 Turborepo and Nx

| Property | Turborepo | Nx |
|---|---|---|
| Default file scope | Package files, normally excluding ignored files; narrowed with `inputs` | Named/file-set inputs for project and dependency sources |
| Graph state | Hashes of upstream tasks are folded through `dependsOn` | Project-dependency files and configured dependency inputs |
| Dependencies | Lockfile/resolved external dependency state | External dependency versions and workspace configuration |
| Environment | Declared `env`, `globalEnv` and framework inference; undeclared variables are dangerous | Declared environment and runtime inputs |
| Invocation | Task definition/command | Command-line arguments and working-directory/runtime inputs |
| Platform | Must be modeled where output differs | May include OS and CPU architecture |
| Cached result | Declared output files plus logs | Output files plus terminal output |
| Remote reuse | Authenticated remote cache | Nx Cloud/Nx Replay or compatible service |

Turborepo hashes inputs into a task fingerprint and checks local and remote storage; matching machines can therefore get a hit on their first local run. [web:107] Important global/task hash inputs include file content, lockfile state, task definition, upstream task hashes, `globalDependencies`, and the values of declared environment variables. An undeclared environment variable that affects a bundle is classic under-hashing. [web:111]

Turborepo can HMAC-SHA256-sign remote artifacts using `TURBO_REMOTE_CACHE_SIGNATURE_KEY`. Downloads with invalid signatures are discarded as misses. This authenticates possession of the shared signing key, but it does not distinguish a malicious untrusted CI job that was given that same key. [web:109]

Nx hashes project/dependency files, workspace configuration, external dependency versions, runtime values, environment and command-line arguments. The cache entry stores the hash and results, not copies of source inputs. [web:120]

Nx’s direct S3, GCS, Azure and shared-filesystem cache packages were deprecated in May 2026 because their single shared read/write credential and unpartitioned key space are intrinsically vulnerable to CREEP. [web:152] This is the concrete form of “my colleague’s cache entry broke my build”: the colleague, PR or compromised runner was allowed to assert that arbitrary output corresponds to a key your trusted build accepts.

## 3.6 Bazel: action cache and CAS

Bazel remote caching is split deliberately:

| Store | Key | Value |
|---|---|---|
| Action Cache, AC | Digest of action metadata and inputs | `ActionResult`: exit/output metadata and digests of output blobs |
| Content-Addressable Store, CAS | Digest of the blob’s own bytes | File, directory/tree or other content blob |

On a lookup, Bazel computes the action key and asks the AC. An AC hit identifies output digests; Bazel then obtains required blobs from CAS. On a miss, Bazel executes the action, uploads outputs to CAS, then writes the `ActionResult` to AC. CAS deduplicates identical files across unrelated actions. [web:91][web:96]

Bazel can use:

- its in-process/local action state;
- `--disk_cache=path` for persistent local AC/CAS;
- HTTP or gRPC remote cache;
- remote execution, whose successful action results also populate the cache.

### Build without the Bytes

`--remote_download_minimal` keeps remote output metadata locally but downloads only outputs required as inputs to local actions, plus files needed for operations such as `bazel run`. `--remote_download_toplevel` downloads top-level requested outputs but avoids many intermediates. This reduces bandwidth and disk I/O, particularly when the next consumer is also remote. [web:92]

A logical AC hit is therefore not equivalent to “all result bytes were downloaded.” Measure AC hit rate, CAS hit/miss behavior, bytes transferred and download latency separately.

### Analytics

BuildBuddy provides per-invocation AC/CAS hits, misses and writes, data volume, throughput and trend graphs; action details expose inputs, arguments, environment and platform properties for comparing misses. [web:227][web:231]

EngFlow’s invocation analytics separates AC and CAS hits/misses, reports time spent checking, uploading and downloading, and prioritizes the longest-running missed actions; newer views expose a searchable action-cache lookup table and action details. [web:226][web:237]

---

# 4. Cache-key theory

## 4.1 Correctness equation

For a deterministic action:

\[
K = H(
  \text{toolchain},
  \text{command},
  \text{declared and discovered inputs},
  \text{relevant environment},
  \text{platform},
  \text{execution semantics}
)
\]

where execution semantics can include sandbox filesystem shape, working directory, locale, timezone, umask, target CPU, symlink behavior, network-derived data and kernel/tool runtime behavior.

The desired invariant is:

> If two executions can produce different accepted outputs, they must not receive the same cache key.

The reverse—different keys for outputs that happen to be equal—is inefficient but correct.

| Keying error | Result | Typical example |
|---|---|---|
| Under-hashing | False hit; stale, incompatible or malicious result accepted | Omitting compiler binary, generated config, environment variable, system header or CI workflow/toolchain |
| Over-hashing | False miss; unnecessary build/network/storage use | Including hostname, random temp path, timestamp, irrelevant environment or absolute checkout root |
| Nondeterministic output | Same valid key can produce different bytes | Timestamps, unordered archive entries, random IDs, embedded workspace paths |
| Bad output declaration | Hit restores an incomplete workspace | Task produces an undeclared side file |
| Mutable key slot | Correct entry can be replaced after publication | Plain object store permitting overwrite under an existing action key |

A sandbox does not automatically make a key correct. It helps by limiting ambient inputs and turning missing declarations into visible failures, but the sandbox image, mounted tools, platform properties and allowed network access must themselves be fixed or represented.

## 4.2 Verifying and constructive traces

The “Build Systems à la Carte” terminology is useful:

| Strategy | Stored trace | Reuse behavior | Representative model |
|---|---|---|---|
| Verifying trace | Prior dependencies and their hashes/values | If the trace still matches, retain the output already present locally | Ninja/Shake-style incremental build |
| Constructive trace | Dependency trace **and resulting value** | Matching trace can reconstruct/download output, including for another user | Bazel/cloud-build style cache |
| Deep constructive trace | Result indexed from terminal/source inputs, ignoring intermediate dependency identities | Can reuse final results even if intermediate reconstruction paths differ | Buck/Nix-like model |

A constructive trace stores the result and is therefore shareable; a deep constructive trace keys using terminal inputs rather than every intermediate node. [web:347][web:348]

“Shallow” and “deep” have two related uses:

- A **shallow build** asks for a target while treating intermediate outputs as available starting state; trace validation matters.
- A **deep build** reasons from terminal source inputs and can reconstruct the complete dependency chain.
- Deep constructive caching can maximize cross-build reuse, but only if every path from terminal inputs to result is deterministic and represented.

Dynamic dependency discovery, as in ccache’s manifests or compiler dep files, is a trace: the first execution observes what was read, while subsequent executions verify that observation. Its danger is dependency-set instability—if changed source would cause the compiler to read a new file, the preliminary key and trace must still force rediscovery.

## 4.3 Authentication is separate from hashing

A content digest answers **“are these bytes unchanged?”** It does not answer:

- who produced them;
- whether that producer was trusted;
- whether they were produced by the claimed action;
- whether an untrusted job raced a trusted job to populate the key.

The minimum shared-cache design should have:

1. TLS and authenticated clients;
2. immutable or first-writer-controlled entries;
3. separate read and write permissions;
4. no write token in untrusted PR/fork jobs;
5. namespaces or visibility rules by repository, tenant and trust level;
6. writes from protected CI only;
7. artifact signature or provenance binding **action digest → output digests → trusted workload identity**;
8. optional reproducibility sampling—rebuild and compare output digests;
9. release jobs that either use only trusted-cache partitions or bypass cache reads.

Nx’s own current guidance is to allow writes only from trusted CI branches and, for the strongest release guarantee, skip cache reads for deployable artifacts. [web:244][web:253]

---

# 5. Operational problems and recommended metrics

## 5.1 Size and eviction

| Policy | Strength | Weakness | Appropriate use |
|---|---|---|---|
| LRU | Retains recently reused entries; simple and common | A large one-time scan can evict valuable older entries | Developer/compiler caches with temporal locality |
| Approximate LRU | Lower metadata/locking cost | Not globally optimal | ccache’s sharded local cleanup |
| LFU | Retains repeatedly popular entries | Slow to adapt; old once-popular entries persist | Large shared caches with stable hot dependencies |
| Size-aware admission/eviction | Maximizes time or requests saved per byte | Needs cost and popularity data | Remote CAS with huge artifacts |
| TTL | Predictable storage horizon | Discards still-useful stable toolchain/dependency entries | Compliance or simple object stores |
| No eviction/object-store lifecycle | Operationally simple | Costs grow indefinitely; cold blobs accumulate | Temporary or externally managed CAS |

ccache uses approximate mtime-based LRU locally, while Redis can be configured independently—for example `allkeys-lru`. [web:181][web:188]

Size the cache from **reuse distance**: how many bytes are produced between creation and likely next use. A 10 GiB cache is ineffective if one full matrix build emits 20 GiB before a second runner requests the first entries.

## 5.2 Network economics

A hit is beneficial only when:

\[
T_{\text{lookup}} + T_{\text{download}} + T_{\text{decompress}} +
T_{\text{materialize}} < T_{\text{execute}}
\]

Consequently:

- caching a 50 ms task with a 20 MiB output is often a loss;
- caching a ten-minute compile that emits 5 MiB is almost always worthwhile;
- large test reports, Docker images, link outputs and generated source trees need size-aware decisions;
- local L1 caches, regional replicas and CAS deduplication reduce repeated transfer;
- Bazel’s minimal-download mode avoids materializing intermediates unnecessarily;
- compressed artifacts save bandwidth but consume CPU and delay first-byte availability.

Gradle’s own performance reporting separates local/remote hits and shows remote transfer time and rate, which is more meaningful than a hit count alone. [web:170]

## 5.3 Metrics that matter

Track at least:

| Metric | Why |
|---|---|
| Eligible/cacheable actions | Denominator; distinguishes misses from tasks that never attempted caching |
| Local, remote and total hit rates | Reveals L1 effectiveness and remote value |
| Direct versus preprocessed ccache hits | Measures avoided preprocessing |
| Hit rate weighted by uncached duration | One avoided 10-minute compile matters more than 1,000 tiny hits |
| Bytes read/written and compression ratio | Quantifies network/storage cost |
| Lookup, first-byte, download and materialization p50/p95/p99 | Identifies a “successful but slow” cache |
| AC versus CAS hits | Separates action reuse from blob availability |
| Eviction and age-at-eviction | Detects undersized cache |
| Miss reason/input delta | Makes hit-rate improvement actionable |
| Upload duplication/backfill | Detects wasted writes |
| Rejected checksum/signature entries | Security and corruption signal |
| Saved execution time minus cache overhead | Actual economic outcome |

BuildBuddy recommends AC hit rate, CAS hit rate by tier, download latency, bytes transferred, backend errors and miss reasons. [web:17]

## 5.4 Cold-start and CI

For ephemeral CI:

1. Put immutable dependency/toolchain layers in the runner image where practical.
2. Use remote action/task/compiler caches for incremental source results.
3. Let trusted main-branch CI warm the shared cache.
4. Give PRs read-only access to trusted entries, or isolate their writes.
5. Prewarm only high-cost/high-reuse entries; bulk downloading a whole cache archive often wastes time.
6. Keep toolchain paths, workspace roots, target triples and environment stable across runners.
7. Do not restore only outputs: restore matching metadata/fingerprints where a system such as Cargo requires them.

## 5.5 Monorepo sharing and colleague-induced failures

Cross-developer sharing works only when:

- developers use pinned toolchains and dependency lockfiles;
- task inputs include global configuration and environment;
- absolute paths and usernames are normalized;
- generated outputs are deterministic;
- platform-specific outputs are separated;
- entries are immutable and writer identity is trusted.

When one colleague’s hit breaks another build, classify it before flushing everything:

1. **Incomplete key:** compare command, environment, toolchain and full input lists.
2. **Incomplete result:** undeclared output or side effect was not archived.
3. **Non-relocatable output:** absolute source/workspace path was restored.
4. **Platform collision:** same key crossed OS/CPU/libc/JDK/SDK boundaries.
5. **Nondeterminism:** same key legitimately generated different bytes.
6. **Corruption:** transport/storage checksum failure.
7. **Poisoning:** unauthorized or insufficiently trusted writer populated the key.
8. **Tool bug:** cache client serialized or restored the result incorrectly.

Then invalidate the narrow entry/namespace, force a recache from trusted CI, and fix the key or trust policy. A global flush merely hides the defect until it recurs.

---

# 6. Reported hit rates and outcomes

Hit rates are not directly comparable unless their denominator, workload and warm/cold state match.

| Report/workload | Reported result | Interpretation |
|---|---:|---|
| 2024 empirical study, projects grouped by build duration | Median hit rates **8.42%** for short, **66.96%** for medium and **77.15%** for long builds | Longer builds tend to expose more reusable work, but project and metric definitions matter. [web:167] |
| QEMU CI ccache report, May 2024 | **63.67%** overall: 1,862 direct hits, 274 preprocessed hits, 1,219 misses | Maintainer noted that the effective hit rate for content still present was in the high 90s, pointing to insufficient capacity/eviction rather than bad keying. [web:172] |
| Nix/ccache troubleshooting report, 2024 | Improved from **1.71%** to **99.66%** after configuration correction | Demonstrates how path/tool invocation configuration can dominate observed rate; it is anecdotal, not a benchmark. [web:87] |
| Mozilla shared compilation-cache experiment | Same-revision build reached **99.9%** ccache hits | A warm, nearly identical build is an upper-bound scenario, not normal edit/branch behavior. [web:166] |
| Bitrise Bazel and Gradle demonstrations, 2024 | Build-time reductions of **83%** for Bazel and **72%** for Gradle | These are elapsed-time reductions, not hit rates: Bazel fell from 11:46 to 2:00 and Gradle from 29:00 to 8:05. [web:84] |
| wxWidgets CI ccache example | Warm run reported **99%** hits; total time fell from about 21.5 to 11 minutes | Also showed a 100%-miss first run could be slower than no cache, illustrating cold-start overhead. [web:175] |

The right target is therefore not “90% hits.” It is:

> maximize trusted, correct, duration-weighted reuse while keeping lookup, transfer, storage and debugging costs below the work avoided.

A team can have a 95% hit rate dominated by trivial actions and still wait on one uncached linker step; another can have 50% hits and remove most of its critical-path compilation time.

Citations:
[121] https://ccache.dev/manual/4.12.1.html
[122] https://ccache.dev/manual/latest.html
[123] https://ccache.dev/manual/4.8.2.html
[124] https://ccache.dev/manual/3.7.9.html
[125] https://ccache.dev/manual/4.8.html
[126] https://ccache.dev/manual/4.13.html
[127] https://ccache.dev/manual/4.13.1.html
[128] https://ccache.dev/manual/4.9.1.html
[129] https://ccache.dev/manual/4.12.html
[130] https://ccache.dev/manual/4.3.html
[131] https://ccache.dev/manual/4.13.6.html
[132] https://ccache.dev/releasenotes.html
[133] https://ccache.dev/documentation.html
[134] https://ccache.dev/manual/3.7.html
[135] https://ccache.dev/manual/4.13.3.html
[1] https://android.googlesource.com/toolchain/sccache/+/39d4118c3a8fc2f430b747c0262ce4fe8c1b01f9/README.md
[2] https://docs.rs/crate/sccache/0.2.6
[3] https://github.com/mozilla/sccache/blob/main/docs/Distributed.md
[4] https://deepwiki.com/mozilla/sccache/2.1-configuration
[5] https://docs.rs/crate/sccache/0.2.5
[6] https://android.googlesource.com/toolchain/sccache/
[7] https://android.googlesource.com/toolchain/sccache/+/HEAD/docs/DistributedQuickstart.md
[8] https://github.com/rust-lang/rust/issues/48412
[9] https://github.com/mozilla/sccache/issues/257
[10] https://github.com/mozilla/sccache/blob/main/docs/Rust.md
[11] https://docs.rs/crate/sccache/0.2.2
[12] https://docs.rs/crate/sccache/0.2.3
[13] https://github.com/mozilla/sccache/issues/236
[14] https://firefox-source-docs.mozilla.org/build/buildsystem/sccache-dist.html
[15] https://leapcell.io/blog/accelerating-rust-web-app-builds
[46] https://github.com/rust-lang/cargo/issues/6529
[47] https://deepwiki.com/rust-lang/cargo/5.5-incremental-compilation-and-fingerprinting
[48] https://github.com/rust-lang/cargo/issues/6296
[49] https://doc.rust-lang.org/cargo/reference/build-cache.html
[50] https://github.com/rust-lang/rust-analyzer/issues/6136
[51] https://doc.rust-lang.org/beta/nightly-rustc/cargo/core/compiler/fingerprint/index.html
[52] https://doc.rust-lang.org/nightly/nightly-rustc/cargo/core/compiler/fingerprint/index.html
[53] https://github.com/rust-lang/rust-analyzer/issues/14722
[54] https://internals.rust-lang.org/t/incremental-compilation-beta/4721
[55] https://web.mit.edu/rust-lang_v1.25/arch/amd64_ubuntu1404/share/doc/rust/html/cargo/guide/build-cache.html
[56] https://rust-lang.github.io/rust-project-goals/2024h2/user-wide-cache.html
[57] https://www.rustfaq.org/en/how-to-fix-broken-build-caches-in-cargo/
[58] https://doc.rust-lang.org/stable/cargo/reference/build-cache.html
[59] https://users.rust-lang.org/t/the-way-of-cargo-fingerprint-calculation/123085
[60] https://doc.rust-lang.org/nightly/nightly-rustc/cargo/compiler/fingerprint/index.html
[136] https://docs.gradle.org/current/userguide/build_cache.html
[137] https://docs.gradle.org/current/userguide/build_cache_debugging.html
[138] https://docs.gradle.org/current/userguide/build_cache_concepts.html
[139] https://docs.gradle.org/current/userguide/configuration_cache_requirements.html
[140] https://mvpfactory.io/blog/gradle-build-cache-deep-dive-content-addressable-storage-remote-cache/
[141] https://docs.gradle.com/enterprise/tutorials/task-inputs-comparison/
[142] https://discuss.gradle.org/t/build-cache-key-consistent-but-build-cache-always-misses/30510
[143] https://docs.gradle.org.cn/current/userguide/build_cache.html
[144] https://discuss.gradle.org/t/investigate-build-cache-miss/23361
[145] https://www.mindfulchase.com/explore/troubleshooting-tips/build-bundling/advanced-troubleshooting-for-gradle-builds-fixing-cache,-memory,-and-dependency-issues-at-scale.html
[146] https://docs.gradle.org/current/userguide/configuration_cache_enabling.html
[147] https://blog.gradle.org/introducing-gradle-build-cache
[148] https://github.com/gradle/gradle/issues/12118
[149] https://docs.gradle.org/current/userguide/configuration_cache.html
[150] https://docs.gradle.org/current/userguide/configuration_cache_status.html
[31] https://maven.apache.org/extensions/maven-build-cache-extension/remote-cache.html
[32] https://maven.apache.org/extensions/maven-build-cache-extension/
[33] https://maven.apache.org/extensions/maven-build-cache-extension/how-to.html
[34] https://maven.apache.org/extensions/maven-build-cache-extension/concepts.html
[35] https://maven.apache.org/extensions/maven-build-cache-extension/parameters.html
[36] https://maven.apache.org/components/extensions-archives/maven-build-cache-extension-1.1.0/
[37] https://underthehood.meltwater.com/blog/2024/05/21/maven-cache-extension/
[38] https://maven.apache.org/components/extensions/maven-build-cache-extension/how-to.html
[39] https://maven.apache.org/extensions/maven-build-cache-extension/usage.html
[40] https://www.mortega.dev/posts/exploring-maven-build-cache-extension/
[41] https://maven.apache.org/components/extensions-archives/maven-build-cache-extension-1.2.2/build-cache-config.html
[42] https://maven.apache.org/components/extensions-archives/maven-build-cache-extension-1.1.0/getting-started.html
[43] https://www.adesso.nl/en/news/blog/meow-moments-minimize-the-wait-for-your-build-to-be-completed.jsp
[44] https://github.com/apache/maven-build-cache-extension
[45] https://maven.apache.org/extensions/maven-build-cache-extension/performance.html
[106] https://turborepo.dev/docs/reference/configuration
[107] https://turborepo.dev/docs/crafting-your-repository/caching
[108] https://turborepo.dev/docs/core-concepts/remote-caching
[109] https://turborepo.dev/blog/turbo-1-2-0
[110] https://ducktors.github.io/turborepo-remote-cache/custom-remote-caching.html
[111] https://cachely.dev/turborepo-remote-cache
[112] https://turborepo.dev/docs/reference/system-environment-variables
[113] https://computingforgeeks.com/self-hosted-turborepo-remote-cache/
[114] https://github.com/ducktors/turborepo-remote-cache/issues/394
[115] https://github.com/vercel/turborepo/discussions/812
[116] https://turbo.hector.im/repo/docs/core-concepts/caching
[117] https://cachely.dev/docs/turborepo
[118] https://github.com/vercel/turbo/discussions/769
[119] https://nx.dev/docs/features/ci-features/remote-cache
[120] https://nx.dev/docs/concepts/how-caching-works
[91] https://bazel.build/remote/caching
[92] https://blog.bazel.build/2023/10/06/bwob-in-bazel-7.html
[93] https://www.systemshardening.com/articles/cicd/bazel-build-security/
[94] https://blog.bazel.build/2019/05/07/builds-without-bytes.html
[95] https://www.buildbuddy.io/blog/bazels-remote-caching-and-remote-execution-explained/
[96] https://blog.engflow.com/2024/05/13/the-many-caches-of-bazel/
[97] https://github.com/buchgr/bazel-remote
[98] https://github.com/bazelbuild/bazel/issues/11339
[99] https://bazel.build/versions/8.6.0/remote/caching
[100] https://bazel.build/reference/command-line-reference
[101] https://bazel.googlesource.com/bazel/+/refs/heads/staging/src/main/java/com/google/devtools/build/lib/remote/README.md
[102] https://meroton.com/blog/bazel-6-errors-build-without-the-bytes/
[103] https://www.buildbuddy.io/blog/unusual-builds-w-bytes/
[104] https://fzakaria.com/2025/01/12/bazel-knowledge-be-mindful-of-build-without-the-bytes
[105] https://deepwiki.com/bazelbuild/bazel/6.1-remote-execution-and-caching
[16] https://www.buildbuddy.io/blog/clickhouse-build-trends/
[17] https://buildbuddy-io-buildbuddy.mintlify.app/architecture-cache-read
[18] https://www.buildbuddy.io/docs/prometheus-metrics-for-cloud/
[19] https://docs.engflow.com/index.html
[20] https://docs.engflow.com/re/performance/index.html
[21] https://www.buildbuddy.io/remote-cache/
[22] https://www.buildbuddy.io/docs/enterprise-api/
[23] https://www.buildbuddy.io/blog/buildbuddy-v1-2-1-release-notes/
[24] https://blog.engflow.com/2025/11/10/enhanced-invocation-level-insights-for-bazel-builds/
[25] https://blog.engflow.com/2023/12/11/bazel-7-is-here---and-bazel-invocation-analyzer-is-ready-for-it/
[26] https://docs.engflow.com/bia/how_to_use/web-ui.html
[27] https://www.buildbuddy.io/docs/config-cache/
[28] https://buildbuddy-io-buildbuddy.mintlify.app/architecture-build-event-read
[29] https://github.com/buildbuddy-io/buildbuddy/issues/39
[30] https://www.buildbuddy.io/docs/config-all-options/
[61] https://www.microsoft.com/en-us/research/wp-content/uploads/2018/03/build-systems-final.pdf
[62] https://thewagner.net/blog/2019/06/19/build-systems-a-la-carte/
[63] https://ndmitchell.com/downloads/slides-distributed_build_systems-18_may_2018.pdf
[64] https://notes.billmill.org/programming/academic_publishing/Build_systems_%C3%A1_la_carte.html
[65] https://jade.fyi/blog/build-systems-ca-tracing/
[66] https://senior-stack.uz/Roadmap/Programming/quality-engineering/build-systems/07-build-caching/senior/
[67] https://senior-stack.uz/Roadmap/Programming/quality-engineering/build-systems/07-build-caching/interview/
[68] https://cachely.dev/blog/what-makes-a-good-build-cache
[69] https://hackage.haskell.org/package/build/docs/Build-Trace.html
[70] https://kunobi.ninja/blog/kache-v0-5-0
[71] https://www.emqx.com/zh/events/paper-reading-build-system
[72] https://blog.csdn.net/qiwoo_weekly/article/details/145151488
[73] https://senior-stack.uz/Roadmap/Programming/quality-engineering/build-systems/07-build-caching/middle/
[74] https://cachely.dev/remote-cache
[75] https://cachely.dev/improve-build-cache-hit-rate
[76] https://ccache.dev/performance.html
[77] https://glandium.org/blog/?p=3054
[78] https://cs.brown.edu/courses/csci0300/2023/notes/l13.html
[79] https://github.com/ccache/ccache/issues/1016
[80] https://stackoverflow.com/questions/72179386/under-what-conditions-can-a-remotely-cached-test-result-be-drawn-from-in-bazel
[81] https://www.sciencedirect.com/topics/computer-science/cache-hit-rate
[82] https://xuanwo.io/en-us/reports/2023-04/
[83] https://fossies.org/linux/bazel/docs/versions/6.2.0/remote/cache-remote.mdx
[84] https://bitrise.io/blog/post/bitrise-build-cache-reduces-circleci-build-times
[85] https://glandium.org/blog/?p=3079
[86] https://docs.gradle.org/current/userguide/build_cache_performance.html
[87] https://discourse.nixos.org/t/really-low-ccache-hit-rates-when-overriding-a-package/47421
[88] https://cloud.tencent.com/developer/article/2217552
[89] https://www.oreilly.com/library/view/feedback-control-for/9781449362638/ch13.html
[90] https://rustutils.com/tools/sccache/
[286] https://ccache.dev/manual/latest.html
[211] https://github.com/mozilla/sccache/blob/main/README.md
[212] https://github.com/mozilla/sccache
[213] https://github.com/mozilla/sccache/blob/main/docs/Distributed.md
[214] https://github.com/mozilla/sccache/blob/main/docs/Caching.md
[215] https://github.com/mozilla/sccache/issues/236
[216] https://buildfetch.com/docs/buildfetch-cache/sccache-remote-storage
[217] https://www.rustfaq.org/en/how-to-use-sccache-for-caching-rust-compilations/
[218] https://github.com/mozilla/sccache/blob/main/docs/Rust.md
[219] https://github.com/wasmerio/sccache
[220] https://docs.rs/crate/sccache/0.2.5/source/README.md
[221] https://docs.rs/sccache/latest/src/sccache/compiler/rust.rs.html
[222] https://packages.gentoo.org/packages/dev-util/sccache
[223] https://xuanwo.io/en-us/reports/2023-04/
[224] https://users.rust-lang.org/t/sccache-for-caching-rust-compilation/10960
[225] https://android.googlesource.com/toolchain/sccache/+/HEAD/docs/DistributedQuickstart.md
[196] https://doc.rust-lang.org/cargo/CHANGELOG.html
[197] https://github.com/rust-lang/cargo/issues/6529
[198] https://github.com/rust-lang/cargo/issues/6733
[199] https://deepwiki.com/rust-lang/cargo/5.5-incremental-compilation-and-fingerprinting
[200] https://github.com/rust-lang/cargo/issues/2261
[201] https://docs.rs/cargo/latest/src/cargo/core/compiler/fingerprint/mod.rs.html
[202] https://github.com/rust-lang/cargo/issues/12060
[203] https://rustwiki.org/en/cargo/faq.html
[204] https://internals.rust-lang.org/t/alternate-registry-package-rebuilds/13409
[205] https://users.rust-lang.org/t/cargo-install-triggering-new-compilation-after-cargo-build/54123
[206] https://github.com/rust-lang/compiler-team/issues/765
[207] https://github.com/rust-lang/cargo/issues/6972
[208] https://cloud.tencent.com/developer/ask/sof/108835639
[209] https://github.com/rust-lang/cargo/issues/6770
[210] https://github.com/rust-lang/cargo/issues/14136
[271] https://docs.gradle.org/current/userguide/build_cache.html
[272] https://docs.gradle.org/current/userguide/build_cache_debugging.html
[273] https://docs.gradle.com/enterprise/tutorials/task-inputs-comparison/
[274] https://discuss.gradle.org/t/investigate-build-cache-miss/23361
[275] https://docs.gradle.org/current/userguide/build_cache_performance.html
[276] https://docs.gradle.org/current/userguide/directory_layout.html
[277] https://bitrise.io/blog/post/new-task-inputs-visibility-feature-for-build-cache-gradle
[278] https://docs.gradle.com/enterprise/maven-build-cache/
[279] https://docs.gradle.org/current/userguide/configuration_cache.html
[280] https://docs.gradle.org/current/userguide/upgrading_version_8.html
[281] https://github.com/gradle/dpeuni-gradle-build-cache-deep-dive-unstable-inputs
[282] https://docs.gradle.com/enterprise/tutorials/caching/?wtime=4048s&wtime=1440s
[283] https://discuss.gradle.org/t/gradle-cache-growing-indefinitely/14931
[284] https://github.com/gradle/gradle/issues/24024
[285] https://docs.gradle.org.cn/current/userguide/build_cache_debugging.html
[151] https://nx.dev/blog/cve-2025-36852-critical-cache-poisoning-vulnerability-creep
[152] https://nx.dev/docs/reference/deprecated/self-hosted-cache-packages
[153] https://nx.dev/docs/reference/inputs
[154] https://www.businesswire.com/news/home/20250626857363/en/Nx-Identifies-Critical-Security-Vulnerability-in-Build-Cache-Systems-Affects-Thousands-of-Organizations-Worldwide
[155] https://advisories.gitlab.com/npm/@nx/azure-cache/CVE-2025-36852/
[156] https://nx.dev/enterprise/security
[157] https://nx.dev/docs/concepts/how-caching-works
[158] https://x.com/jeffbcross/status/2057543663727833369
[159] https://www.javascript-package-publishing.com/monorepo-architecture-orchestration/turborepo-pipeline-configuration/
[160] https://static-asset-fingerprinting.com/build-tool-framework-asset-pipeline-integration/monorepo-and-micro-frontend-hashing/stopping-hash-drift-between-turborepo-cache-hits/
[161] https://turbo.hector.im/repo/docs/core-concepts/caching
[162] https://cachely.dev/vs/nx-powerpack
[163] https://cachely.dev/turborepo-remote-cache
[164] https://21.nx.dev/docs/features/ci-features/remote-cache
[165] https://nx.dev/docs/kb/configure-inputs
[181] https://ccache.dev/manual/latest.html
[182] https://ccache.dev/manual/4.7.html
[183] https://ccache.dev/manual/4.11.2.html
[184] https://ccache.dev/manual/4.13.6.html
[185] https://ccache.dev/manual/4.13.html
[186] https://deepwiki.com/ccache/ccache/2.2-storage-architecture
[187] https://ccache.dev/releasenotes.html
[188] https://github.com/ccache/ccache/wiki/Redis-storage
[189] https://github.com/gradle/gradle/issues/12319
[190] https://ccache.dev/manual/3.7.html
[191] https://www.buildbuddy.io/blog/tags/engineering/
[192] https://github.com/bazelbuild/bazel/issues/7664
[193] https://medium.com/swlh/how-fast-does-my-internet-need-to-be-to-use-the-gradle-remote-build-cache-part-1-4acaa6f9a2fa
[194] https://github.com/mozilla/sccache/issues/629
[195] https://android.googlesource.com/platform/prebuilts/android-emulator-build/common/+/emu-master-dev/sccache/linux-x86_64/README.md
[166] https://glandium.org/blog/?p=3079
[167] https://arxiv.org/html/2405.00796v1
[168] https://github.com/ccache/ccache/discussions/1420
[169] https://dev.to/claire_nguyen/the-cost-math-behind-our-ci-cache-hit-rate-going-from-40-to-91-284d
[170] https://docs.gradle.org/current/userguide/build_cache_performance.html
[171] https://stackoverflow.com/questions/72179386/under-what-conditions-can-a-remotely-cached-test-result-be-drawn-from-in-bazel
[172] https://lists.gnu.org/archive/html/qemu-devel/2024-06/msg00215.html
[173] https://ccache.dev/performance.html
[174] https://blog.engflow.com/2025/11/10/enhanced-invocation-level-insights-for-bazel-builds/
[175] https://groups.google.com/g/wx-dev/c/49xPXcRmRBY
[176] https://www.cppdoctor.com/blog/ccache-for-c-cmake-projects
[177] https://www.hermetiq.com/blog/why-your-bazel-cache-hit-rate-is-lying
[178] https://fossies.org/linux/bazel/docs/versions/6.2.0/remote/cache-remote.mdx
[179] https://cachely.dev/improve-build-cache-hit-rate
[180] https://depot.dev/blog/faster-bazel-builds-with-remote-cache
[241] https://nx.dev/docs/reference/deprecated/self-hosted-cache-packages
[242] https://nx.dev/blog/creep-vulnerability-build-cache-security
[243] https://nx.dev/blog/cve-2025-36852-critical-cache-poisoning-vulnerability-creep
[244] https://nx.dev/enterprise/security
[245] https://nx.dev/blog/can-you-trust-your-build-cache
[246] https://x.com/jeffbcross/status/2057543663727833369
[247] https://x.com/jeffbcross/status/1943022162794942870
[248] https://github.com/nrwl/nx/commit/bd898d3220
[249] https://github.com/nrwl/nx/commit/550c7e37da
[250] https://app.daily.dev/posts/cve-2025-36852-critical-cache-poisoning-vulnerability-affects-multiple-build-systems-vgqluiaif
[251] https://www.kbytechnologies.com/devops-automation/mitigating-remote-cache-poisoning-in-ci-cd
[252] https://advisories.gitlab.com/npm/@nx/azure-cache/CVE-2025-36852/
[253] https://nx.dev/docs/kb/cache-security
[254] https://www.linkedin.com/posts/nxdevtools_is-your-build-cache-giving-every-developer-activity-7344064067747135488-wSqs
[255] https://tweag.io/blog/2020-04-09-bazel-remote-cache/
[226] https://blog.engflow.com/2025/11/10/enhanced-invocation-level-insights-for-bazel-builds/
[227] https://www.buildbuddy.io/blog/buildbuddy-v1-3-0-release-notes/
[228] https://github.com/buildbuddy-io/buildbuddy/issues/39
[229] https://blog.engflow.com/2025/12/31/engflow-2025-year-end-wrap/
[230] https://docs.engflow.com/CHANGELOG.html
[231] https://www.buildbuddy.io/blog/debugging-slow-bazel-builds/
[232] https://www.buildbuddy.io/blog/clickhouse-build-trends/
[233] https://github.com/bazelbuild/bazel/discussions/20347
[234] https://buildbuddy-io-buildbuddy.mintlify.app/architecture-cache-read
[235] https://docs.engflow.com/web_ui/action-details-page.html
[236] https://docs.engflow.com/re/performance/index.html
[237] https://blog.engflow.com/2023/12/11/bazel-7-is-here---and-bazel-invocation-analyzer-is-ready-for-it/
[238] https://blog.engflow.com/2026/08/10/from-prototype-to-production-the-engflow-build-analytics-journey/
[239] https://docs.engflow.com/re/performance/regional-replication.html
[240] https://www.buildbuddy.io/remote-cache/
[256] https://maven.apache.org/extensions/maven-build-cache-extension/how-to.html
[257] https://maven.apache.org/extensions/maven-build-cache-extension/parameters.html
[258] https://maven.apache.org/extensions/maven-build-cache-extension/build-cache-config.html
[259] https://maven.apache.org/extensions/maven-build-cache-extension/remote-cache.html
[260] https://www.mortega.dev/posts/exploring-maven-build-cache-extension/
[261] https://underthehood.meltwater.com/blog/2024/05/21/maven-cache-extension/
[262] https://maven.apache.org/extensions/maven-build-cache-extension/
[263] https://maven.apache.org/extensions/maven-build-cache-extension/usage.html
[264] https://stackoverflow.com/questions/79102882/use-custom-directory-for-maven-build-cache-extension-ideally-per-maven-run
[265] https://maven.apache.org/extensions/maven-build-cache-extension/concepts.html
[266] https://wumpz.github.io/maven/maven-build-cache/
[267] https://www.adesso.nl/en/news/blog/meow-moments-minimize-the-wait-for-your-build-to-be-completed.jsp
[268] https://github.com/apache/maven-build-cache-extension/releases
[269] https://github.com/gradle/dpeuni-maven-build-cache-deep-dive-getting-started/blob/main/README.md
[270] https://maven.apache.org/extensions-archives/maven-build-cache-extension-1.1.0/build-cache-config.html
[362] https://ccache.dev/manual/3.7.9.html
[363] https://ccache.dev/manual/4.12.1.html
[364] https://ccache.dev/manual/4.8.2.html
[365] https://ccache.dev/manual/4.13.1.html
[366] https://ccache.dev/manual/4.8.html
[367] https://ccache.dev/manual/latest.html
[368] https://ccache.dev/manual/4.13.6.html
[369] https://ccache.dev/manual/3.1.11.html
[370] https://ccache.dev/manual/3.7.html
[371] https://ccache.dev/manual/4.13.html
[372] https://ccache.dev/manual/4.13.2.html
[373] https://ccache.dev/manual/3.3.3.html
[374] https://ccache.dev/manual/4.9.1.html
[375] https://ccache.dev/manual/4.5.html
[376] https://ccache.dev/manual/4.7.5.html
[302] https://ccache.dev/manual/3.3.3.html
[303] https://ccache.dev/manual/4.12.1.html
[304] https://ccache.dev/manual/4.13.1.html
[305] https://ccache.dev/manual/latest.html
[306] https://ccache.dev/manual/4.12.2.html
[307] https://ccache.dev/manual/3.3.html
[308] https://ccache.dev/manual/4.3.html
[309] https://ccache.dev/manual/4.9.1.html
[310] https://ccache.dev/manual/4.8.2.html
[311] https://ccache.dev/releasenotes.html
[312] https://ccache.dev/manual/4.9.html
[313] https://ccache.dev/manual/4.13.3.html
[314] https://ccache.dev/manual/4.13.html
[315] https://ccache.dev/manual/3.1.11.html
[316] https://ccache.dev/manual/3.7.9.html
[287] https://github.com/mozilla/sccache
[288] https://github.com/mozilla/sccache/blob/main/README.md
[289] https://github.com/mozilla/sccache/releases
[290] https://github.com/mozilla/sccache/blob/main/docs/S3.md
[291] https://github.com/mozilla/sccache/issues/1020
[292] https://github.com/mozilla/sccache/blob/main/docs/Redis.md
[293] https://github.com/mozilla/sccache/issues/257
[294] https://github.com/mozilla/sccache/issues/1485
[295] https://github.com/mozilla/sccache/issues/236
[296] https://github.com/mozilla/sccache/blob/main/docs/Azure.md
[297] https://github.com/mozilla/sccache/blob/main/docs/Local.md
[298] https://github.com/mozilla/sccache/blob/main/docs/Rust.md
[299] https://github.com/mozilla/sccache/blob/main/docs/Caching.md
[300] https://github.com/mozilla/sccache/issues/455
[301] https://github.com/mozilla/sccache/discussions
[317] https://github.com/mozilla/sccache/issues/236
[318] https://users.rust-lang.org/t/why-doesnt-rust-incremental-compiler-store-compiled-libraries-in-central-repository/50068/11
[319] https://blog.rust-lang.org/2021/05/10/Rust-1.52.1/
[320] https://android.googlesource.com/toolchain/sccache/
[321] https://users.rust-lang.org/t/why-doesnt-rust-incremental-compiler-store-compiled-libraries-in-central-repository/50068
[322] https://github.com/mozilla/sccache
[323] https://github.com/mozilla/sccache/issues/257
[324] https://github.com/mozilla/sccache/blob/main/docs/Rust.md
[325] https://gear.rs/tips-tricks/accelerate-build.html
[326] https://github.com/clux/muslrust/blob/main/SCCACHE.md
[327] https://doc.rust-lang.org/cargo/reference/environment-variables.html
[328] https://www.reddit.com/r/rust/comments/rvqxkf/does_sccache_really_help/
[329] https://www.rustfaq.org/en/how-to-use-sccache-for-caching-rust-compilations/
[330] https://xxchan.me/blog/2023-02-17-optimize-rust-comptime-en/
[331] https://users.rust-lang.org/t/sccache-for-caching-rust-compilation/10960
[332] https://docs.gradle.org/current/userguide/configuration_cache.html
[333] https://docs.gradle.org/current/userguide/configuration_cache_requirements.html
[334] https://docs.gradle.org/9.1.0/userguide/configuration_cache.html
[335] https://docs.gradle.org/current/userguide/build_lifecycle.html
[336] https://blog.gradle.org/improvements-in-the-build-configuration-input-tracking
[337] https://deepwiki.com/gradle/gradle/10-configuration-cache
[338] https://docs.gradle.org/current/userguide/build_cache.html
[339] https://docs.gradle.org/current/userguide/build_environment.html
[340] https://medium.com/androiddevelopers/configuration-caching-deep-dive-bcb304698070
[341] https://cookbook.gradle.org/plugin-development/configuration-cache/example-config-cache-liquibase-plugin/
[342] https://blog.gradle.org/fast-inner-dev-loops-with-gradle
[343] https://coldfusion-example.blogspot.com/2026/02/speed-up-android-studio-enabling-gradle.html
[344] https://blog.gradle.org/road-to-configuration-cache
[345] https://github.com/gradle/gradle/issues/20873
[346] https://jasonatwood.io/archives/2127
[347] https://www.microsoft.com/en-us/research/wp-content/uploads/2018/03/build-systems-final.pdf
[348] https://thewagner.net/blog/2019/06/19/build-systems-a-la-carte/
[349] https://ndmitchell.com/downloads/slides-distributed_build_systems-18_may_2018.pdf
[350] https://notes.billmill.org/programming/academic_publishing/Build_systems_%C3%A1_la_carte.html
[351] https://jade.fyi/blog/build-systems-ca-tracing/
[352] https://simon.peytonjones.org/build-systems-a-la-carte-theory-and-practice/
[353] https://www.emqx.com/zh/events/paper-reading-build-system
[354] https://hackage.haskell.org/package/build
[355] https://www.semanticscholar.org/paper/Build-systems-%C3%A0-la-carte-Mokhov-Mitchell/0a8b8e3318839fde73eab4720b680b1b246dcfa4
[356] https://www.cambridge.org/core/journals/journal-of-functional-programming/article/build-systems-a-la-carte-theory-and-practice/097CE52C750E69BD16B78C318754C7A4
[357] http://neilmitchell.blogspot.com/2018/07/inside-paper-build-systems-la-carte.html
[358] https://news.ycombinator.com/item?id=35589506
[359] https://www.diva-portal.org/smash/get/diva2:1527566/FULLTEXT01.pdf
[360] https://pure.manchester.ac.uk/ws/files/56919083/main.4.pdf
[361] https://jyn.dev/what-is-a-build-system-anyway/
[393] https://ccache.dev/manual/latest.html
[394] https://ccache.dev/manual/4.5.1.html
[395] https://ccache.dev/manual/4.9.html
[396] https://ccache.dev/manual/4.11.html
[397] https://ccache.dev/releasenotes.html
[398] https://ccache.dev/manual/3.3.3.html
[399] https://ccache.dev/manual/4.13.6.html
[400] https://ccache.dev/manual/4.13.html
[401] https://ccache.dev/manual/3.7.9.html
[402] https://github.com/ccache/ccache/discussions/1455
[403] https://www.mail-archive.com/ccache@lists.samba.org/msg01506.html
[404] https://ccache.dev/manual/4.12.1.html
[405] https://ccache.dev/manual/4.3.html
[406] https://ccache.dev/manual/3.7.1.html
[407] https://ccache.dev/manual/4.8.2.html
[392] https://ccache.dev/manual/latest.html
[377] https://ccache.dev/manual/4.7.html
[378] https://ccache.dev/manual/latest.html
[379] https://ccache.dev/manual/4.12.1.html
[380] https://ccache.dev/manual/4.13.6.html
[381] https://ccache.dev/manual/4.10.html
[382] https://ccache.dev/manual/4.13.2.html
[383] https://docs.armbian.com/build-framework/extensions/ccache-remote/
[384] https://docs.armbian.com/Developer-Guide_Extension-ccache-remote/
[385] https://ccache.dev/manual/4.13.html
[386] https://github.com/ccache/ccache/discussions/1220
[387] https://ccache.dev/manual/4.13.3.html
[388] https://stackoverflow.com/questions/71068872/configure-ccache-to-only-use-secondary-storage
[389] https://ccache.dev/manual/4.8.2.html
[390] https://buildfetch.com/docs/buildfetch-cache/ccache-remote-storage
[391] https://github.com/ccache/ccache/commit/0576d5437aa1c8e853f2d4579b7f72f77a7c3775
[408] https://ccache.dev/manual/4.12.1.html
[409] https://ccache.dev/manual/latest.html
[410] https://ccache.dev/manual/4.8.2.html
[411] https://ccache.dev/manual/4.7.5.html
[412] https://ccache.dev/manual/4.13.html
[413] https://ccache.dev/manual/4.9.html
[414] https://ccache.dev/manual/4.9.1.html
[415] https://ccache.dev/manual/4.13.6.html
[416] https://ccache.dev/manual/4.13.2.html
[417] https://ccache.dev/manual/3.7.html
[418] https://ccache.dev/manual/4.13.1.html
[419] https://ccache.dev/manual/3.7.9.html
[420] https://ccache.dev/manual/4.3.html
[421] https://deepwiki.com/ccache/ccache/2.1-caching-modes
[422] https://github.com/ccache/ccache/issues/234
[423] https://ccache.dev/manual/4.9.html
[424] https://ccache.dev/manual/4.8.2.html
[425] https://ccache.dev/manual/4.13.6.html
[426] https://github.com/cyan4973/xxhash
[427] https://ccache.dev/manual/4.13.html
[428] https://github.com/mozilla/sccache/blob/main/docs/Caching.md
[429] https://ccache.dev/manual/4.7.5.html
[430] https://github.com/Cyan4973/xxHash/blob/dev/doc/xxhash_spec.md
[431] https://ccache.dev/manual/latest.html
[432] https://ccache.dev/releasenotes.html
[433] https://chromium.googlesource.com/external/github.com/Cyan4973/xxHash/+/refs/tags/v0.7.3/xxh3.h
[434] https://blog.csdn.net/gitblog_00493/article/details/143539697
[435] https://github.com/ccache/ccache/discussions/503
[436] https://www.ietf.org/archive/id/draft-josefsson-xxhash-00.html
[437] https://chromium.googlesource.com/external/github.com/Cyan4973/xxHash/+/375d401bd4a4eba07ee75d6e627546052cb5b0ec/README.md
