# Multiplayer sessions and attribution

**What.** Liquid #30 adds private speculative sessions, scoped delegation, semantic leases,
transient presence, and compaction epochs. Liquid #248 adds the per-line attribution query used by
liquify to identify the session and change that last wrote each line, including whether that write
came from a delegated agent session.

**Status.** The session, lease, presence, epoch, attribution, repository, daemon, and CLI surfaces
are implemented on macOS and Linux. The query is read-only and available directly through
libliquid, across the local daemon wire, and through `liquid blame`.

## Public records and query

[`session.hpp`](../../include/liquid/session.hpp) defines the common multiplayer records. A
`line_attribution` contains:

| Field | Meaning |
|---|---|
| `line` | One-based line number at the requested change. |
| `text` | The line contents without the terminating newline. |
| `by` | TAAP actor recorded for the operation. |
| `session` | The 16-byte session that wrote the line. |
| `change` | The change that introduced the current line contents. |
| `agent` | `true` when the operation was published under scoped delegation. |
| `identity_verified` | `true` only when an identity chain verifies `by`: from the operation that wrote the line, or from the change when no operation is recorded. Otherwise show `unverified_signer_label`, never `by` ([identity](identity.md#every-decision-that-trusts-an-identity-requires-a-verified-chain), liquid#508). |

`repository::attribution(path, change)` returns these records in line order. The path is resolved
in the requested historical tree. Attribution then follows that file's stable `node_id` through
moves and walks first-parent changes from oldest to the requested change. It applies the public
text hunks in order: unchanged lines retain their prior record, inserted or replaced lines take the
matching operation's actor, session, agent marker, and enclosing change, and removed lines leave
the result. Pure moves preserve every line's attribution.

Operation records are grouped by the `record` operation that closes a change. A matching content
operation supplies the session and actor; old repository documents without the optional agent
marker decode it as `false`. The signed change remains the historical boundary. The query does not
infer authorship from commit messages, process names, or editor metadata.

The query refuses a missing path, a directory, corrupt history, and content for which the public
diff cannot produce UTF-8 line hunks. An empty file returns an empty vector.

## Daemon and CLI

`daemon::attribution(path, change)` carries the same records over the checkout daemon's
length-prefixed canonical-CBOR request protocol. The version-1 request names `attribution`, `path`,
and `change`; the success value contains a `lines` array with all public record fields. Malformed
or incomplete responses are refused before any result reaches the caller. A line without
`identity_verified` (an older daemon) decodes as unverified.

`liquid blame <path>` resolves the current head, starts or attaches to the checkout daemon, and
prints one line as:

```text
1 change=blake3:<digest> session=<32-hex-digits> actor=<taap-id> agent=false\ttext
```

`--json` emits one versioned object with `v`, `path`, `change`, and a `lines` array. Each line has
`line`, `change`, `session`, `actor`, `identity_verified`, `agent`, and `text`. This is the stable
editor-facing form. An author no identity chain verifies is printed as `actor=signed by an
unverified key` (JSON `"actor":"signed by an unverified key","identity_verified":false`), never
as the `taap_id` the record claims (liquid#508).

## Plugin delivery

The native liquify plugin descriptor can append a versioned event callback table. Its presence,
lease, and attribution callbacks receive borrowed canonical-CBOR event bytes, so an editor plugin
can update all three multiplayer views without polling. The table and its compatibility rules are
documented in [`frontends.md`](frontends.md).

## Measurement

`liquid-attribution-measure` creates a 10,000-line file, changes one middle line, verifies all
10,000 results, and samples the query five times. A release static build on an Apple M3 Max macOS
host on 2026-09-06 recorded:

```text
lines=10000
samples_ms=5.809,6.478,6.535,6.976,7.119
median_ms=6.535
load_average=14.131,58.606,73.125
```

Run it from a configured static build with:

```sh
cmake --build build-static --target liquid-attribution-measure -j 6
build-static/tests/liquid-attribution-measure build-static/attribution-measure-repository
```

These local measurements are context rather than a cross-host benchmark.

## Threat model

Delegated sessions are their own trust boundary. The threat model in
[`../security/delegation-threat-model.md`](../security/delegation-threat-model.md) (liquid #418)
lists every must-never for delegation acceptance, session authority, and session join and leave,
where each is enforced, and the test that proves it. What holds: only a complete `delegation/1`
signed by this repository's own principal starts a session, and publish and lease are refused
outside the delegation's scopes and capabilities, at both ends of a move. A delegation has a
bounded validity window and its revocation status is asked through the caller's
`delegation_status_resolver` when the session is forked and before every lease and publish, so a
revoked or expired delegation neither starts nor continues a session
([#514](https://github.com/oramasearch/liquid/issues/514)); the CLI uses this machine's revocation
list, `$LIQUID_HOME/trust/delegation-revocations`. A delegation is bound to the agent it names, its dispatcher must be its issuer, and it names the
one repository it may be used in, so `drip.by` and `liquid blame` name the delegated agent
([#515](https://github.com/oramasearch/liquid/issues/515)); `liquid session start <delegation> --as
<agent>` starts a session as that agent. What does not hold yet: `open_session` trusts stored session authority without verifying it again
([#516](https://github.com/oramasearch/liquid/issues/516)). Sub-delegation containment is
conservative and every link of a chain (`session_options::chain`) is linked, validated and verified
when a session acts ([#517](https://github.com/oramasearch/liquid/issues/517)).

## How to verify

The delegated-session must-nevers are proved by the `Collaboration` and `AttestationRegistry` tests
named in the [threat model](../security/delegation-threat-model.md#must-nevers).
`Collaboration.line_attribution_follows_changes_sessions_agents_and_moves` checks human and agent
line ownership plus rename preservation. `Daemon.attribution_query_crosses_the_wire` compares the
daemon result with the direct repository query. `cli-blame` checks the text and JSON contracts.
The static and shared full suites also compile every public C++ header independently.

## Limits

- Attribution is defined for UTF-8 text with line hunks. Binary files return `errc::unsupported`.
- History traversal uses the requested change's first-parent chain. A merge-introduced line is
  attributed to the merge operation that introduced it on that chain.
- Presence remains transient. It is delivered to active consumers but never added to repository
  history or used to infer attribution.
