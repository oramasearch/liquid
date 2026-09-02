# Build engine

> Research: [04-build-systems](../research/04-build-systems/) ·
> [08-incremental-compilation](../research/08-incremental-compilation/) ·
> [16-monorepo-scale](../research/16-monorepo-scale-and-authoring-loop/)

## The model: a memoised query graph

liquid's build is not a task DAG that gets executed. It is a **demand-driven graph
of memoised queries**, in the tradition of rustc's query system and Salsa.

You do not "run the compiler". You ask for a result — an object file, a type, a
diagnostic, a package — and the engine computes it, records what it read, and caches
the answer under the hash of its inputs.

An **action** is the coarse end of the same spectrum: a pure function from an input
Merkle tree plus a command and environment to a set of outputs, addressed by digest.
Actions and queries are the same mechanism at different granularities, so there is
one engine rather than two.

## Early cutoff

Every result carries a **fingerprint**. When a dependency re-executes but produces
the same fingerprint, dependents are *not* re-executed.

This is the mechanism that makes "compile the minimum delta" real:

- A comment-only edit changes the file hash, recompiles nothing downstream because
  the emitted object is identical.
- A version bump that produces byte-identical output stops there instead of
  cascading.

Combined with content-addressed outputs ([01](01-object-model-and-storage.md)), this
is what eliminates "build the world" — the failure mode that makes Nix rebuild an
entire package set for a glibc patch that changed nothing observable.

## Observed dependencies

Actions run in a **sandbox** that records every file read. That record *is* the
dependency set.

- Under-declaration is impossible — you cannot read a file without it being noticed.
- Over-declaration is unnecessary — nobody has to list headers defensively.
- Non-hermetic access is detected at the moment it happens, not as a mysterious
  remote-only failure later.

This replaces the entire authoring burden that makes Bazel hard to adopt. There are
no `deps = [...]` lists to maintain.

## Durability tiers

Inputs are classified by how often they change:

| Tier | Examples |
|---|---|
| **Volatile** | The file currently being typed in |
| **Low** | Other files in the project |
| **Medium** | Project dependencies |
| **High** | Toolchain, standard library, system headers |

A change to a volatile input cannot invalidate anything that depends only on
high-durability inputs, so most of the graph is never visited on a keystroke.
Without this, per-edit cost grows with repository size instead of with the size of
the change.

## Granularity

As fine as the language allows, with a floor.

- **Per translation unit** for C and C++ today.
- **Per definition** where the language and frontend permit — the Unison insight,
  applied to languages liquid does not own, using tree-sitter and compiler frontends
  to extract the units.
- **Never per package.** Package-level rebuild is the root cause of most wasted work
  in Nix, Spack and Docker.

The floor matters: an action too small to amortise its own hashing and scheduling
overhead is a loss. Very short work stays local and may be batched
([06](06-distributed-execution.md)).

## Compile as you type

Every write to disk is a **drip** ([02](02-version-control.md)), and every drip
invalidates the minimum part of the graph. That is what makes drip-by-drip
compilation literal: the unit of rebuild is the unit of authoring.

The editor and the build query the **same warm graph**.

- Unsaved editor buffers are volatile inputs via an in-memory overlay.
- A keystroke cancels in-flight analysis and re-queries.
- Diagnostics and completions are queries against state the build already populated —
  and vice versa.

There is no separate language server maintaining a parallel universe of parse trees
and caches. That duplication is the largest piece of wasted work in modern
toolchains, and removing it is the direct route to drip-by-drip builds.

Patches from the datastore ([17](17-implementation.md)) drive invalidation, so an
edit arriving from another machine updates the graph the same way a local edit does.

## The daemon

liquid runs a persistent daemon. The graph stays warm in memory; file changes arrive
from the platform watcher (fanotify, FSEvents, USN journal) rather than from
scanning. Events are treated as **hints** — the daemon persists a cursor and can
always rescan against its database after overflow or restart.

A cold-start architecture cannot deliver keystroke latency, and retrofitting a
daemon later forces a rewrite of everything that assumed process-per-invocation.

## Correctness

Incremental compilation is hard to get right, and a stale-but-plausible artifact is
worse than a slow build. rustc has shipped incremental correctness bugs repeatedly.

liquid therefore **continuously validates itself**: sampled targets are rebuilt from
clean on idle capacity and compared against the incremental result. A mismatch is a
critical error surfaced by the tool, not discovered by a confused user. The same
sampling infrastructure detects divergent remote workers
([06](06-distributed-execution.md)) and verifies reproducibility
([10](10-supply-chain.md)).

## Adopting existing projects

liquid must be useful before a project changes anything.

```
liquid wrap ./configure && make
liquid wrap cmake --build build
```

Runs an existing build inside liquid's sandbox, observes its dependencies, caches
its actions, distributes them, and produces provenance — for a project with no
liquid configuration at all. This is the adoption path, and it is the first
shippable product.

`compile_commands.json` is a first-class input.

Configure-time probing is replaced with a **cached fact base**: most `AC_CHECK_*`
answers depend only on (toolchain, sysroot, target), which is content-addressable, so
they are computed once and shared globally rather than re-probed per project. This
also fixes cross-compilation, because the fact base for a target is looked up rather
than executed.

## Observability

- Critical path analysis.
- Time attribution: analysis vs execution vs I/O vs transfer.
- **Per-action cache-miss explanation** naming the specific input that differed.

The last is the feature everyone wants and almost nobody ships. Without it users
cannot fix their own hit rates, and "the cache doesn't work" is what kills adoption.

## Open questions

- **How far definition-level granularity can go for C++** before modules are
  universally available.
- **Sandbox cost on macOS and Windows**, where the primitives are weaker than Linux
  namespaces and seccomp.
- **Whether the query engine and the action executor share one scheduler** or are
  layered.
