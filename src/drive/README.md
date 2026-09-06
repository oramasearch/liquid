# Drive client implementation

The client keeps the synchronized folder as an ordinary repository and creates one remote shadow
repository in the same object store. Repository metadata contains the last remote, local, and
mutually synchronized tree digests, the pull cursor, interruption marker, bounded-state counters,
and the latest per-reference outcomes.

Every reconciliation starts with a full filesystem walk. Watcher events only request another run,
so overflow and coalescing cannot make the stored tree authoritative over disk. Directory walks and
publication syscalls use `runtime::offload`; file contents enter and leave the store one chunk at a
time. The in-memory path index refuses to exceed the configured ceiling.

The three-tree query keys established files by `node_id`, combines a move on one side with an edit
on the other, retains both content records for concurrent edits, and gives the remote value a
deterministic visible conflict name. Transfer interruption is persisted before network activity;
the existing missing-object negotiation makes the next run resume without republishing partial
metadata.
