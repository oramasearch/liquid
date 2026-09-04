# Sandbox implementation

Platform launchers enforce the fixed action environment and isolate network and filesystem access.
The wrap launcher installs `wrap_hook.c` only in the child sandbox: Linux bind-mounts it and injects
`LD_PRELOAD`; macOS permits its path in the generated profile and injects
`DYLD_INSERT_LIBRARIES`. The hook records process identity, parent, executable, normalized working
directory, and argv, then exits a process early when its per-run plan entry is a validated hit.

Windows deliberately returns `errc::unsupported` until its Phase 6 launcher exists.
