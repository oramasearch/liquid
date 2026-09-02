# Monorepo scale and the authoring loop

> Evidence: [`sources.md`](sources.md)
>
> This is the machinery behind "compile the minimum delta *as* we're authoring the
> software" — the drip-by-drip requirement.

## The scale techniques that work

**Meta: Sapling + EdenFS + Mononoke + Watchman.** A virtual filesystem materialises
files lazily, so checkout is O(1) rather than O(repo). Watchman answers "what
changed?" without scanning. Mononoke serves the history. This is the most complete
answer anyone has built.

**Microsoft: VFS for Git → Scalar.** VFS for Git virtualised the Windows repo;
Scalar is the successor and is now upstream in Git — it configures partial clone,
sparse-checkout, filesystem monitoring, commit-graph, background prefetch and
incremental repacking. Scalar is an *operational bundle around existing Git
features*, not a new format, which is why it shipped.

**Google: Piper + CitC.** Clients in the Cloud is a FUSE filesystem over the
central repository; a "checkout" is a metadata operation and files are fetched on
read. Blaze integrates directly.

**Git's own toolkit**, now mature: `--filter=blob:none` (blobless clone), sparse
checkout with cone mode, the **sparse index** (collapses excluded directories into
single entries, so `git status` stops being O(all files)), and the commit-graph.

The universal conclusion: **do not materialise what has not been read**, and **do
not scan what a monitor can tell you about**.

## File change detection at scale

Each platform has a different mechanism and a different failure mode:

- **Linux inotify** — per-watch, and hits `max_user_watches` limits on large trees.
  `fanotify` is the more scalable modern option.
- **macOS FSEvents** — coalesced directory-level events; you often learn *a
  directory* changed, not which file.
- **Windows `ReadDirectoryChangesW`** and the **USN journal** — the journal is
  actually the best of the three for bulk change enumeration.

**Watchman's** contribution is the abstraction plus **cookies**: to know whether a
query result reflects all changes up to now, it writes a sentinel file and waits to
observe it, which resolves the "have I seen everything yet?" race that naive
watchers get wrong.

## The compile-as-you-type loop

Language servers are already incremental compilers running on every keystroke:

- **rust-analyzer** (Salsa-based), **clangd** (driven by
  `compile_commands.json`), **gopls**, **tsserver**.
- Each maintains an **in-memory VFS overlay** so unsaved editor buffers are
  analysed without touching disk.
- Each supports **cancellation** — a new keystroke invalidates the in-flight
  analysis.
- Latency budget: roughly 100 ms for completion to feel instant, and diagnostics
  within a second or two.

**And all of this work is thrown away and redone by the batch build.** The LSP and
the compiler maintain two separate universes of parse trees, symbol tables and
caches over the same source. This is the single largest piece of duplicated effort
in modern toolchains, and it is the strongest argument for liquid's architecture —
see [08-incremental-compilation](../08-incremental-compilation/).

## Daemons and continuous build

Bazel's server, the Gradle daemon and the Buck2 daemon all exist for the same
reason: **keeping the analysis graph warm in memory is worth more than any
optimisation to cold-start.** JIT warmup, parsed build files and dependency graphs
are expensive to rebuild each invocation.

Nix notably lacks a daemon-side incremental model — `nix build` re-evaluates — which
is part of why evaluation feels slow.

On the JS side, Vite/esbuild/turbopack HMR set user expectations: sub-second
feedback from save to browser. That is now the baseline developers compare
everything against, fairly or not.

**"Build on save" vs "build on keystroke"** is a real design choice. On save is
simpler and sufficient for most; on keystroke requires cancellation, speculative
work and an in-memory overlay, and is what makes the experience feel like a
language server rather than a build system.

## Observability

Bazel's **Build Event Protocol**, execution logs and Chrome-trace profiles;
BuildBuddy and Develocity build scans. The questions users need answered: what was
the critical path? was time spent in analysis, execution or I/O? *why* did this
action miss the cache?

Cache-miss explanation is the feature people want most and almost nobody ships
([06](../06-build-and-compiler-caching/)).

## The cost of slow builds

The developer complaints are consistent and specific: 40-minute rebuilds for
one-line changes; CI feedback loops measured in tens of minutes; context-switching
away during builds and losing flow. DORA-style research ties deployment frequency
and lead time to organisational performance, and build latency is upstream of both.

## Implications for liquid

1. **The virtual filesystem is one mechanism serving four needs.** Source checkout
   ([01](../01-version-control/)), large-file materialisation
   ([02](../02-large-files-and-binary-assets/)), remote build output
   ([07](../07-distributed-execution/)) and artifact fetch
   ([12](../12-artifact-distribution/)) are all "present a tree, fetch content on
   read." Build it once, well, with a real page-fault path — it is the single
   highest-leverage component in the system.

2. **Ship a daemon from the start.** The graph must stay warm. A cold-start
   architecture cannot deliver keystroke latency, and retrofitting a daemon later
   forces a rewrite of everything that assumed process-per-invocation.

3. **One graph for the editor and the build.** liquid's incremental query engine
   ([08](../08-incremental-compilation/)) serves LSP requests and build requests
   from the same memoised state. Diagnostics while typing are just a query against a
   graph that is already warm because the build populated it — and vice versa. This
   is the architectural payoff of unifying the tools, and it is not available to
   anyone building a build system and a language server separately.

4. **In-memory overlay for unsaved buffers, with cancellation.** Editor buffers
   become low-durability inputs to the query graph
   ([08](../08-incremental-compilation/)); a keystroke cancels in-flight work.
   This is what makes "drip by drip" real rather than "build on save with a short
   debounce."

5. **Own file-change detection; do not scan.** fanotify on Linux, FSEvents on
   macOS, USN journal on Windows, with Watchman's cookie technique for
   "have I seen everything?" A scan-based implementation will not scale, and the
   platform differences are subtle enough to be worth encapsulating once.

6. **Durability tiers are what keep keystroke latency flat.** The file being typed
   is volatile; the project's dependencies are stable; the toolchain is fixed. Most
   of the graph must never be visited on a keystroke, or per-edit cost grows with
   repository size instead of with the size of the change.

7. **Observability as a first-class product surface.** Critical path, time
   attribution, and per-action cache-miss explanation. Users cannot fix what they
   cannot see, and "the cache doesn't seem to work" is the failure mode that kills
   adoption.
