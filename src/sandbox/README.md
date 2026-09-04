# Sandbox implementation

Platform launchers enforce the fixed action environment and isolate network and filesystem access.
The wrap launcher installs `wrap_hook.c` only in the child sandbox: Linux bind-mounts it and injects
`LD_PRELOAD`; macOS permits its path in the generated profile and injects
`DYLD_INSERT_LIBRARIES`. The hook records process identity, parent, executable, normalized working
directory, and argv, then exits a process early when its per-run plan entry is a validated hit.
The hook also records monotonic entry and exit values used for each wrapped action's execution
phase and Chrome trace offset. A validated hit records its exit value before terminating early.
Verification launches the recorded working directory in a newly materialised input snapshot and
captures stdout and stderr separately for digest comparison.

Windows deliberately returns `errc::unsupported` until its Phase 6 launcher exists.

On Linux, a remote-eligible action is prefixed with the static `read_guard.c` tracer inside the
sandbox. It follows child processes, refuses file-open calls outside the action input and toolchain
roots, and returns the first normalized path through the launch result.
