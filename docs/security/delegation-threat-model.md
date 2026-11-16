# Delegation and multiplayer sessions: threat model

**What.** The threat model for `delegation/1`, its attenuation, and the private speculative
sessions that act under it (liquid #30, the CLI from #246). It was written for
[liquid #418](https://github.com/oramasearch/liquid/issues/418) (finding L-36 of the October 2026
review) by reading the code at `27d56ba` on `dev`, not the specifications. Line numbers below are
at that commit.

**Result.** The checks that exist are sound where they apply: a delegation must be a complete,
correctly signed `delegation/1` record issued by the repository's own principal, and a session's
publish and lease are refused outside its scopes and capabilities. Four must-nevers did **not**
hold; each was filed, and each is now fixed:

| Issue | Defect |
|---|---|
| [#514](https://github.com/oramasearch/liquid/issues/514) | `fork_session` accepts revoked and expired delegations: it made up its own "not revoked, checked now" status, and `delegation/1` had no expiry. **Fixed**: a required `not_after` and a required status resolver, asked at fork, lease and publish. |
| [#515](https://github.com/oramasearch/liquid/issues/515) | A delegation was not bound to the agent, the dispatcher or the repository that uses it. **Fixed**: subject, dispatcher and a required `repository` attribute are checked at fork, and `liquid session start --as <agent>` acts as the delegated agent. |
| [#516](https://github.com/oramasearch/liquid/issues/516) | `open_session` restored authority from unsigned session state without verifying it again. **Fixed**: `open_session` verifies the stored authority exactly as `fork_session` does and checks the digests recorded at fork. |
| [#517](https://github.com/oramasearch/liquid/issues/517) | `validate_delegation` let a sub-delegation broaden its scopes and did not link it to its parent; chains were never checked when a session acts. **Fixed**: conservative containment, linked links, and `session_options::chain` validated and verified link by link. |

The must-nevers that hold and had no test now have one (*Must-nevers* below).

## Scope

In scope: `delegation/1` (`src/attestation/registry.cpp`), `validate_delegation`, `verify_attestation`
as used for delegations, `repository::fork_session` and `open_session`, every `session` member
(`src/repository/repository_impl.cpp`), the version-3 drip fields that record delegated work,
attribution, semantic leases and presence as a session uses them, and `liquid session`
(`src/cli/session.cpp`).

Out of scope here: TAAP signature and identity-chain verification in general (#383, #391), the
transport that would carry sessions between hosts (there is none today: no daemon request and no
sync item carries a session), Lake's own handling of delegations, and exclusive locks
([`../library/locks.md`](../library/locks.md)).

## Assets

| Asset | Why it matters |
|---|---|
| The shared repository's tree and history | A publish lands in it, signed by the principal's key. |
| The principal's signing key | Every drip a session publishes is signed with it (`append_drip`, `repository_impl.cpp:5044-5098`); the agent never holds a key of its own. |
| The `delegation/1` attestation | The only statement of what the agent may do: `scopes`, `may_publish`, `may_lease`, `depth_remaining`, `dispatcher`, optional `model` (`registry.cpp:138-147`). |
| Session state (`LIQUID_SESSION` and the private replica) | Holds the agent, the delegation and the fork point (`encode_stored_session`, `repository_impl.cpp:8670-8681`). |
| Attribution | `drip.by`, `drip.delegation`, `line_attribution.agent` and `liquid blame`: what reviewers and editors are told about who wrote a line. |
| Semantic leases | Block other writers from a file, symbol or range. |

## Actors and trust boundaries

| Actor | Trust |
|---|---|
| **Principal** | The local identity (`identity_detail::local_signer()`). Issues delegations; owns the repository and its key. Trusted. |
| **Agent** | The delegate. Names itself in `session_options.actor`. Holds only the delegation and a session id. Untrusted beyond what the delegation grants. |
| **Subagent** | An agent that received a sub-delegation. Untrusted beyond its own delegation and every ancestor. |
| **Dispatcher** | Whoever launched the agent; recorded as the `dispatcher` attribute, which must be the delegation's issuer (#515). |
| **Library caller** | Lake, liquify or the CLI, calling `fork_session` and `session::*`. Chooses the actor, supplies the delegation, and supplies the `delegation_status_resolver` that answers revocation (#514; the CLI reads `$LIQUID_HOME/trust/delegation-revocations`). |
| **State or store writer** | Anything that can write the repository's state backend or object store: a remote backend, a transferred copy, another local process. Untrusted. |

Boundaries:

1. **Delegation acceptance** — `fork_session` turns a supplied attestation into session authority.
2. **Session state** — `open_session` turns stored bytes back into authority.
3. **Publish and lease** — the session's private replica meets the shared repository.
4. **Sub-delegation** — `validate_delegation(child, &parent)` decides whether a child is an
   attenuation of its parent.

## Data flow

```text
principal ──signs delegation/1──▶ caller ──fork_session(actor, delegation)──▶ repository
                                                │ validate_delegation (no parent)       8738
                                                │ window + resolver status (#514)
                                                │ issuer == this principal              8751
                                                │ scopes / may_publish / may_lease      8755-8767
                                                ▼
                                  delegation stored (put_plain)               8772
                                  private replica + LIQUID_SESSION written    8796-8848
                                                │
agent ──session write/move/remove──▶ private replica (no scope check, by design)  8953-8989
agent ──session lease──▶ may_lease + scope check ──▶ shared lease            8991-9013
agent ──session publish──▶ may_publish 9044, diff base→private, every old and new
                           path in scopes 9049-9073, base + leases 9080-9095,
                           write each path to shared under acting_actor/acting_delegation,
                           version-3 drips (author = principal, agent = actor,
                           delegation = digest) 5081-5084, state removed 9279
CLI later: liquid session write|publish <id> ──▶ open_session ──▶ LIQUID_SESSION decoded,
                           authority verified again as at fork (#516)
```

## Must-nevers

Each row: the property, where it is enforced, the test that proves it, and its status. Tests are in
`tests/test_collaboration.cpp` (`Collaboration.*`) and `tests/attestation/test_registry.cpp`
(`AttestationRegistry.*`); those marked **new** were added with this review.

### Delegation acceptance

| Id | Must never | Enforced at | Test | Status |
|---|---|---|---|---|
| D1 | Anything but a complete, unambiguous `delegation/1` record (right profile, subject kind `actor`, every required attribute with its type, no duplicate attribute, non-empty scopes) becomes session authority. | `validate_delegation` `registry.cpp:480-490`; `attestation_profile_registry::validate` `registry.cpp:269-314`; empty scopes `repository_impl.cpp:8755-8764` | `delegation_refuses_other_profiles_and_incomplete_authority` (**new**); `fork_session_refuses_altered_unsigned_and_empty_delegations` (**new**) | Holds |
| D2 | A delegation altered after signing, or unsigned, starts a session. | `verify_attestation` `registry.cpp:404-425`, called at `repository_impl.cpp:8742-8750` | `fork_session_refuses_altered_unsigned_and_empty_delegations` (**new**) | Holds |
| D3 | A delegation issued by anyone but this repository's principal starts a session. | `repository_impl.cpp:8751-8754` (and the identity-chain check inside `verify_attestation`) | `fork_session_refuses_a_delegation_issued_by_another_identity` (**new**) | Holds |
| D4 | A revoked, expired or future-dated delegation starts a session, or a running session leases or publishes after its delegation is revoked or expires. | `check_delegation_window` (required `not_after`, at most 30 days, 5 minutes of skew) and the caller's `delegation_status_resolver` (required; an error refuses), both in `check_delegation_now`, called by `fork_session`, `session::lease` and `session::publish` | `fork_session_refuses_missing_revoked_and_stale_status`, `fork_session_refuses_expired_future_and_unbounded_delegations`, `publish_and_lease_refuse_after_revocation_or_expiry`; `AttestationRegistry.delegation_window_refuses_expired_future_and_unbounded_delegations`, `local_delegation_status_reads_the_revocation_list` | Holds ([#514](https://github.com/oramasearch/liquid/issues/514)) |
| D5 | A sub-delegation keeps or raises `depth_remaining`, delegates from depth 0, or adds `may_publish` or `may_lease`. | `validate_delegation`, applied by `fork_session` to every link of `session_options::chain` | `delegation_chains_only_attenuate_authority`; `sub_delegation_reduces_depth_and_never_adds_leases`; `Collaboration.fork_session_validates_every_link_of_a_delegation_chain` | Holds ([#517](https://github.com/oramasearch/liquid/issues/517)) |
| D6 | A sub-delegation's scopes match a path its parent's scopes do not. | `within` in `validate_delegation`: a child scope must equal a parent scope or lie under a parent `P/**` or `**`; everything else is refused | `sub_delegation_scopes_are_contained_conservatively`; `fork_session_validates_every_link_of_a_delegation_chain` | Holds ([#517](https://github.com/oramasearch/liquid/issues/517)) |
| D7 | An unlinked chain is accepted, or a session acts on a link that was never checked against its parent. | `validate_delegation(child, &parent)`: child issuer is the parent's subject, same repository, issued no earlier and ending no later; `fork_session` validates, binds, verifies and asks the status of every link, the root must be the principal's, and `check_delegation_now` repeats the verification and status for the whole chain before every lease and publish | `sub_delegation_must_be_linked_to_its_parent`; `fork_session_validates_every_link_of_a_delegation_chain` | Holds ([#517](https://github.com/oramasearch/liquid/issues/517)) |
| D8 | A session acts as, or is attributed to, an agent other than the delegation's subject; or names a dispatcher who did not issue it. | `check_delegation_binding` in `fork_session`: `delegation.subject == delegation_subject(options.actor)` and `dispatcher == issuer.taap_id`; the CLI acts as `--as <agent>` | `fork_session_binds_the_agent_dispatcher_and_repository`; `cli-session` | Holds ([#515](https://github.com/oramasearch/liquid/issues/515)) |
| D9 | A delegation issued for one repository authorises work in another. | `check_delegation_binding`: the required `repository` attribute equals the forked repository's id | `fork_session_binds_the_agent_dispatcher_and_repository` | Holds ([#515](https://github.com/oramasearch/liquid/issues/515)) |
| D10 | `repository::issue_delegation` grants more than its caller asked for, outlives `delegation_maximum_lifetime`, or signs for anyone but this repository's principal. | `issue_delegation`: capabilities default to none (`may_publish`, `may_lease` false, `depth_remaining` 0), `lifetime` defaults to 15 minutes and must be positive and at most 30 days, scopes are required and non-empty, the principal cannot be the agent, the record is checked by `validate_delegation` and `check_delegation_window` before it is signed with the repository's own key, and only root delegations are issued | `DelegatedEdit.issue_delegation_takes_the_narrowest_defaults_and_refuses_broad_requests` | Holds ([#538](https://github.com/oramasearch/liquid/issues/538)) |
| D11 | An in-process agent's identity is claimed by a key, or names an agent under another principal. | `local_agent`: `<principal>/agent/<name>`; key-derived TAAP ids never contain `/`, the name alphabet excludes `/`, and a local agent cannot be a principal | `DelegatedEdit.issue_delegation_takes_the_narrowest_defaults_and_refuses_broad_requests` | Holds ([#538](https://github.com/oramasearch/liquid/issues/538)) |

### Session authority

| Id | Must never | Enforced at | Test | Status |
|---|---|---|---|---|
| S1 | A session without `may_publish` publishes. | `session::publish` `repository_impl.cpp:9044-9047` | `session_without_capabilities_cannot_publish_or_lease` (**new**) | Holds |
| S2 | A session without `may_lease` takes a lease. | `session::lease` `repository_impl.cpp:8995-8998` | `session_without_capabilities_cannot_publish_or_lease` (**new**) | Holds |
| S3 | A session leases a path outside its scopes. | `repository_impl.cpp:8999-9002`, `session_path_allowed` `8723-8726` | `session_lease_outside_delegation_scopes_is_refused` (**new**) | Holds |
| S4 | A publish changes a path outside the scopes, including either end of a move. | `repository_impl.cpp:9049-9073` (old and new path of every blob entry) | `speculative_writes_are_private_and_publish_is_attributed_and_scoped`; `publish_refuses_a_move_across_the_delegation_scopes` (**new**) | Holds |
| S5 | A session's writes reach the shared repository before publish. | Private replica, `repository_impl.cpp:8796-8815`, `8953-8989` | `speculative_writes_are_private_and_publish_is_attributed_and_scoped` | Holds |
| S6 | A refused or failed atomic publish leaves part of its writes in the shared repository. | Scope and base checks run before any write (`9049-9095`); `restore_shared` `9114-9130`, used on a failed atomic write `9181-9195` | `speculative_writes_are_private_and_publish_is_attributed_and_scoped`, `session_without_capabilities_cannot_publish_or_lease` (**new**) | Holds |
| S7 | A publish skips another session's or a human's lease. | `validate_lease_write` per path `repository_impl.cpp:9087-9095`, `5738-5753` | `symbol_leases_refuse_intersections_allow_other_ranges_and_expire` | Holds |
| S8 | Delegated work is recorded without its delegation. | `append_drip` `repository_impl.cpp:5081-5084` (signed version-3 body); attribution `agent` marker | `speculative_writes_are_private_and_publish_is_attributed_and_scoped`, `line_attribution_follows_changes_sessions_agents_and_moves` | Holds (but the agent named is unbound, D8) |
| S9 | A structural edit applied in a session writes outside the scopes, or its published operation names someone other than the agent and its principal. | `session::apply_edit` checks every file against the scopes and the delegation's window and status before applying; `session::publish` groups the edit's writes into one `structural_edit` operation with `by` the agent, `agent` set and `principal` recorded | `DelegatedEdit.an_assistant_edit_is_published_as_the_agent_for_the_principal`, `DelegatedEdit.an_edit_outside_the_delegation_scopes_is_refused_before_it_applies` | Holds ([#538](https://github.com/oramasearch/liquid/issues/538)) |

### Multiplayer state

| Id | Must never | Enforced at | Test | Status |
|---|---|---|---|---|
| M1 | A reopened session acts on authority that was not verified again, or that was changed in storage. | `open_session` runs `authorize_session`, the same checks as `fork_session` (chain, binding to the stored agent and this repository, window, status from the caller's required resolver, signatures, principal root), and refuses a stored delegation or chain whose digests differ from those recorded at fork (`LIQUID_SESSION` version 2) | `open_session_verifies_the_stored_delegation_again` | Holds ([#516](https://github.com/oramasearch/liquid/issues/516)) |
| M2 | A published or discarded session is used or reopened again. | `impl_->active` checks on every member; state removed `repository_impl.cpp:9279`, `9303` | `finished_sessions_cannot_be_reused_or_reopened` (**new**) | Holds |
| M3 | A session releases a lease held by another session or by the human. | `repository::release` `repository_impl.cpp:9553-9556` (actor and session) | `a_session_cannot_release_another_sessions_lease` (**new**) | Holds |
| M4 | Presence is written into history or used to infer attribution. | `report_presence` is transient; attribution reads operations only | `presence_is_transient_and_crosses_repository_sync` | Holds |

## STRIDE summary

| Threat | Where | Today |
|---|---|---|
| **Spoofing** the agent | `options.actor` must be the delegation's subject (D8) | Mitigated, #515 |
| **Spoofing** the issuer | issuer check and identity chain (D2, D3) | Mitigated |
| **Tampering** with the delegation in flight | signature (D2) | Mitigated |
| **Tampering** with stored session authority | `LIQUID_SESSION` is unsigned, so its authority is verified again on open (M1) | Mitigated, #516 |
| **Repudiation** of agent work | version-3 drips carry the delegation digest, signed by the principal, and name the delegation's subject (S8, D8) | Mitigated |
| **Information disclosure** | a session reads the whole private replica, not only its scopes | Accepted: scopes limit writes, not reads (below) |
| **Denial of service** | an in-scope lease blocks the human's writes until it expires | Accepted: leases are time-bounded; Lake can authorise them (`authorize_lease`) |
| **Elevation of privilege** | broadening or unlinked sub-delegation (D6, D7) | Mitigated, #517 |
| **Elevation of privilege** | revoked or expired delegation (D4) | Mitigated, #514: bounded window and required status at fork, lease and publish |

## Confused deputy

The deputy is the repository: it holds the principal's key and signs whatever a session publishes.
The confused-deputy question is whether a party can make it use that key for something the
principal did not intend.

- **Who may present authority.** Only a delegation the principal signed (D2, D3). This is the strong
  part: a delegation from another identity, or an edited one, is refused.
- **For whom.** Checked (#515). The acting agent must be the delegation's subject
  (`delegation_subject`, the BLAKE3 digest of its TAAP id), and the dispatcher must be the issuer.
  The CLI acts as the agent named by `--as`, so CLI history names the agent, not the human.
- **Where.** Checked (#515). The required `repository` attribute names the one repository a
  delegation may be used in.
- **When.** Checked (#514). A delegation is valid from `at` (5 minutes of skew) to `not_after`, at
  most 30 days, and its status comes from the caller's resolver at fork and before every lease and
  publish.
- **Through whom.** Checked (#517). A session may carry the chain above its delegation
  (`session_options::chain`, root first); every link must be a linked attenuation of the one before
  it, the root must be the principal's, and every link is verified and its status asked.

The depth limit, which `docs/features/25` names as *the* mitigation, is enforced by the session
itself for every link of the chain it acts under. An intermediate link is signed by an agent, so
that agent's identity chain must be in this machine's identity store (`trust_identity_chain`);
otherwise the link does not verify and the session is refused.

## Attenuation, depth, scope, expiry and revocation

- **Attenuation of capabilities.** `may_publish` and `may_lease` can only be kept or dropped (D5).
- **Depth.** A child must have strictly less `depth_remaining`, and depth 0 cannot delegate (D5).
  `fork_session` applies this to every link of the chain a session acts under (D7, #517).
- **Scope.** Path globs matched by `glob_matches` (`repository_impl.cpp:4022-4048`), where `*` and
  `?` do not cross `/` and `**` does. A session's own checks use this matcher exactly. Sub-delegation
  containment is conservative (D6, #517): a child scope must equal a parent scope or lie under a
  parent `P/**` (or `**`). A child that might be contained but is not one of these, such as
  `src/main.cpp` under `src/*.cpp`, is refused. Scopes
  limit writes, leases and publishes, never reads.
- **Expiry.** `delegation/1` requires `not_after` (seconds since the epoch). `check_delegation_window`
  refuses one that has expired, one issued more than 5 minutes in the future, and one valid for
  longer than 30 days (D4, #514).
- **Revocation.** `delegation/1` is revocable with a one-hour status age. The status comes from the
  caller's `delegation_status_resolver`, given the delegation and its digest; an empty resolver or an
  error refuses. `fork_session`, `session::lease` and `session::publish` each ask it, so a revocation
  stops a running session (D4, #514). The CLI's resolver, `local_delegation_status`, reads
  `$LIQUID_HOME/trust/delegation-revocations` on every call, until Lake serves delegation status.

## Replay

A delegation is a bearer record: anyone who can pass it to `fork_session` in the principal's
repository can start a session, any number of times, until its `not_after` or its revocation (#514;
there is no nonce or use count). Within one session there is no replay concern for publish: a finished session cannot be
reused or reopened (M2), and each publish re-checks its base change and cursor
(`validate_write`, `repository_impl.cpp:9080-9086`). Expiry and
real status (#514) bound replay in time; binding to agent and repository (#515) bounds who and where.

## Multiplayer sessions: join, leave and authority

- **Join.** A session is created only by `fork_session` (no daemon or sync request creates one).
  Its id is 16 random bytes (`random_id()`); the id is a handle, not a credential, because whoever
  can call `open_session` can already write the repository's state.
- **Rejoin.** `open_session` verifies the authority in `LIQUID_SESSION` exactly as `fork_session`
  does, with this repository's principal and the caller's status resolver, and refuses a delegation
  or chain whose digests differ from those recorded at fork (M1, #516). The record travels with a
  repository transfer (`tests/store/test_transfer.cpp`); on a host whose principal never issued its
  delegation it is refused.
- **Authority while joined.** Writes go to the private replica only (S5). Leases and publishes are
  checked against the verified scopes and capabilities (S1-S4), after the window, status and
  signatures of every link are checked again (D4, D7). Presence carries the session's agent
  and id, is never history (M4), and is not scope-checked: an agent can report presence on any path.
- **Leave.** `publish` and `discard` remove the session state and mark the handle inactive (M2).
  `discard` releases the session's own leases; a session cannot release anyone else's (M3). A
  session that is neither published nor discarded keeps its state, and its leases expire with their
  time to live.

## Accepted risks

- Scopes do not limit reads: the private replica is a full copy at the fork point. Confidential
  paths need encryption ([`../library/encryption.md`](../library/encryption.md)), not scopes.
- Presence is unauthenticated metadata and is not scope-checked; it is transient and never used for
  attribution.
- The `model` attribute is the issuer's statement and is not verified.

## How to verify

Run `ctest --test-dir build -R 'Collaboration|AttestationRegistry'`. The tests named in the tables
are the proof for each holding must-never. `cli-session` proves that `liquid session start --as`
acts as the delegated agent.
