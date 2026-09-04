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

**Implementation status.** Issues #10, #11, #12, #13, #14, and #15 implement the local action path,
disk cache, `liquid wrap`, reproducibility verification, automatic SLSA provenance, and cache
diagnostics:
canonical action,
sandbox, and result records; a fixed environment; toolchain materialisation; an observed FUSE
mount; declared-output collection; and Linux namespace execution. The read manifest is assembled
only from the mount's request callback, which carries the FUSE caller pid. It has no tracing,
preload, or DTrace dependency. macOS carries the generated deprecated `sandbox-exec` profile but
requires macFUSE before it can run the fixed virtual-root path; Windows remains explicitly
unsupported until Phase 6. The disk cache validates observed manifests before a hit, verifies every
result's TAAP signature and persists its action index. `wrap` discovers a project's invoked tools,
pins their content tree in `config.wrap.toolchain`, splits the mount stream at output-producing leaf
processes, publishes the inner and outer actions, and serves valid results through the sandbox-only
exec hook. Verification re-executes sampled cached actions, emits signed attestations, and
quarantines divergent results. Each result references a verified, device-signed
`slsa_provenance/1` attestation derived from its action and manifest. Memory caching and remote
caching and execution remain their own issues.

## Records in the store

Two record kinds reserved in the object model become concrete, mirroring REAPI's
`Action`/`Command`/`ActionResult` closely enough that Phase 3's REAPI mapping is a
translation, not a redesign:

```
action  { v, kind:"action", inputs: digest(tree), command: [argv…], cwd, env: {k: v}, outputs: [path…],
          platform: { os, arch }, toolchain: digest(tree), sandbox: digest(sandbox definition), timeout_s? }
result  { v, kind:"result", action: digest, exit: int, outputs: [ { path, mode, blob: digest } … ],
          stdout: digest, stderr: digest, manifest: [ { path, digest } … ],   # everything the action read, with content hashes
          timing: { queued_ms, sandbox_ms, exec_ms, upload_ms }, worker: string, observed_at: time,
          started_at: time, finished_at: time, provenance: digest(attestation),
          producer: actor, sig: TAAP-signature }
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

Issue #11 implements both stages through `src/build/cache.{hpp,cpp}`. Validation resolves each
manifest path from the action's current input-tree record and compares the referenced blob digest;
it never hashes a checkout path. Missing paths have no actual digest. Results are tried newest
first. A candidate is served only when its result and action records decode, the action derives the
same candidate key, the producer's TAAP `result` signature and referenced provenance verify, every
manifest entry matches, and every output and stream blob record still exists.
`action_result::cache` and `why` describe the current execution request and are not part of the
immutable producer-signed result bytes.
`src/build/why.cpp` turns recorded comparisons into the public `why_report` and persists the
completed-wrap counters used by `liquid cache stats`.

### Diagnostics and lookup history

The local disk cache appends one compact `LOOKUPS` row after a completed action lookup. A row holds
only hit or miss, the selected or newly produced result digest, and the prior result digest when one
exists. It does not duplicate a cause. `liquid why` loads the immutable action and result records
named by the row and derives the cause from their fields:

| First recorded difference | Classification | Detail |
|---|---|---|
| argv | `command changed` | complete old and new argv; the CLI prints changed indexes |
| fixed environment | `environment changed` | first differing key and its optional values |
| toolchain tree | `toolchain changed` | old/new tree digests and first differing decoded tool path |
| sandbox record | `sandbox changed` | old/new definition digests |
| prior manifest against the current lookup or result manifest | `input changed` | path and both blob digests; UTF-8 line hunk and repository attribution when available |
| no prior result | `never built` | the recorded current result remains available in the report |
| an action-index entry marked divergent by `liquid verify` | `divergent result quarantined` | result digest and its verification attestation digest |
| incomplete or indistinguishable records | `unknown` | known action/result identifiers and a detail string |

Candidate fields are checked before manifests because candidate selection occurs before manifest
validation. This makes a command change the cause even if source also changed. Input validation at
lookup time resolves the current digest from the stored input tree; a completed result comparison
uses the two stored manifests directly. Both versions' blobs remain in the CAS, so the explanation
works with an action index and CAS copied from another machine. When `LOOKUPS` is not copied,
action-index sequence supplies the most recent result pair. Text classification requires valid
UTF-8 without disallowed control bytes; binary inputs receive the digest pair without a line claim.

Tracked input attribution comes from the repository's ordered operation and signed-drip history.
The path and current content digest select the newest write operation; its node, actor, time, and
recorded order select the corresponding signed drip. The displayed drip identifier is the content
digest of its canonical body. If the correspondence cannot be established, attribution is omitted
rather than inferred.

The adjacent `stats` file is a recoverable projection of completed wrap reports. It totals actions,
miss reasons, bytes avoided by disk hits, and bytes rebuilt from output blob sizes. Memory and
remote tier fields are explicitly unavailable. Result timing fields, when non-zero, yield the five
longest actions in a wrap report; dependency-aware critical-path calculation remains #57.

Divergence is not inferred from two different output lists. `liquid why` uses only the authoritative
divergence flag stored in the action-index entry by `liquid verify`; it reports the linked verification
attestation digest when present.

Tiers (features/05) share one store and one key:

| Tier | Where | Phase |
|---|---|---|
| Memory | the daemon's map of candidate keys to results | Phase 4 (#21) |
| Disk | `result` and `action` records and output blobs in the local store; the index under `actions/` is a small run set like the chunk index | Phase 2 (#11) |
| Remote | lake, REAPI's ActionCache and CAS | Phase 3 (#16), Phase 6 |

The Action Cache and the CAS are the two halves the object model already separates
(records versus chunks), and their lifetimes are coupled by the collector (#8): a result
whose output chunks were collected is never returned, because the result record is reachable
only through the roots that keep its outputs. The same reference graph keeps the result's
provenance attestation.

### Disk action-index format

The disk tier lives under `.liquid-store/actions/`. `ROOT` contains the text digest of a canonical
`action-index-root` record in the store. That record contains every candidate/result pair, its
monotonic sequence, permanent divergence flag, and optional verification-attestation digest. It is
both the recovery source and the collector root. The repository keeps it
with the same 30-day persisted lease used for repository refs, replacing or renewing that lease on
every publish or verification. The collector's `action-index-root` extractor reaches results and
verification attestations; the `result` extractor
reaches its action, output blob records, stdout, and stderr; the `action` extractor reaches inputs,
toolchain, and sandbox. Once the cache-root lease is no longer live, collection can remove the
result and its outputs together. A stale on-disk candidate then misses because lookup requires all
referenced records to exist.

`INDEX` is a checksummed sorted projection rebuilt from `ROOT` when absent, corrupt, or for a
different root:

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 8 | `LQACT`, version byte `2`, two zero bytes |
| 8 | 8 | record count, little-endian |
| 16 | 8 | next sequence, little-endian |
| 24 | 1 + 7 | root digest algorithm and reserved zero bytes |
| 32 | 32 | root digest bytes |
| 64 | 114 × count | fixed records described below |
| end | 8 | FNV-1a checksum of every preceding byte |

Each 114-byte record is candidate algorithm plus 32 digest bytes, result algorithm plus 32 digest
bytes, an eight-byte sequence, a one-byte divergence flag, a 33-byte attestation digest or zeros,
and six reserved zero bytes. Records sort by candidate algorithm and digest ascending, then sequence
descending, so candidate lookup returns newest non-divergent results first. Version 1 indexes are
accepted and rewritten as version 2 on the next mutation. `ROOT` is replaced before `INDEX`; a crash
between them is recovered from the store record.

This index deliberately uses one sorted file rather than the chunk index's mapped run set. A chunk
run pays 256 KiB for its 65,537-entry fanout before storing one record; at 114 bytes per action row,
2,299 action rows occupy less than that fanout alone. Local repositories are expected to have far
fewer action candidates than chunks, and publication is explicitly opt-in, so atomic rewrite is the
smaller disk and implementation cost for this phase. The root record preserves a migration path to
runs without changing candidate keys or collector reachability.

Trust (features/05): developer machines read; only signed writers publish (the signature is
the producer's TAAP identity on the result record, the same `sig` field as changes); CI and
verified-reproducible builds are the trust anchors. Sampled re-execution compares two results for
one action digest and treats divergence as a critical alert.

`repository_options::cache.publish` is persisted in the repository document and defaults to
`false`. Execution still produces a signed result when publication is disabled, but `publish`
returns `errc::unauthorized` and no action-index entry is added. CI or another trusted producer must
create the repository with publication enabled. Lookup verifies signatures even though this phase
has no shared tier, keeping unsigned data out when the remote tier arrives.

### Reproducibility verification

`repository_options::verify.rate` is persisted and defaults to 20: sampling selects the first and
then every twentieth unverified cache entry. `wrap` schedules that local sample after the user's
command completes on the shared runtime and returns without waiting; `--no-verify` suppresses it.
The on-demand forms are `liquid verify <action>`, `liquid verify --all`, and `liquid verify
--changed`.

Each selected action is reconstructed from its content-addressed input and re-executed in a fresh
instance of the exact recorded sandbox definition. Verification compares exit status, stdout,
stderr, and every declared output by digest. Agreement writes a device-signed
`reproducibility/1` attestation whose subject is the cached result and whose attributes name the
verifier, sandbox digest, and agreement. Disagreement adds the differing output and digest pairs,
marks every index row for that result permanently divergent, and appends a critical alert to
`cache_report`. Lookup skips divergent rows before any bytes can be served. The CLI names the action
and outputs and exits non-zero; `--explain` reports the first differing byte because Phase 2 has no
structured output codecs.

Local replay is the Phase 2 trust check. Re-execution on a different worker remains Phase 3 work;
it uses the same comparison and attestation records.

## `liquid wrap`: an existing build becomes actions

```
liquid wrap cmake --build build
liquid wrap /bin/sh -c './configure && make'
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

After an observed run, `wrap` snapshots the completed working tree as the replay input. A verifier
materialises that tree in a fresh directory, removes the cached outputs, and launches the recorded
command with the same wrap sandbox definition. This preserves intermediate inputs for independently
replaying child and outer actions without consulting the checkout.

`compile_commands.json` is a first-class input: when present, `wrap` seeds the action list
from it and can run compiles in parallel across the runtime's workers instead of waiting for
the driver to spawn them.

The exec-time hook is the implemented choice. It is a preload library mounted or allowed only in
the sandbox: Linux uses `LD_PRELOAD`, macOS uses `DYLD_INSERT_LIBRARIES`, and the parent process
environment is never changed. Before launch, the wrapper validates saved action manifests and
writes a per-run plan keyed by executable, fixed-root working directory, and argv. A planned hit
exits from the tool process constructor after the wrapper has materialised its outputs. This also
handles drivers that invoke an absolute compiler path, which a `PATH` wrapper would miss.

The first fixture set exposed two details now encoded in the implementation. Build drivers such as
Ninja write bookkeeping files, so a writer is an inner action only when no output-writing descendant
belongs below it; this leaves compiler, linker, and configure probes as the boundaries while the
driver remains the outer action. FUSE shutdown must wake the request loop through a fresh lookup,
because an attribute lookup of the mount root can be answered from the kernel cache without waking
the blocked worker.

The configure fact base (#56) is the same mechanism applied to `AC_CHECK_*` probes: a probe is
an action keyed by (toolchain, sysroot, target), and its result is a cached fact.

## What Phase 2 does not do

No query graph beyond actions (Phase 4 turns queries and actions into one engine), no remote
tier (Phase 3), no daemon (Phase 4), no per-definition granularity (Phase 4). Actions are the
coarse end of the spectrum features/04 describes; the fine end arrives when the graph does.

## How to verify

- An unmodified CMake, Autotools, or Make project builds under `liquid wrap`; a second checkout at a
  different host path reuses every unchanged compile through the fixed virtual root.
- Under-hashing is caught by construction: a test edits a header that was read but not
  declared and asserts a miss naming that header.
- A pure action receives a signature-valid `reproducibility/1` attestation.
- An action that writes random bytes is detected, named in the alert, marked divergent, and not
  served after reopening the repository.
- A rate of one verifies every action, and `liquid verify --all` verifies every action in a wrapped
  recursive-Make fixture.
- Every executed action carries a verified SLSA provenance attestation, and an identical cached
  execution reuses the same attestation digest.
- `why` names a changed header with both digests, its UTF-8 line hunk, and its repository drip.
  Portable record tests cover command, environment, toolchain, sandbox, never-built, imported-index,
  authoritative divergence quarantine, and cumulative-stat explanations. The privileged Linux wrap
  test proves the observed header and completed-wrap statistics end to end.

## How to change

A new field in `action` or `result` is a versioned record change like any other. A new
platform's enforcement is a row in the table and a `src/sandbox/<platform>.cpp`; the
observation layer never changes per platform. A new tier is a new lookup site over the same
key.

## Open

- Sandbox cost on macOS (`sandbox-exec` is deprecated but present; FSKit-based enforcement is
  the successor) and the Windows path (Phase 6).
