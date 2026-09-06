# Sandbox implementation

Platform launchers enforce the fixed action environment and isolate network and filesystem access.
Ordinary build and wrap requests deny network access. PostgreSQL data lifecycle requests and wraps
whose fixed environment contains a repository-resolved session branch use the same launcher with
an explicit network capability; callers cannot enable it through an ordinary `action_input`.
The wrap launcher installs `wrap_hook.c` only in the child sandbox: Linux bind-mounts it and injects
`LD_PRELOAD`; macOS permits its path in the generated profile and injects
`DYLD_INSERT_LIBRARIES`. The hook records process identity, parent, executable, normalized working
directory, and argv, then exits a process early when its per-run plan entry is a validated hit.
Conditional hit rows also carry the size and byte guard for previously invalidated inputs. The
hook checks those inputs in the mounted working tree at process entry, after their producers have
finished, and executes the process if any guard differs.
The hook also records monotonic entry and exit values used for each wrapped action's execution
phase and Chrome trace offset. A validated hit records its exit value before terminating early.
Verification launches the recorded working directory in a newly materialised input snapshot and
captures stdout and stderr separately for digest comparison.
The fixed action environment includes the recorded source epoch as `SOURCE_DATE_EPOCH` and
`ZERO_AR_DATE=1`; wrap uses epoch zero. Sandbox record version 2 also carries the output-normaliser
rules, so changing a collection rule changes the sandbox digest used by cache identity.
Local action launch also carries the coroutine stop token. A cancelled local/remote race kills the
Linux sandbox process group, waits for it, unmounts its input view, and removes the run directory;
only the scheduler's winner can reach result publication.

Linux exposes `lib` and `lib64` aliases only when those directories exist in the declared
toolchain. The dynamic loader and libraries remain content-addressed toolchain inputs; the read
guard permits those aliases and the optional loader-cache lookup inside the otherwise isolated
sandbox root.

Windows deliberately returns `errc::unsupported` until its Phase 6 launcher exists.

On Linux, a remote-eligible action is prefixed with the static `read_guard.c` tracer inside the
sandbox. It follows child processes, refuses file-open calls outside the action input and toolchain
roots, and returns the first normalized path through the launch result.
