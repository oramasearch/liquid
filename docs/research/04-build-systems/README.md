# Build systems

> Evidence: [`sources.md`](sources.md)

## The one idea that matters

Build performance is not primarily a compiler-speed problem. It is a
**dependency-accuracy problem and a data-movement problem**. Everything below
follows from that.

If dependencies are over-declared, you rebuild things that did not change. If they
are under-declared, you get stale artifacts and the culture responds with
`make clean` — which converts every build into a full build and destroys
incrementality permanently. Most "slow builds" are one of these two failures, not
a slow compiler.

## The taxonomy (Build Systems à la Carte)

Mokhov, Mitchell and Peyton Jones give the vocabulary liquid should be designed
in. Two axes:

**Scheduler** — how the build decides what to run: topological (Make: static
order), restarting (Excel: run, abort on missing dep, retry), or suspending
(Shake, Bazel: run, block on a dependency, resume).

**Rebuilder** — how it decides *whether* to run:

| Rebuilder | Mechanism | Property |
|---|---|---|
| Dirty bit | mtime | Fast, wrong under clock skew, checkout, and touch. |
| Verifying traces | Hash of inputs recorded from last run | Gives **early cutoff**. |
| Constructive traces | Hash of inputs → stored *outputs* | Gives early cutoff *and* a shared cache. |
| Deep constructive | Transitive input hash → outputs | Enables cloud builds; loses early cutoff. |

Two properties are worth more than everything else:

- **Early cutoff.** If a source change does not change a compiled object, stop —
  do not relink, do not re-run downstream. Comment-only edits should cost nothing.
  Make cannot do this at all; Bazel and Buck2 can.
- **Dynamic dependencies.** A C++ file's real dependencies (its headers) are only
  known after scanning it. A build system that requires dependencies to be
  declared statically and completely either forces over-declaration or is wrong.

## Bazel

**What's genuinely good.** The action graph is the right abstraction: an action is
a pure function from a Merkle tree of inputs to a set of outputs, identified by a
digest. That single decision buys hermeticity, remote caching, remote execution
and reproducibility simultaneously. Skyframe gives incremental, parallel,
suspending evaluation. Correctness is real: cache hits are trustworthy in a way
Make's never are.

**What people hate, consistently:**

1. **The analysis phase is a bottleneck.** Before any compilation, Bazel loads and
   analyses the entire relevant graph. On large repos this is tens of seconds to
   minutes of wall time before work starts, and it is largely single-threaded.
2. **`rules_*` churn and the WORKSPACE → Bzlmod migration.** Years of breaking
   changes; every upgrade is a project.
3. **Toolchain configuration is brutal.** Getting a hermetic C++ toolchain right
   is a specialist skill.
4. **Poor IDE integration.** The compile database is a second-class output.
5. **Hermeticity escapes.** Non-hermetic actions work locally and fail remotely —
   or worse, succeed remotely with *different output*, poisoning the shared cache.
6. **It is all-or-nothing.** You cannot adopt Bazel for 5% of a repo.

## Buck2

The Rust rewrite with the important architectural correction: **dynamic
dependencies as a first-class concept**, on top of a general incremental
computation engine (DICE) rather than a bespoke one. This lets the graph change
shape during the build — which is what C++ header scanning, generated code and
compiled-language module systems actually need. It also merges analysis and
execution rather than treating them as strict phases, which attacks Bazel's
biggest latency complaint. Buck2 is the strongest evidence about what a modern
build engine should look like.

## CMake, and why it is resented

CMake won cross-platform C++ by being a *generator* — it emits Ninja files, VS
solutions, Xcode projects. That is its genuine value and why it will not go away.

The complaints are specific and fair:

- **The language.** Everything is a string; lists are semicolon-joined strings;
  variable scoping is dynamic and surprising; there are no real types.
- **Directory-scoped vs target-scoped state.** The old `include_directories()`
  world and the "modern CMake" target-based world coexist, and most real projects
  are a mixture. `PUBLIC`/`PRIVATE`/`INTERFACE` is correct but poorly understood.
- **`find_package` has two incompatible modes** (Module and Config) with different
  authorship, different discovery rules, and different failure messages.
- **Generator expressions** are a second, differently-evaluated mini-language.
- **It is not a package manager.** Dependency acquisition is left to
  `FetchContent`, vcpkg, Conan, or the system — so every project solves it
  differently.
- **No build cache of its own.** You bolt on ccache.

Ninja, by contrast, is almost universally liked, precisely because it does one
thing: execute a static dependency graph fast. It is the correct back end and the
wrong front end.

Meson is the modern answer to Autotools/CMake for native code: a small,
deliberately non-Turing-complete declarative language over Ninja. Its adoption by
GNOME, systemd, GStreamer and X.org is the clearest signal of where the native
build world is going — see
[05-autotools-and-configure-era](../05-autotools-and-configure-era/).

## Why "build the world" happens

The specific complaint this project starts from. It has four distinct causes, and they need four
distinct fixes:

| Cause | Fix |
|---|---|
| **Coarse action granularity.** The unit of rebuild is a whole package/derivation, so any change rebuilds all of it. (This is Nix's mass-rebuild problem — see [09](../09-package-managers/).) | Make actions fine-grained; make the *compile* the unit, not the package. |
| **No early cutoff.** A version bump changes a hash, so everything downstream rebuilds even if the *content* is identical. | Content-address outputs, not inputs. If the rebuilt artifact is byte-identical, downstream stops. |
| **Over-declared dependencies.** A header everything includes; a toolchain in the input closure of everything. | Fine-grained, *observed* dependencies rather than declared ones. |
| **Cache misses from irrelevant variation.** Absolute paths, timestamps, env vars leaking into cache keys. | Aggressive normalisation of the build environment. See [06](../06-build-and-compiler-caching/). |

**Content-addressed outputs are the single highest-leverage fix.** If a glibc
patch produces a byte-identical `libc.so`, nothing downstream should rebuild. Nix
knows this (CA derivations, RFC 062) but has struggled to make it the default.
liquid can make it the default from the start.

## Compiler-level incrementality

The build system can only be as incremental as the compiler allows. C++ is the
hard case: the preprocessor means a translation unit's dependencies are dynamic,
and header changes fan out enormously. The relevant mechanisms:

- **`clang-scan-deps` / P1689** — a standard format for reporting a TU's module
  and header dependencies, so a build system can discover the graph before
  compiling. This is what makes C++20 modules buildable at all.
- **C++ modules** genuinely fix the fan-out problem, but the tooling in 2026 is
  still maturing — see [18-implementation-cpp26](../18-implementation-cpp26/).
- **Unity builds and PCH** are workarounds that trade incrementality for
  throughput; they make full builds faster and incremental builds worse.
- **Rust's query system** is the model for doing this *inside* the compiler —
  see [08-incremental-compilation](../08-incremental-compilation/).

## Implications for liquid

1. **Adopt the action-graph model, with Buck2's dynamic dependencies.** An action
   is a pure function from an input Merkle tree to outputs, addressed by digest.
   This is non-negotiable — it is what makes caching, distribution and provenance
   all fall out of one mechanism.

2. **Content-address outputs by default, not inputs.** This is what kills "build
   the world". A rebuild that produces identical bytes must terminate the cascade.
   It requires reproducible builds to be a hard requirement rather than an
   aspiration — see [13](../13-supply-chain-security/).

3. **Make actions as fine-grained as the language allows.** Per-TU, and where the
   language supports it, per-definition
   ([08](../08-incremental-compilation/)). Coarse actions are the root cause of
   most rebuild waste. But respect the granularity floor: an action too small to
   amortise its own scheduling and hashing overhead is a loss
   ([07](../07-distributed-execution/)).

4. **Observe dependencies; do not require them to be declared.** Sandboxed
   execution can *record* exactly which files an action read, and that record is
   the dependency set. This makes under-declaration impossible and
   over-declaration unnecessary, and it removes the single biggest source of
   Bazel's authoring burden.

5. **No separate analysis phase.** Bazel's biggest latency complaint is
   architectural. Merge analysis and execution into one incremental computation
   graph, as Buck2 does.

6. **Be adoptable incrementally.** Bazel's all-or-nothing adoption is why most
   projects never try it. liquid must be able to wrap an existing CMake or
   Autotools project and deliver caching and distribution benefits *before* the
   project rewrites anything.

7. **Ingest existing build descriptions.** `compile_commands.json` is the universal
   interchange format for native code and should be a first-class input. So should
   the ability to run `./configure && make` inside a liquid action and capture the
   result.
