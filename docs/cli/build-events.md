# `liquid build` plans and events

`liquid build --plan [target]` prints the configured repository plan as JSON Lines without
starting actions. `liquid build [target]` sends that target through the checkout daemon and prints
the ordinary event stream described below. An empty target means the complete recorded build. A
project must first record its configuration and actions with `liquid wrap`; otherwise both forms
return `not_configured` and name that command.

Each plan line has stable schema version `v: 1`:

| Field | Value |
|---|---|
| `action` | planned action digest for the current input tree |
| `dependencies` | planned action digests that must complete first |
| `state` | `hit` or `miss` after signature, manifest, and referenced-object validation |
| `result` | selected result digest on a hit, otherwise null |
| `input.command` | complete argument vector |
| `input.cwd` | repository-relative working directory |
| `input.env` | complete fixed environment object |
| `input.outputs` | declared repository-relative outputs |
| `input.inputs` | current repository tree digest |
| `input.toolchain` | recorded toolchain tree digest |
| `input.platform` | `{os, arch}` |
| `input.timeout_s` | timeout seconds or null |
| `input.remote_eligible` | whether remote read enforcement is enabled |

Consumers should ignore unknown members within version 1 and reject unsupported versions. The
daemon's `build_plan` response uses these same fields in canonical CBOR.

`liquid build --events <command> [args...]` runs an existing build through the observed wrapper and
prints its public build stream as JSON Lines. Standard output contains one complete JSON object per
line and no prose. The command returns the `build_finished.summary.exit` value; a recorded failing
action therefore preserves its nonzero status.

Every object has stable schema version `v: 1`, a monotonically increasing `sequence`, a `type`, and
nullable `action` and `result` digests. Additional fields depend on `type`:

| Type | Fields |
|---|---|
| `action_queued` | common fields only |
| `action_started` | `execution.kind` is `local` or `remote`; `execution.worker` names a remote worker or is null |
| `action_hit` | the cached `result` digest |
| `action_done` | `outputs`, each with `path`, `mode`, and `blob` digest |
| `action_failed` | `diagnostics`, each with `file`, `line`, `column`, `severity`, and `message` |
| `critical_path_updated` | `critical_path` entries with action/result digests, duration, and hit state |
| `test_result` | `test` with name, status, output, duration, and hit state |
| `build_finished` | `summary` with exit status, action/hit/failure counts, and duration |

Within an action, queued precedes started and one hit/done/failed event follows. Critical-path
updates follow the terminal states they describe. `build_finished` is always last. A source edit
during the command starts another plan before the final event, and actions unaffected by that edit
are reported as hits.

Example:

```sh
liquid build --events cmake --build build > build-events.jsonl
```

The first wrapper orchestration events have null action/result values. Observed child actions carry
their content digests. Consumers should ignore unknown object members within schema version 1 and
reject an unsupported `v`.
