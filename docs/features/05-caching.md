# Caching

> Research: [06-build-and-compiler-caching](../research/06-build-and-compiler-caching/)
>
> **Requirement:** a build cache *and* a compiler cache, out of the box. Not a
> plugin, not a separate tool, not "configure ccache yourself."

## One mechanism, not two

Existing toolchains make you assemble a build cache (Bazel, Gradle, Turborepo) and a
compiler cache (ccache, sccache) with different keys, different storage and different
bugs.

In liquid a compile **is** an action, so the build cache and the compiler cache are
the same cache. There is one key derivation, one store, one eviction policy and one
diagnostic surface.

## Three tiers, zero configuration

| Tier | Scope | Purpose |
|---|---|---|
| **Memory** | Current daemon session | Keystroke latency |
| **Disk** | This machine | Across sessions and branches |
| **Remote** | Team, CI, public | Cold machines and shared work |

Same key, same content-addressed store, promoted between tiers. It works on first
run with no flags. A local-only cache is nearly worthless in CI, where every runner
starts empty — so the remote tier is not an advanced feature.

## Correctness

A cache entry is correct only if its key covers **every input capable of changing the
output**.

- **Under-hashing → false hits.** Silent, wrong binaries, and in a shared cache it
  propagates to everyone. Catastrophic.
- **Over-hashing → false misses.** Correct but useless. This is the common failure,
  and why most teams' hit rates are far below what they assume.

liquid's key covers: the input Merkle tree (observed, not declared), the command,
the environment, the toolchain **by content**, and the sandbox definition.

## Normalisation by construction

Most cache misses are self-inflicted. liquid removes the causes rather than asking
users to configure around them:

| Cause of misses | liquid's answer |
|---|---|
| Absolute paths in `__FILE__`, debug info, `-I` flags | Virtualised path root — every action sees the same paths regardless of where the checkout lives |
| Timestamps | No clock in the sandbox; `SOURCE_DATE_EPOCH` |
| `PATH`, `HOME`, locale, `TZ`, hostname, user | Fixed minimal environment |
| Compiler identified by path or mtime | Toolchain hashed **by content** and carried in the input tree |
| Tool non-determinism | Reproducibility enforcement ([10](10-supply-chain.md)) |

Nobody should have to discover `base_dir`, `-ffile-prefix-map` or
`-fdebug-prefix-map`. The paths are stable because the sandbox is stable.

## Manifests

Generalising ccache's direct mode: **every action stores a manifest of what it
actually read**, with hashes.

That manifest is three things at once — the validation key for the next run, the
dependency graph ([04](04-build-engine.md)), and the provenance record
([10](10-supply-chain.md)). It is computed once.

## Action Cache and CAS

Two stores, deliberately separate:

- **CAS** — chunks by hash ([01](01-object-model-and-storage.md)).
- **Action Cache** — action digest → result descriptor (output digests, exit status,
  streams, manifest).

This split enables **build without the bytes**: liquid can learn that an action
succeeded and what its outputs are without downloading them
([06](06-distributed-execution.md)).

Their **lifetimes are coupled**. An Action Cache entry whose chunks have been
collected is never returned. The "cache hit that cannot be materialised" failure —
endemic to Bazel deployments — is structurally excluded
([01](01-object-model-and-storage.md)).

## Trust

A writable shared cache is a fleet-wide code-execution primitive. If any machine can
write an entry others consume, one compromised or merely misconfigured machine
poisons everyone.

- **Read by default.** Developer machines consume; they do not publish.
- **Writes are authenticated and signed** by the producer.
- **CI and verified reproducible builds are the trust anchors.**
- **Sampled re-execution** locally detects divergence now; Phase 3 moves the replay to a different
  worker. Different exit status or output bytes for one action digest is a critical alert, not a
  silent overwrite. The cached result is permanently quarantined.

## Diagnostics

```
liquid why <target>
```

Names the **specific input that differed** and caused a miss. Gradle's build scan is
the bar; almost nothing else clears it.

Also reported: hit rate by tier, bytes transferred versus bytes avoided, and the
actions dominating the critical path.

**Implemented locally (#15).** The target may be an output path, action digest, or result digest.
Input explanations carry both stored digests, a UTF-8 line hunk, and recorded drip attribution;
command, environment, toolchain, sandbox, never-built, and unknown results have distinct output.
`--json` exposes the same fields. `wrap --explain` and `cache stats` report miss reasons, the disk
hit rate, and output bytes rebuilt versus avoided. The memory and remote tier rows remain explicitly
unavailable, and #57 owns dependency-aware critical-path refinement. Divergence remains unknown
until #13's quarantine record exists; diagnostics do not infer quarantine from differing bytes.

## Eviction

Size-capped with a policy that accounts for recomputation cost, not just recency —
an artifact that took 40 minutes to build should outlive one that took 40
milliseconds. Retention is expressible in terms users think in, and reachability is
computed by the tool ([09](09-artifact-service.md)).

## Open questions

- **Eviction policy weighting.** Cost-aware eviction is obviously right and
  non-trivial to tune.
- **How much history to keep in the remote tier** before archival.
- **Whether sampled re-execution runs continuously or on idle capacity**, and who
  pays for it.
