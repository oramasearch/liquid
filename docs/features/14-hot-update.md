# Hot update

> Research: [17-runtime-hot-update](../research/17-runtime-hot-update/)
>
> The long-term ambition: an incremental build updates a running system without a
> full restart. **This is the most speculative part of liquid, and it is scoped
> accordingly.**

**Implementation status:** liquid #32 implements the classifier for every tier, automatic tier 0,
the tier-1 `liquid-hot` runtime on macOS arm64 and Linux x86_64, signed records, restart hooks, and
daemon delivery. Tier 2 consumes #27's stored ABI descriptors and requires a compatible comparison;
without it an additive change is tier 3. See [the library design](../library/hot-update.md).

## The honest problem

Loading new code is comparatively easy. The obstacle is **state**: what happens to
live data whose type just changed?

There is no generic answer. Every system that succeeds picks one of three
constraints:

1. **Isolate and migrate explicitly** — Erlang/OTP. Works because processes are
   isolated, state is process-local, and the developer writes `code_change/3`. It is
   a *language* property, not a tooling achievement.
2. **Restrict to layout-preserving changes** — kernel live patching (kpatch,
   livepatch, Ksplice). Redirect function entry points at a quiescent state; refuse
   anything that alters data structure layout. Proven in production, in the most
   demanding environment there is.
3. **Have no long-lived state** — stateless handlers. Which is why "roll a new pod"
   is the industry's answer, and why it is usually correct.

A tool promising option 1 for arbitrary languages is promising something nobody has
delivered. liquid does not.

## What liquid can actually offer

liquid's structural advantage is that **it knows exactly what changed**: its
incremental graph identifies the changed definitions ([04](04-build-engine.md)); its
ABI extraction determines whether a change is layout-preserving
([07](07-dependency-resolution.md)); its provenance chain proves the patch
corresponds to reviewed source ([10](10-supply-chain.md)).

That supports a **classified, tiered** approach rather than a blanket promise.

| Tier | Change class | Mechanism | Confidence |
|---|---|---|---|
| **0** | No observable change — comment, formatting | Nothing to do; early cutoff already stopped it | Certain |
| **1** | Function body changed; ABI and layout unchanged | Live-patch-style entry-point redirection at a quiescent point | High — this is what kernel livepatch does in production |
| **2** | Additive interface change; existing layouts unchanged | Load new code; existing callers unaffected | Good, with ABI verification |
| **3** | Data layout or interface changed | Developer-supplied migration, or restart | Usually restart |
| **4** | Unclassifiable | Restart | Always available |

**The deliverable is the classification, not the patching.** The novel thing liquid
can do is decide, mechanically and with evidence, whether a change is safely
hot-appliable. Tiers 0–2 automatic and provably safe, with a clean fall back
otherwise, is a genuinely useful product. "Hot reload anything" is not.

Tier 1 is the first target, because function-body changes with unchanged layout cover
a large fraction of real edits — and **almost all security fixes**, which is where
restart-free patching is most valuable.

The selected mechanism is an in-process C library. Programs declare quiescence points and register
stable entry slots; liquid loads verified relocatable definition objects and redirects those slots
only at the next declared point. This is cooperative and portable across the supported targets.
External attachment and a Wasm component target remain rejected alternatives for this phase.

## Restart is a first-class outcome

Not a failure. Because deployment is integrated ([13](13-deployment-runtime.md)), the
fallback is a fast rolling update from an image sharing nearly all its chunks with the
predecessor. That is already dramatically better than the status quo, and it makes
the hot path an optimisation rather than a dependency.

## Reproducibility is never traded for liveness

The Smalltalk image trap — where the running image becomes the artifact and
reproducibility is lost.

A hot-updated process must remain describable as "artifact X plus patches Y and Z",
each content-addressed and attested, so its exact composition is always known and
reconstructible ([10](10-supply-chain.md)).

## What must be designed in now

Even though the feature is deferred, the artifact model must be able to express
**"this build supersedes that one at these specific symbols, with this ABI delta."**
If artifacts are opaque blobs, hot update becomes impossible later. Symbol-level
identity is retained from the start.

That model is now concrete: a running composition is a base result plus an ordered chain of signed,
content-addressed `patch` records. Restart decisions are also content-addressed and attested rather
than represented as errors.

## Open questions

- **Quiescence detection** for arbitrary user code, which the kernel solves with
  its own scheduler knowledge.
- **Whether to target Wasm components** where liquid controls the runtime — explicit
  typed interfaces make safe replacement decidable, and this is the strongest
  argument for what a liquid-owned compilation target would look like.
- **Whether tier 3 migrations are worth supporting at all**, given they require
  per-change developer effort.
