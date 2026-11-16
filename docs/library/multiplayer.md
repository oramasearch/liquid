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
| `by` | TAAP actor recorded for the operation: the agent, for a delegated write. |
| `session` | The 16-byte session that wrote the line. |
| `change` | The change that introduced the current line contents. |
| `agent` | `true` when the operation was published under scoped delegation. |
| `principal` | For a delegated write, the principal the agent acted for, whose key signed the drips (liquid #538). Absent otherwise, and for delegated operations recorded before liquid #538. |

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
or incomplete responses are refused before any result reaches the caller.

`liquid blame <path>` resolves the current head, starts or attaches to the checkout daemon, and
prints one line as:

```text
1 change=blake3:<digest> session=<32-hex-digits> actor=<taap-id> agent=false\ttext
2 change=blake3:<digest> session=<32-hex-digits> actor=<agent-id> agent=true for=<principal-id>\ttext
```

`--json` emits one versioned object with `v`, `path`, `change`, and a `lines` array. Each line has
`line`, `change`, `session`, `actor`, `principal` (a TAAP id, or `null` for a line not written
under delegation), `agent`, and `text`. This is the stable editor-facing form. The daemon wire
carries `principal` as an optional text field of each line.

`liquid log --ops` names a delegated drip's principal the same way: the text form ends with
` for=<principal-id>`, and `--json` gives each drip a `principal` field (`null` for an ordinary
drip). `drip::principal` and `operation_record::principal` carry it in the library, with
`operation_record::agent`.

## Assistant edits as a delegated agent

liquid #538 lets an editor record an accepted assistant edit as the agent acting for the person,
using the public API only:

```cpp
auto principal = repository.principal();
auto assistant = liquid::local_agent(*principal, "assistant");
auto issued = co_await repository.issue_delegation({.agent = *assistant,
                                                    .scopes = {"src/left.cpp"},
                                                    .may_publish = true,
                                                    .model = "editor-assistant"});
auto session = co_await repository.fork_session({.actor = *assistant,
                                                 .delegation = issued->delegation,
                                                 .status = liquid::local_delegation_status()});
co_await session->apply_edit(preview);
auto report = co_await session->publish({.message = "assistant edit"});
```

- `local_agent` and `issue_delegation` are described in
  [attestations.md](attestations.md#issuing-a-delegation).
- `session::apply_edit(preview)` checks every file against the delegation's scopes and the
  delegation's window and status, then applies the preview in the session's private replica, where
  each file's `before` digest must match the snapshot the session forked from.
- The edit must be the only change the session holds. It is refused after a write, move, remove or
  another edit, and those are refused after it, so the published operation is exactly the edit.
- `session::publish` checks the base, scopes and leases as for any session, then records every
  path it writes as one `structural_edit` operation. That operation carries the preview id in
  `from` and the preview's attribution in `to`, names the agent in `by`, and is marked `agent` with
  the principal. Its id is `publish_report::operation`, and `repository::undo` reverts it whole.
- The published drips are signed by the principal and carry the delegation digest and the agent,
  one drip per changed directory, as for any publish. `repository::attribution` marks the edited
  lines `agent` with both identities.
- The record lives in the session's private replica, so a session reopened with `open_session`
  publishes its edit the same way. A failure part-way through `apply_edit` leaves the session
  holding a partial edit; discard it.

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
where each is enforced, and the test that proves it. Every must-never holds:

- Only a complete `delegation/1` signed by this repository's own principal starts a session, and
  publish and lease are refused outside the delegation's scopes and capabilities, at both ends of a
  move.
- A delegation has a bounded validity window (`not_after`, at most 30 days), and its revocation
  status is asked through the caller's `delegation_status_resolver` when the session is forked and
  before every lease and publish, so a revoked or expired delegation neither starts nor continues a
  session ([#514](https://github.com/oramasearch/liquid/issues/514)). The CLI uses this machine's
  revocation list, `$LIQUID_HOME/trust/delegation-revocations`.
- A delegation is bound to the agent it names, its dispatcher must be its issuer, and it names the
  one repository it may be used in, so `drip.by` and `liquid blame` name the delegated agent
  ([#515](https://github.com/oramasearch/liquid/issues/515)). `liquid session start <delegation>
  --as <agent>` starts a session as that agent.
- `open_session` verifies stored session authority again exactly as `fork_session` does, with a
  required `session_open_options::status`, and refuses a stored delegation that differs from the
  one the session was forked with ([#516](https://github.com/oramasearch/liquid/issues/516)).
- Sub-delegation containment is conservative, and every link of a chain
  (`session_options::chain`) is linked, validated and verified when a session acts
  ([#517](https://github.com/oramasearch/liquid/issues/517)).

## How to verify

The delegated-session must-nevers are proved by the `Collaboration` and `AttestationRegistry` tests
named in the [threat model](../security/delegation-threat-model.md#must-nevers).
`Collaboration.line_attribution_follows_changes_sessions_agents_and_moves` checks human and agent
line ownership plus rename preservation. The `DelegatedEdit` tests
(`tests/test_delegated_edit.cpp`, target `liquid-delegated-edit-tests`, which links only
`liquid::liquid` and includes no private header) issue a delegation to a local agent, apply an edit
preview in its session, publish it, and read the agent and the principal back from the drips, the
operation and the attribution; they also check scope refusal and the narrow defaults. `Daemon.attribution_query_crosses_the_wire` compares the
daemon result with the direct repository query. `cli-blame` checks the text and JSON contracts.
The static and shared full suites also compile every public C++ header independently.

## Limits

- Attribution is defined for UTF-8 text with line hunks. Binary files return `errc::unsupported`.
- History traversal uses the requested change's first-parent chain. A merge-introduced line is
  attributed to the merge operation that introduced it on that chain.
- Presence remains transient. It is delivered to active consumers but never added to repository
  history or used to infer attribution.
