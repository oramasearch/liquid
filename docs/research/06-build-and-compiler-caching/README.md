# Build and compiler caching

> Evidence: [`sources.md`](sources.md)
>
> **Requirement:** liquid must ship with a build cache *and* a compiler cache out
> of the box. Not a plugin, not a bolt-on, not "configure ccache separately."

## The correctness rule

A cache entry is correct only if its key covers **every input capable of changing
the output**. Everything else is a consequence of getting that wrong in one of two
directions:

- **Under-hashing → false hits.** The cache returns an artifact that does not match
  the inputs. This is the catastrophic failure: it is silent, it produces wrong
  binaries, and in a *shared* cache it propagates to everyone. One non-hermetic
  action on one machine can poison a fleet.
- **Over-hashing → false misses.** The cache is correct but useless. This is the
  common failure, and the reason most teams' cache hit rates are far below what
  they assume.

Almost all practical caching work is finding inputs that are in the key but should
not be (over-hashing) or are missing from it but should not be (under-hashing).

## What actually causes misses

The empirically dominant causes, from ccache/sccache/Bazel/Gradle experience:

| Cause | Why | Fix |
|---|---|---|
| **Absolute paths** | Build directory appears in `__FILE__`, debug info (`DW_AT_comp_dir`), and `-I` flags. Two developers in different home directories never share a hit. | Path normalisation. ccache's `base_dir`; `-ffile-prefix-map` / `-fdebug-prefix-map`. |
| **Timestamps** | `__DATE__`/`__TIME__`, archive member mtimes, embedded build times. | Ban them; `SOURCE_DATE_EPOCH`. |
| **Compiler identity** | Upgrading the compiler must invalidate everything — and usually does so correctly, but a *rebuilt* identical compiler should not. | Hash the compiler *content*, not its path or mtime. |
| **Environment leakage** | `PATH`, `HOME`, locale, `TZ`, `PWD`, hostname, user. | Sandbox with a fixed, minimal environment. |
| **Non-determinism in the tool** | Hash ordering, parallel-emission order, ASLR-dependent output, PGO. | Reproducible-build hygiene. See [13](../13-supply-chain-security/). |
| **Cold CI runners** | Ephemeral machines start with nothing. | A *remote* cache is mandatory; a local-only cache is nearly worthless in CI. |

## ccache: the mechanism to learn from

ccache has two modes, and the difference is instructive.

**Preprocessor mode.** Run the preprocessor, hash the fully-preprocessed output
plus the compiler and flags. Always correct, because the preprocessed text *is*
the complete input. Cost: you pay for preprocessing on every compile, which for
C++ with heavy headers is a large fraction of total compile time.

**Direct mode.** Hash the source file, the flags, and — via a stored **manifest** —
the set of headers the previous compilation actually included, with their hashes.
If all of those match, hit, and preprocessing is skipped entirely. Much faster.
The manifest is the key data structure: it is a *learned, observed* dependency set,
recorded from a previous run and validated on the next.

This is exactly the "observe dependencies rather than declare them" idea from
[04-build-systems](../04-build-systems/), implemented at the file level a decade
before Bazel made it architectural. liquid should generalise it: **every action
stores a manifest of what it actually read, and that manifest is the cache key for
the next run.**

ccache also supports secondary/remote storage (HTTP, Redis, file), which is the
bridge from a personal cache to a team cache.

## sccache and the Rust problem

sccache extends the model to Rust, CUDA and cloud object stores (S3, GCS, Redis,
Azure, memcached), and `sccache-dist` adds a scheduler plus build servers with
toolchain packaging — a lightweight distributed compile without full REAPI.

The important lesson is the **Rust conflict**: Rust's own incremental compilation
(`-Cincremental`) writes a stateful, machine-local, non-portable directory of
intermediate results. That state is fundamentally incompatible with a
content-addressed shared cache, so sccache and incremental compilation are
mutually exclusive in practice. Cargo's fingerprint model compounds it with
well-known false invalidations (feature-flag changes, path dependencies, `build.rs`
re-runs).

**The general principle:** *compiler-internal incremental state and cross-machine
caching are in tension.* A tool that wants both must own the incremental state and
make it content-addressed and portable, rather than letting each compiler keep a
private mutable directory. This is a direct argument for liquid eventually owning
more of the compilation pipeline.

## Gradle, Turborepo, Nx: the relocatability lesson

Gradle's build cache made **relocatability** an explicit, testable property: a
cached task output must be usable from a different absolute path. Gradle also
ships cache-miss diagnostics (build scans) that tell you *why* a task missed —
which is the feature every other system lacks and every user wants.

Turborepo and Nx brought remote caching to JS monorepos with a much simpler
hashing scheme, and proved that developers will adopt caching immediately when it
requires no configuration. Their weakness is coarse granularity (per-package,
not per-file).

## Bazel's split: Action Cache vs CAS

Bazel separates two things that are often conflated:

- **CAS (Content Addressable Storage)** — blobs by hash. Dumb, shared, huge.
- **Action Cache (AC)** — maps an *action digest* to an `ActionResult` describing
  the outputs (which live in the CAS).

This split matters because it allows **Build without the Bytes**: the client can
learn that an action succeeded and what its outputs' digests are, without
downloading the outputs. Only the artifacts actually needed locally get
materialised. On large builds this is the difference between transferring hundreds
of gigabytes and transferring almost nothing — see
[07-distributed-execution](../07-distributed-execution/).

The failure mode of the split is **AC/CAS skew**: an AC entry survives while the
CAS blobs it references are evicted, producing a "hit" that cannot be
materialised. Any implementation must either validate on read or tie the
lifetimes together.

## Cache trust in a shared cache

A shared write-enabled cache is a supply-chain surface. If any contributor's
machine can write an entry that others consume, then any compromised or merely
misconfigured machine can inject a malicious binary that everyone else will use
without rebuilding. This is not hypothetical — it is the same class of problem as
the build-system compromises in
[13-supply-chain-security](../13-supply-chain-security/).

The mitigations: only trusted builders (CI) may write; everyone else is read-only.
Entries are signed by the producer. High-value or security-relevant actions are
built more than once and compared. And reproducibility becomes the verification
mechanism — if two independent builders produce the same bytes, the entry is
credible.

## Implications for liquid

1. **One cache, three tiers, zero configuration.** In-memory (hot, current
   session) → local disk (this machine) → remote/shared (team, CI, public). The
   same key, the same content-addressed store, promoted between tiers. It must
   work on first run with no flags.

2. **Cache the action, not the compiler invocation.** ccache caches compiles;
   Bazel caches actions. liquid should do both by making a compile *be* an action.
   That unifies the "build cache" and "compiler cache" the user asked for into one
   mechanism rather than two subsystems with different keys and different bugs.

3. **Manifests everywhere.** Generalise ccache's direct mode: every action records
   the set of files it actually read, with hashes. That manifest validates the next
   run and doubles as the dependency graph
   ([04](../04-build-systems/)) and the provenance record
   ([13](../13-supply-chain-security/)).

4. **Normalise aggressively by construction.** Sandbox every action with a fixed
   environment, a virtualised path root, and no clock. Do not ask users to
   configure `base_dir` and `-ffile-prefix-map` — make the paths stable by making
   the sandbox stable. Most cache misses are self-inflicted and preventable.

5. **Split AC from CAS, and implement Build without the Bytes from day one.** Do
   not materialise artifacts nobody asked for. Tie AC entry lifetime to CAS blob
   lifetime, or validate on read; never return an unmaterialisable hit.

6. **Ship cache diagnostics as a first-class command.** "Why did this miss?" must
   have an answer that names the specific input that differed. Gradle's build scan
   is the bar. Without this, users cannot fix their own hit rates and will
   conclude the cache does not work.

7. **Make writes authenticated and reads verifiable.** Signed cache entries;
   read-only by default for developer machines; CI and reproducibility as the
   trust anchor. A shared cache without this is a fleet-wide code-execution
   primitive.

8. **Own the incremental state.** The Rust/sccache conflict shows that
   compiler-private incremental directories cannot be shared. Where liquid controls
   compilation, its incremental state must be content-addressed and portable
   ([08](../08-incremental-compilation/)) so that "incremental" and "shared cache"
   are the same thing rather than alternatives.
