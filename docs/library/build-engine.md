# The build engine: actions, the sandbox, and the cache

**What.** The design for Phase 2 (the build core): how an action is represented, how liquid
observes what an action reads, how a result is cached and looked up, and how `liquid wrap`
turns an existing build into cached actions with no configuration. It fixes the records,
the observation mechanism per platform, the cache key, and the tier model. features/04, 05,
06 and 10 are the specification; this page is the shape the implementation follows, the way
[`object-model.md`](object-model.md) is for Phase 1.

**Why.** Every build system that asks users to declare dependencies has a maintenance
burden users refuse, and every cache that under-hashes ships wrong binaries. liquid's
advantage is structural: it owns the filesystem. An action run inside a liquid mount has
every read recorded by the mount, attributed to the process that made it, so observed
dependencies come from the same code path that serves checkouts, with no interposition.
The cache key then covers exactly what was read, and a miss can name the input that
changed.

## Records in the store

Two record kinds reserved in the object model become concrete, mirroring REAPI's
`Action`/`Command`/`ActionResult` closely enough that Phase 3's REAPI mapping is a
translation, not a redesign:

```
action  { v, kind:"action", inputs: digest(tree), command: [argv…], cwd, env: {k: v}, outputs: [path…],
          platform: { os, arch }, toolchain: digest(tree), sandbox: digest(sandbox definition), timeout_s? }
result  { v, kind:"result", action: digest, exit: int, outputs: [ { path, mode, blob: digest } … ],
          stdout: digest, stderr: digest, manifest: [ { path, digest } … ],   # everything the action read, with content hashes
          timing: { queued_ms, sandbox_ms, exec_ms, upload_ms }, worker: string, observed_at: time }
```

- The **toolchain is an input tree** (features/06): compilers, sysroot and tools are
  materialised into the sandbox from the store and addressed by content; the `toolchain`
  field is that tree's digest. `PATH` inside the sandbox points only at it.
- The **manifest** is the observed read set with hashes. It is three things at once
  (features/05): the validation key for the next lookup, the dependency edge set, and the
  provenance record (#14 wraps it in a `slsa_provenance/1` attestation).
- `outputs` in the action are the paths the action may write; the result lists what it did
  write. A write outside the declared outputs is a sandbox violation, reported with the path.

## The sandbox: observation by the mount, enforcement by the platform

Observation and enforcement are separate layers, and only enforcement differs per platform.

**Observation (every platform with a mount).** The action's input tree is mounted with the
VFS (#6) at a **fixed virtual root**, `/liquid/work/<action-id>/` on Linux and macOS, so paths
inside the sandbox are identical on every machine and `__FILE__`, `-I` and debug info carry
no checkout location (features/05's normalisation by construction). The mount records every
`open`/`read`/`readdir`/`getattr` with the calling **process id** (FUSE hands it over in the
request context), and the executor turns that stream into the manifest per process tree.
The mount is read-only except the declared output directory. Nothing is materialised until
read, so a large input tree costs nothing to set up (features/06: data movement is the cost).

**Enforcement.**

| Platform | Mechanism | What it guarantees |
|---|---|---|
| Linux | user + mount + pid + net namespaces (unprivileged `unshare`), the sandbox root bind-mounted as the only visible filesystem besides `/proc` and a private `/tmp`; seccomp is not required for correctness | a read outside the mount is impossible, not merely recorded |
| macOS | `sandbox-exec` profile denying file reads outside the mount and the fixed toolchain path, denying network; the mount is the only readable tree | the same guarantee with a weaker tool; the observation layer is unchanged |
| Windows | Phase 2: no sandbox and no mount; actions run with declared inputs only, `wrap` refuses with `errc::unsupported` and the reason; Phase 6 brings Cloud Files and a job-object sandbox | honest about the gap |

Environment inside the sandbox is fixed and minimal (`PATH` to the toolchain tree, `HOME`
and `TMPDIR` to sandbox-private directories, `LANG=C.UTF-8`, `TZ=UTC`, `SOURCE_DATE_EPOCH`
from the change's time, nothing inherited). The sandbox definition (the mount layout, the
environment, the enforcement profile) is itself a record and its digest is in the action, so
two machines with different sandbox definitions never share a cache entry by accident.

## The cache: lookup by manifest, tiers over one store

The cache key covers every input capable of changing the output (features/05): the observed
inputs, the command, the environment, the toolchain by content, and the sandbox definition.
Because observed inputs are known only after a first run, lookup is two-stage, ccache's
direct mode generalised:

1. **Candidate key**: `digest(command, cwd, env, toolchain, sandbox, platform)` → a list of
   result digests recorded under it.
2. **Validation**: for each candidate result, hash the paths in its manifest as they are in
   the current input tree; the first manifest whose hashes all match is a hit. A miss records
   which manifest entry differed, which is `liquid why` (#15) for free.

Tiers (features/05) share one store and one key:

| Tier | Where | Phase |
|---|---|---|
| Memory | the daemon's map of candidate keys to results | Phase 4 (#21) |
| Disk | `result` and `action` records and output blobs in the local store; the index under `actions/` is a small run set like the chunk index | Phase 2 (#11) |
| Remote | lake, REAPI's ActionCache and CAS | Phase 3 (#16), Phase 6 |

The Action Cache and the CAS are the two halves the object model already separates
(records versus chunks), and their lifetimes are coupled by the collector (#8): a result
whose output chunks were collected is never returned, because the result record is reachable
only through the roots that keep its outputs.

Trust (features/05): developer machines read; only signed writers publish (the signature is
the producer's TAAP identity on the result record, the same `sig` field as changes); CI and
verified-reproducible builds are the trust anchors; sampled re-execution (#13) compares
two results for one action digest and treats divergence as a critical alert.

## `liquid wrap`: an existing build becomes actions

```
liquid wrap cmake --build build
liquid wrap ./configure && make
```

`wrap` runs the whole command inside one sandbox mount. Every process the build spawns
reads through the mount, so the mount's per-process read stream splits the build into
**one action per compiler or tool invocation** without shims or `PATH` tricks: a process is
an action boundary when its executable is in the toolchain tree and it wrote a declared
output. Its command line, environment and reads become the action and manifest; its
outputs are hashed and stored. The next `wrap` of the same build runs the driver again but
every unchanged compile is served from the cache before the compiler starts, by a small
exec-time hook the sandbox installs (Linux: a `ptrace`-free `LD_PRELOAD` of `execve` inside
the sandbox only; macOS: `DYLD_INSERT_LIBRARIES` inside the sandbox only), which asks the
executor "is there a valid result for this command?" and materialises the outputs instead.
The driver's own reads (Makefiles, `compile_commands.json`, configure results) are one
outer action whose manifest explains a full re-run.

`compile_commands.json` is a first-class input: when present, `wrap` seeds the action list
from it and can run compiles in parallel across the runtime's workers instead of waiting for
the driver to spawn them.

The configure fact base (#56) is the same mechanism applied to `AC_CHECK_*` probes: a probe is
an action keyed by (toolchain, sysroot, target), and its result is a cached fact.

## What Phase 2 does not do

No query graph beyond actions (Phase 4 turns queries and actions into one engine), no remote
tier (Phase 3), no daemon (Phase 4), no per-definition granularity (Phase 4). Actions are the
coarse end of the spectrum features/04 describes; the fine end arrives when the graph does.

## How to verify

- The Phase 2 exit: an unmodified CMake or Autotools project builds under `liquid wrap` with
  correct caching across two machines, and every artifact carries a verifiable attestation.
- Under-hashing is caught by construction: a test edits a header that was read but not
  declared and asserts a miss naming that header.
- Sampled re-execution (#13) detects an injected non-determinism.

## How to change

A new field in `action` or `result` is a versioned record change like any other. A new
platform's enforcement is a row in the table and a `src/sandbox/<platform>.cpp`; the
observation layer never changes per platform. A new tier is a new lookup site over the same
key.

## Open

- Sandbox cost on macOS (`sandbox-exec` is deprecated but present; FSKit-based enforcement is
  the successor) and the Windows path (Phase 6).
- Whether the exec-time hook or a `PATH` shim is the more robust way to serve cached compiles
  under drivers that hard-code compiler paths; decide with #12's first real project.
