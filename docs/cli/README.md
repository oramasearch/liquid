# liquid command line

`liquid` is a thin command-line client of libliquid. Repository commands open from the current
directory unless a command names another repository; `wrap` instead snapshots the current working
tree into its project cache. Every library operation runs through `runtime::block_on`; the CLI does
not read repository storage directly.

`init` imports regular files already present in its target directory through
`repository::write_file`. Subsequent external edits need the watcher-backed capture service from
liquid #5. `status` opens capture while it gathers its report, and includes capture telemetry.

## Commands

This table is generated from the command table in `src/main.cpp`. The `cli-help-docs` test checks
that the table and `liquid --help` name the same commands.

| Command | Usage | Description | Availability |
|---|---|---|---|
| `version` | `version [--json]` | Print the library version. | available |
| `init` | `init [path]` | Create a repository and import its regular files. | available |
| `status` | `status [--json]` | Show repository status. | available |
| `log` | `log [--json]` | Show change history. | available |
| `cut` | `cut <message>` | Cut the working change. | available |
| `refs` | `refs [--json]` | List repository references. | available |
| `move` | `move <from> <to>` | Record a path move. | available |
| `rm` | `rm <path>` | Record a path removal. | available |
| `nest` | `nest <repository> <path>` | Nest another repository. | available |
| `split` | `split <path> <into>` | Split a subtree into a repository. | available |
| `combine` | `combine <repository> [--at <path>]` | Combine another repository. | available |
| `ops` | `ops [--json]` | List recorded operations, newest first. | available |
| `undo` | `undo [op-id]` | Undo the newest or named operation. | available |
| `lock` | `lock <path>` | Advisory-lock a path. | available |
| `unlock` | `unlock <path>` | Remove an advisory lock. | available |
| `mount` | `mount <mountpoint> [--read-only]` | Mount a lazy working tree until Ctrl-C. | available where FUSE is available |
| `materialise` | `materialise <target> [path]` | Write a full working tree. | available |
| `gc` | `gc [--dry-run] [--json] [--explain] [--compact] [--compact-below <fraction>] [repository]` | Checkpoint and collect a repository. | available |
| `identity` | `identity [show\|rotate] [--json]` | Show or rotate the device identity. | available |
| `attest` | `attest <show\|verify> <digest>` | Show or verify an attestation in the current repository or capture store. | available |
| `verify` | `verify <action\|--all\|--changed> [--explain]` | Re-execute and compare cached actions. | available on Linux with FUSE and on macOS with macFUSE |
| `wrap` | `wrap [--jobs <count>] [--no-cache] [--no-verify] [--explain] <command> [args...]` | Run an existing build as cached actions. | available on Linux with FUSE and on macOS with macFUSE |
| `build` | `build --profile <command> [args...] \| build trace [--chrome]` | Profile a build or export its trace. | available on Linux with FUSE and on macOS with macFUSE |
| `provenance` | `provenance [--dsse] <result\|path>` | Print and verify build provenance. | available |
| `why` | `why <target> [--json]` | Explain the latest cache lookup for an output, action, or result. | available |
| `why-slow` | `why-slow <target>` | Show the critical path to a build target. | available |
| `cache` | `cache stats [--json]` | Show build-cache statistics. | available |
| `policy` | `policy <check\|explain> [target]` | Check or explain build-input policy. | available |
| `pause` | `pause` | Pause working-tree capture. | available |
| `resume` | `resume` | Resume working-tree capture. | available |
| `watch` | `watch` | Capture changes until Ctrl-C. | available |

## Output and errors

`status`, `log`, `refs`, `ops`, `identity show`, `gc`, `why`, and `cache stats` accept `--json`;
`version --json` is also available for scripts. Text output is line-oriented and stable. A library
error always has this shape and exits with status 1:

```text
error: <code>: <message> (<context>)
```

Invalid command usage exits with status 2 and starts with `error: usage:`.

`wrap` prints `actions`, cache `hits` and `misses`, started output-producing processes, and the
outer action's status. `--explain` adds one `miss:` line for each changed manifest path. `--jobs`
sets driver parallelism and the compilation-database seed width; `--no-cache` executes and records
the observation without publishing or serving cache entries. The persistent project cache defaults
to the platform cache directory and can be relocated with `LIQUID_WRAP_CACHE`. On completion,
`wrap` schedules the configured local verification sample on the shared runtime; `--no-verify`
skips that background work.

`verify` opens the current project's wrap cache. A digest selects one action, `--all` selects every
cached result, and `--changed` selects results without a recorded verification. Each line names the
action, result, signed attestation, and agreement. Divergence prints a `critical:` line naming every
differing output and exits with status 1; `--explain` adds the first differing byte and its values.

`provenance` accepts a result digest or produced path in a liquid repository. In a wrapped project,
a produced path opens that project's persistent cache. It verifies the result and attestation before
writing JSON. `--dsse` writes an in-toto Statement v1 inside a DSSE-compatible envelope, projected
from the same canonical TAAP record.
`wrap` prints `actions`, cache `hits` and `misses`, started output-producing processes, the outer
action's status, the disk hit rate, bytes transferred, bytes avoided, bytes rebuilt, and miss counts
by reason. Its observability line names wall and critical-path durations, achieved parallelism, the
totals for queued, sandbox, exec, hash, and store phases, bytes served versus rebuilt, and the
persisted trace digest. Following `critical_action` lines are the longest dependent chain, not a
list sorted only by individual duration. `--explain` adds the
recorded cause for every miss. Input changes carry the path, old and new digests, a line hunk for
UTF-8 text, and the repository drip's digest, TAAP actor, and time when the path is tracked. Command
changes print an indexed argv diff; environment changes name the key; toolchain and sandbox changes
print their old and new tree digests, with the changed tool path when its tree entry can be resolved.
`--jobs` sets driver parallelism and the compilation-database seed width; `--no-cache` executes and
records the observation without publishing or serving cache entries. The persistent project cache
defaults to the platform cache directory and can be relocated with `LIQUID_WRAP_CACHE`.

`liquid build --profile <command> [args...]` is the profiled build spelling of `wrap`; every wrap is
profiled, so existing `liquid wrap` callers receive the same stored record and summary. `liquid
build trace` describes the latest trace retained by the project's cache root. `--chrome` writes a
Chrome trace-event JSON object with one complete event for every action phase. The export reads only
the recorded trace and result graph.

`liquid why-slow <target>` accepts an output path, action digest, or result digest from the latest
trace. It prints the longest dependency path ending at that action, followed by queued, sandbox,
exec, hash, and store durations for every action. When both an inner tool action and the outer driver
record the target, the command selects the inner action with the narrower output set.

`liquid why <target>` accepts an output path, an action digest, or a result digest. It selects the
most recent recorded lookup for that target and compares only the action, result, manifest, input
tree, toolchain tree, sandbox record, and repository history named by that lookup. It never inspects
an unrecorded checkout path to invent a cause. If a local lookup journal is absent, as on a machine
that imported remote cache records, it uses action-index sequence and the signed result manifests.
An absent target is `never built`; incomplete or ambiguous records are `unknown` and retain the
known action and result identifiers. A divergence explanation is emitted only for the authoritative
action-index flag written by `liquid verify`, with its verification attestation digest when recorded.
Current input policy is checked first for a recorded action. An unsatisfied refuse rule is
`policy refused` and names its first failing input and rule even if the previous lookup was a hit.
`--json` emits the same fields, including nullable digest, line-change, attribution, environment,
and tool fields.

`liquid cache stats` reports cumulative completed-wrap counts. Disk is the only implemented tier;
its hit rate and bytes avoided are reported, while memory and remote say `unavailable` and remote
bytes transferred remain zero. Bytes avoided and rebuilt are sums of recorded output blob sizes.
The report also groups misses by the same reason vocabulary as `why`.

`policy check <target>` evaluates the current repository's `config.policy` against a cached action,
result, or produced path without executing it. It exits non-zero when a `refuse` rule fails.
`policy explain` prints approved builders, rules, and the policy digest; an optional target adds
every selected input, accepted signer count, and satisfaction result. Warning rules print
`warning:` lines and exit successfully. Both commands open the current liquid repository or the
current project's wrap repository.

## Examples

```sh
liquid version --json
liquid init .
liquid status --json
liquid log --json
liquid cut "import existing files"
liquid refs --json
liquid move notes.txt archive/notes.txt
liquid rm archive/notes.txt
liquid nest ../shared-assets vendor/assets
liquid split src ../src-repository
liquid combine ../src-repository --at imported-src
liquid ops --json
liquid undo
liquid lock src/main.cpp
liquid unlock src/main.cpp
liquid mount /tmp/liquid-mount --read-only
liquid materialise /tmp/liquid-checkout src
liquid gc --dry-run .
liquid identity rotate
liquid attest show blake3:<attestation-digest>
liquid attest verify blake3:<attestation-digest>
liquid verify --all
liquid verify --changed --explain
liquid provenance blake3:<result-digest>
liquid provenance --dsse build/output.o
liquid policy check build/output.o
liquid policy explain build/output.o
liquid wrap cmake -S . -B build -G Ninja
liquid wrap --jobs 8 --explain cmake --build build
liquid build --profile cmake --build build
liquid build trace --chrome > build-trace.json
liquid why build/CMakeFiles/app.dir/main.cpp.o
liquid why blake3:<action-or-result-digest> --json
liquid why-slow build/app
liquid cache stats --json
liquid pause
liquid resume
liquid watch
```

## How to verify

Run the static and shared build proof from the repository root. The `cli-scenario` CTest creates
two repositories, imports files, cuts changes, moves a path, nests a repository, splits a subtree,
combines it, reads JSON history and operations, undoes the move, verifies a locked mutation is
refused, pauses and resumes capture, collects the repository, and reads the retained files back.
`cli-help-docs` compares the
documented command names to `--help`.

## How to change

Add a command row to the table in `src/main.cpp`, put its implementation in the matching
`src/cli/*.cpp` command group, and update this page's generated command table and example. Keep
each command on public libliquid calls; a library capability that does not exist belongs in the
library before it reaches the CLI.
