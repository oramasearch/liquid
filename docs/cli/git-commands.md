# liquid and Git's commands

liquid is the next generation of Git, and a person who knows Git should be able to guess its
commands. **Where Git has a command for the same thing, liquid uses Git's name, arguments and
order.** A different name needs a reason, and the reason is recorded on this page.

Steve, 2026-10-12: *"liquid should really not deviate from git commands without a good reason
(e.g. git mv vs liquid move)"*.

## The same as Git

| Git | liquid | Notes |
|---|---|---|
| `git init` | `liquid init` | Also imports the folder's regular files. |
| `git clone` | `liquid clone` | |
| `git status` | `liquid status` | |
| `git log` | `liquid log` | |
| `git diff` | `liquid diff` | Adds `--structural` and `--formats`. |
| `git blame` | `liquid blame` | Attributes each line to a session and a change. |
| `git mv` | `liquid mv` | Was `move` before v0.1.0; `move` is still accepted. |
| `git rm` | `liquid rm` | |
| `git push` | `liquid push` | |
| `git pull` | `liquid pull` | |
| `git gc` | `liquid gc` | `--explain` says why a chunk is kept. |
| `git repack` | `liquid repack` | |
| `git version` | `liquid version` | |
| `git lfs lock`, `unlock` | `liquid lock`, `liquid unlock` | Locks are part of liquid, not an extension. |

## Different on purpose

Typing one of the Git names in the first column prints the liquid command to use, where the
table says *hint*.

| Git | liquid | Why it differs |
|---|---|---|
| `git add` | none (*hint*) | There is no staging area. Every write is captured as a drip. |
| `git commit` | `liquid cut <message>` (*hint*) | A change is a named group of drips that already exist. Nothing is created at that moment except the name, so calling it a commit would describe the wrong thing. The project's vocabulary reserves "commit" for Git. |
| `git grep` | `liquid search` (*hint*) | Search ranks results from an index and also takes a substring or a regular expression. It is not a line-by-line scan. |
| `git reflog`, `git reset` | `liquid ops`, `liquid undo` (*hint* on `reflog`) | liquid records operations, not reference movements, and undoes the newest, a named one, or a whole session's. |
| `git submodule` | `liquid nest` (*hint*) | Nesting is part of liquid's object model, recorded as an operation like any other write. |
| `git subtree split` | `liquid split` | The split repository owns its chunks and keeps its signed history. |
| `git merge --allow-unrelated-histories` | `liquid combine` | Combining two repositories is its own operation with its own record. |
| `git mergetool` | `liquid conflicts`, `liquid conflict`, `liquid resolve` (*hint*) | A conflict is a recorded value, listed and resolved by id, not markers in a file. |
| `git restore` | `liquid history <path> --restore <change>` (*hint*) | Restore is one use of a path's history, which follows the path across moves. |
| `git branch` | `liquid refs` (*hint*) | liquid has references. `refs` lists them. |
| `git fast-import`, `git fast-export` | `liquid import git`, `liquid export git` | `import` and `export` also handle local trees and Spack universes. |

## Open: the same name with a different meaning

These are not renames, but a person who knows Git will guess wrong. They are recorded here so
the decision is made deliberately. None is changed in v0.1.0.

| liquid | What Git's command of that name does | What liquid's does |
|---|---|---|
| `liquid config` | Reads and writes options. | Checks, lowers and formats `.liquid` configuration files. |
| `liquid daemon` | Serves repositories over the network. | Starts or stops the checkout daemon that captures writes. Serving is `liquid serve`. |
| `liquid verify` | (`git verify-commit` checks a signature.) | Re-executes cached build actions and compares their outputs. |

## Open: Git commands with no liquid command yet

`checkout` and `switch`, `branch` and `tag` creation, `merge`, `rebase`, `fetch`, `remote`,
`stash`, `show`, `revert` and `cherry-pick`. `liquid materialise` writes a working tree and
`liquid mount` mounts one lazily, which covers part of what `checkout` is used for. When one of
these is added, it takes Git's name.

## How the rule is kept

- **A new command** that does what a Git command does takes Git's name. If it must differ, add a
  row to *Different on purpose* with the reason, and a hint in `git_hints` in `src/main.cpp` so
  the Git name points at it.
- **`liquid --help`** lists Git's own commands first, in the *Start here* block and the first
  group.
- **`cli-help-docs`** checks that `mv` is the command, that `move` is still accepted, and that
  `commit`, `add`, `grep` and `reflog` each print their hint.
