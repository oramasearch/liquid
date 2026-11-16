# Hot update

**Status: tiers 0–2 classification, signed patch records, cooperative tier-1 delivery, and
restart outcomes are implemented by liquid #32.** Tier 2 consumes #27's stored ABI descriptors; an
additive interface without compatible old and new descriptors is tier 3. Tier 3 migrations,
automatic quiescence discovery, Wasm, and deployment integration are not implemented.

## Safety decision

The implementation is the in-process `liquid-hot` C library chosen on 2026-09-05. A participating program links that library, registers stable entry slots,
and calls `liquid_hot_quiescent()` only where replacing those entries is safe. For a Capy program,
the contract places a quiescence call between coroutine resumptions on every executor. Arbitrary
programs must identify their own safe points; liquid does not infer them.

The alternatives were an external debugger-style patcher and a Wasm component target. An external
patcher would make attachment, sandbox policy, and safe-point discovery platform-specific. Wasm
would provide stronger typed replacement but is not a liquid compilation target. The cooperative
library makes the safety boundary explicit, works with a debugger, and follows the kernel-livepatch
model of redirecting an entry point only at a quiescent point.

The classifier is authoritative. Failure to prove a lower tier produces `restart`; it never weakens
the reproducibility or layout checks to keep a process live.

## Evidence and decision rules

`hot_update_build_evidence` associates one build result with its artifact, changed input,
per-definition units, and optional ABI evidence. Each unit records its frontend kind, stable name,
signature, source fingerprint, and output fingerprint. Unit signatures carry the normalized symbol
interface; source locations and load addresses are excluded. The content-addressed evidence record
is stored under `hot_update/evidence/<result>`.

For tier 2, the selected new descriptor must be referenced by the signed new `action_result`, name
one of its output blobs, and have an old descriptor on the same output path. Liquid compares the
stored descriptors with `diff_abi` and also requires their binary kind, architecture, install name,
and needed libraries to match. The plan carries both descriptor records as evidence. A caller-set
compatibility boolean is not accepted as proof.

`hot_update::classify(repository&, old, next)` applies these rules in order:

| Tier | Rule | Outcome |
|---|---|---|
| 0 | The two builds have the same unit keys and every unit output fingerprint is equal. A comment or formatting edit changes source evidence but not these outputs. | `patched`; no runtime operation |
| 1 | Unit kinds, names, and signatures are equal; only definition output fingerprints changed. | `patched`; redirect the changed definitions |
| 2 | Existing units and signatures are unchanged, only definitions were added, and the new evidence names an ABI descriptor that confirms layouts are unchanged. | `patched`; load the additive definitions |
| 3 | A unit was removed, a signature or non-definition changed, old code changed alongside an addition, layout evidence is absent, or layout compatibility was not confirmed. | `restart` |
| 4 | Either changed input has no frontend units or no evidence record. | `restart` |

The plan names `supersedes`, `target`, tier, outcome, changed symbols, the two frontend evidence
records, both ABI descriptors when used, and a human-readable reason. Tier 0 is successful early cutoff. Tier 4 is a
safe answer for a frontend that cannot expose definitions.

## Patch and outcome records

A live process is described as artifact X plus ordered patch records Y and Z. Each patch is
canonical CBOR and content addressed:

```text
patch { v:1, kind:"patch", supersedes:result_id, target:result_id,
        tier:1|2, symbols:[text...], object:digest }
```

`object` identifies the frontend's relocatable replacement object. `create_patch` stores the object
and record, signs a `hot_update/1` attestation, and indexes the record by target. `verify_patch`
requires a matching valid signature from an allowed signer before reading or delivering the object
(next section). `hot_update::status` walks the signed `supersedes` chain, verifying every patch the
same way, and returns the base artifact plus every patch in application order, so composition
remains reconstructible.

### Who may sign a patch

A patch is a shared library loaded into a running process, so accepting one is running its code.
Anyone who can add a record and an attestation to the repository (sync, a shared checkout) can
offer one, so a valid signature is not enough (liquid#388). `verify_patch`, `apply` and `status`
take a `hot_update_trust` record, `signers`: the TAAP identity ids allowed to sign a patch. A
patch is delivered only when a matching `hot_update/1` attestation is issued by a listed identity
**and** verifies through that identity's chain in this machine's identity store (liquid#383). The
key carried in the record is never trusted.

- **An empty list refuses every patch** (`no hot-update signers are configured`).
- An attestation from an identity outside the list is ignored, however valid its signature and
  chain; when no listed identity signed, the patch is refused with `hot-update patch is not signed
  by a configured hot-update signer` (`HotUpdate.PatchFromAnUnlistedSignerIsRefused`).
- A listed identity with no stored chain is an unknown signer, refused with `hot-update attestation
  is invalid: unknown signer` (`HotUpdate.PatchFromAnUnknownSignerIsRefused`).

Where the list comes from:

| Caller | Signer set |
|---|---|
| `liquid hot-update apply` and `status` | `$LIQUID_HOME/trust/hot-update-signers`, one identity id per line ([identity.md](identity.md#local-trust-files)). A missing or empty file refuses the command and names the file to create. The repository never supplies it. |
| The daemon's `hot-update` target | The repository's own signing identity only: the daemon delivers the patch it has just built and signed, never one it received. |
| A libliquid caller | The `hot_update_trust` it passes. |

Typically the file lists the build identity that produces patches for this machine:
`liquid identity show --json` on the build machine prints its `id`, and `liquid identity show
--chain` / `liquid identity trust <chain>` moves its chain here.

Tier 0 and restart write a canonical `hot-update-outcome` record containing the old and new results,
tier, outcome, symbols, and classifier evidence. It is signed with the same profile and indexed as
an outcome. Every plan therefore leaves a content-addressed, attested result.

## Runtime protocol and quiescence

`liquid_hot_register(name, slot)` registers the address of a stable function-pointer slot.
`liquid_hot_listen(endpoint)` opens the local Unix endpoint. Liquid verifies the record, writes the
object beneath `.liquid/hot-update/`, and sends a length-prefixed `liquid-hot/1` payload containing
the object path, tier, and old-to-new symbol map. The library loads the object and resolves every
symbol, but only assigns tier-1 pending entry slots during the next `liquid_hot_quiescent()` call.
A tier-2 additive object is retained without redirecting an existing slot. A missing tier-1
registration or any missing export rejects the whole payload.
The endpoint is created owner-readable and owner-writable; callers should place it in an
owner-controlled directory. The runtime acknowledges acceptance at the quiescence call; liquid
does not advance the daemon cursor or record a successful delivery before that acknowledgement.

The runtime does not alter stack frames, active coroutines, or data layouts. The caller guarantees
that no replaced entry is executing and that all executors reach the declared boundary. The C ABI
calls are serialized by the program. `liquid_hot_close()` restores every registered slot to its
original entry before unloading replacement objects. The C ABI is versioned independently as
`LIQUID_HOT_ABI_VERSION` in `liquid/hot_abi.h`.

The trampoline-slot implementation is supported on macOS arm64 and Linux x86_64. Windows returns
`LIQUID_HOT_UNSUPPORTED`. A production program owns the stable call indirection; direct calls that
bypass its registered slot cannot be redirected.

## Restart contract and daemon

Repository `config.hot_update` persists a `restart_hook` argument vector and configured targets of
`{name, endpoint}`. A configured target requires a non-empty restart hook. The configuration is
local to the checkout that set it through `repository_options::hot_update`: clone, pull, drive sync
and composition never carry it, so a hook named by a remote repository never runs (liquid #420,
[sync.md](sync.md#trust-configuration-stays-with-the-checkout-that-set-it)). The daemon's ordered
`hot-update` target runs after graph invalidation for every drip, builds each configured target,
selects its evidence-bearing result, compares it with `hot_update/current/<name>`, and then applies
the plan.

For tier 1 or 2 it creates and delivers a signed patch. For tier 3 or 4 it executes the configured
hook through `build::wrap`, preserving the repository sandbox contract. Liquid appends four
arguments:

```text
restart <old-result> <new-result> <tier>
```

Exit zero records the restart and advances the target result. A failed build, patch, or hook leaves
the daemon cursor and current result unchanged, so the drip is retried. `liquid status` reports the
target's last successful outcome, tier, and record digest through `daemon_target_detail`.
Deployment #84 will replace the hook with `liquid deploy` of the new image; restart remains the
classification outcome.

## Verification

After configuring and building either platform, print the demo transcript with:

```sh
build/tests/liquid-hot-demo-tests --gtest_filter='HotUpdateDemo.*'
```

The tier-1 line names the `input.txt` drip and includes one process id plus
`before=value=1 after=value=2`. The tier-3 line names the restart-hook arguments and signed record
on Linux or macOS with macFUSE. A macOS host without macFUSE prints the explicit sandbox-backend
skip reason.

`HotUpdate.*` covers comment, body, additive, signature/layout, missing-frontend, signed-chain,
unsigned-patch, unknown-signer, unlisted-signer, and empty-signer-set cases. `HotUpdateDemo.TierTwoLoadsAnAdditiveObject` verifies load-only tier-2
delivery without an existing entry slot. `HotUpdateDemo.TierOnePatchChangesARunningCapyLoop` drives the configured
daemon with a source drip and observes `value=1` followed by `value=2` in the same process.
`HotUpdateDemo.TierThreeInvokesRestartHook` verifies the sandboxed hook and its appended arguments
when a sandbox backend is installed. Linux CI has libfuse3 and executes it. macOS runs it when
macFUSE is present; otherwise it reports the same explicit skip as other sandbox-dependent tests.
