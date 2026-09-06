# `liquid drive`

`liquid drive sync <folder> <remote>` opens or creates an ordinary repository in `folder`, stores
the remote URL in repository metadata, performs one three-tree reconciliation, pulls remote
changes, and pushes the result. `LIQUID_TOKEN` supplies an optional bearer credential and is not
persisted or printed.

Add `--watch` to perform the initial synchronization and attach to the folder daemon. The daemon's
capture watcher wakes the drive target after filesystem drips; periodic capture recovery and each
delivery use a full filesystem walk. The spawned daemon inherits `LIQUID_TOKEN` and passes it to
its in-memory drive client without saving the credential.

`liquid drive pin <path>` locates the containing configured drive, persists an
available-offline rule, and hydrates the file. A directory path hydrates all descendants when the
repository's folder-pin policy is enabled. `liquid drive unpin <path>` writes an explicit
online-only rule; it does not immediately evict already hydrated chunks.

`liquid drive status [folder]` opens a previously configured folder without contacting its remote.
It prints the three tree digests, pull cursor, interrupted flag, file and walk counts, measured
state bytes, configured memory ceiling, pending uploads, hydration debt, last offline reason,
per-path state reasons, conflict paths, and the last per-reference outcomes. A
refusal includes the server reason. One-shot sync exits with status 1 if any reference was refused,
2 for invalid usage, and 0 after all references apply.

See [drive synchronization](../library/drive.md) for reconciliation and resume behavior.
