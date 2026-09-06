# Daemon internals

`fanout.cpp` owns ordered, at-least-once drip delivery and persists each target's successful cursor.
It also projects a target-supplied success detail beside cursor, delivery, and error status. The
built-in `hot-update` target uses that field for its latest outcome, tier, and record digest.

The public lifecycle and target interface are implemented in `src/daemon.cpp` and documented in
[`docs/library/incremental-engine.md`](../../docs/library/incremental-engine.md).

Presence reports are transient daemon events. The fan-out target receives local session and client
updates without writing a drip or advancing a persisted cursor.
