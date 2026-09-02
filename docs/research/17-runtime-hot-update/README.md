# Runtime hot update

> **Note on sources:** this topic has no dedicated `sources.md`. It was researched
> as part of [08-incremental-compilation](../08-incremental-compilation/), whose
> `sources.md` §4 covers hot reloading, live patching and state migration in
> detail. See also [20-deployment-runtime-k8s](../20-deployment-runtime-k8s/) for
> where an update actually lands.

## The goal

liquid's long-term ambition: an incremental build updates a *running* system
without a full restart. Change a function, and the process executing it picks up
the new version — drip by drip, all the way to production.

This is the most speculative part of the project, and it should be treated as such.
It is also the part with the clearest prior art showing both that it works and
exactly why it is hard.

## The prior art, ranked by how honestly it succeeds

**Erlang/OTP — the gold standard.** Two versions of a module coexist in the VM;
processes migrate to the new one at a known boundary; `code_change/3` explicitly
migrates state; release upgrades are scripted with appups and relups. It works
because the *language* was designed for it: processes are isolated, state is
immutable and process-local, and there is a supervised, well-defined point at which
a process can be asked to convert its state.

The lesson is uncomfortable for a general-purpose tool: **Erlang's hot upgrade works
because of language semantics, not because of clever tooling.**

**Kernel live patching** (kpatch, kGraft, Ksplice, and the merged `livepatch`
infrastructure) — proof that this is achievable for *native code* in the most
demanding environment there is. The mechanism: redirect function entry points to
new implementations, and wait for a quiescent state where no thread is executing
inside a function being replaced. The constraint: **it cannot change data structure
layouts.** Patches are limited to changes that leave state representation intact.

**Smalltalk and Lisp images** — the running image *is* the program. Superb
interactivity; but the image becomes the artifact, which is directly at odds with
reproducibility ([13](../13-supply-chain-security/)).

**JVM HotSwap / DCEVM / JRebel** — stock HotSwap can only redefine method bodies:
no adding or removing methods or fields. DCEVM and JRebel lift much of that. Good
for development; not a production update mechanism.

**.NET Hot Reload** — similar capabilities, similar limits.

**Rust and C++ hot reload** (subsecond/Dioxus, hot-lib-reloader) — reload a dynamic
library and re-resolve symbols. Works for isolated, mostly-pure logic; falls apart
as soon as state outlives the reload, which in practice is immediately.

**WebAssembly component model** — dynamic linking with explicitly typed interfaces
at the boundary. The most promising *new* substrate, precisely because the
interface between components is declared rather than implicit.

**eBPF** — a different point in the space: safe, verified code loaded into a running
kernel. Not general-purpose, but the verification model is instructive.

## The actual obstacle is state, not code

Loading new code is comparatively easy. The hard question is: **what happens to
live data whose type just changed?**

There is no generic answer. Every system that succeeds does so by picking one of
three constraints:

1. **Isolate and migrate explicitly.** Erlang. State is process-local, and the
   developer writes the migration function. Works, and requires language support
   plus developer effort per change.
2. **Restrict to layout-preserving changes.** Kernel live patching. The tool proves
   the change is safe and refuses otherwise. Works, and covers a limited but
   valuable class of changes — notably security fixes.
3. **Have no long-lived state.** Stateless request handlers. Works trivially, which
   is why "just roll a new pod" is the industry's answer and why it is often
   correct.

A general-purpose tool that promises option 1 for arbitrary languages is promising
something nobody has delivered.

## What liquid can realistically offer

liquid has one genuine structural advantage: **it knows exactly what changed.** Its
incremental graph ([08](../08-incremental-compilation/)) identifies the changed
definitions; its ABI extraction ([15](../15-binary-introspection/)) can determine
whether a change is layout-preserving; and its provenance chain
([13](../13-supply-chain-security/)) can prove the patch corresponds to reviewed
source.

That combination supports a **classified, tiered** approach rather than a blanket
promise:

| Tier | Change class | Mechanism | Confidence |
|---|---|---|---|
| **0** | No observable change (comment, formatting) | Nothing to do — early cutoff already stopped it | Certain |
| **1** | Function body changed; ABI and layout unchanged | Live-patch style entry-point redirection at a quiescent point | High — this is what kernel livepatch does in production |
| **2** | Additive interface change; existing layouts unchanged | Load new code; old callers unaffected | Good, with ABI verification |
| **3** | Data layout or interface changed | Requires a developer-supplied migration, or a restart | Honest answer: usually restart |
| **4** | Anything the analysis cannot classify | Restart | Always available |

The value is in **tiers 0–2 being automatic and provably safe**, with a clean,
unembarrassing fall back to a rolling restart otherwise. That is a genuinely useful
product. "Hot reload anything" is not.

## Implications for liquid

1. **Defer this, but do not design it out.** The artifact model must be able to
   express "this build supersedes that one at these specific symbols, and here is
   the ABI delta." If artifacts are opaque blobs, hot update is impossible later.
   Keep the symbol-level identity.

2. **Classification is the deliverable, not the patching.** The valuable, novel
   thing liquid can do is *decide, mechanically and with evidence, whether a change
   is hot-appliable.* The ABI diff already needed for packaging
   ([10](../10-os-packaging/)) and content-addressed substitution
   ([09](../09-package-managers/)) is the same analysis. One mechanism, four uses.

3. **Target tier 1 first, and target the kernel-livepatch model.** Function-body
   changes with unchanged layout cover a large fraction of real edits — and almost
   all security fixes, which is where restart-free patching is most valuable.

4. **Restart is a first-class outcome, not a failure.** Deployment integration
   ([20](../20-deployment-runtime-k8s/)) means the fallback is a fast rolling
   update from an image that shares nearly all its chunks with the previous one.
   That is already dramatically better than the status quo, and it makes the hot
   path an optimisation rather than a dependency.

5. **Prefer substrates designed for it where liquid controls the runtime.** Wasm
   components have explicit typed interfaces at the boundary, which is exactly what
   makes safe replacement decidable. If liquid ever grows its own compiler — the
   deferred question in the project premise — this is the argument for what its
   runtime model should look like.

6. **Never trade reproducibility for liveness.** The Smalltalk image trap. A
   hot-updated process must remain describable as "artifact X plus patches
   Y and Z", each content-addressed and attested, so its exact composition is
   always known and reconstructible.
