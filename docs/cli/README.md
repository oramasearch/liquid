# liquid command line

`liquid` is a thin command-line client of libliquid. It opens a repository from the current
directory unless a command names another repository. Every library operation runs through
`runtime::block_on`; the CLI does not read repository storage directly.

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
| `pause` | `pause` | Pause working-tree capture. | available |
| `resume` | `resume` | Resume working-tree capture. | available |
| `watch` | `watch` | Capture changes until Ctrl-C. | available |

## Output and errors

`status`, `log`, `refs`, `ops`, `identity show`, and `gc` accept `--json`; `version --json` is
also available for scripts. Text output is line-oriented and stable. A library error always has this
shape and exits with status 1:

```text
error: <code>: <message> (<context>)
```

Invalid command usage exits with status 2 and starts with `error: usage:`.

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
