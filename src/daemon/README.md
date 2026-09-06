# Daemon internals

`fanout.cpp` owns ordered, at-least-once drip delivery and persists each target's successful cursor.
It also projects a target-supplied success detail beside cursor, delivery, and error status. The
built-in `hot-update` target uses that field for its latest outcome, tier, and record digest.

The public lifecycle and target interface are implemented in `src/daemon.cpp` and documented in
[`docs/library/incremental-engine.md`](../../docs/library/incremental-engine.md).

Presence reports are transient daemon events. The fan-out target receives local session and client
updates without writing a drip or advancing a persisted cursor.

Lock and unlock requests open the checkout through the public repository API, apply an optional
connected authority callback, and send the complete current lock set to every fan-out target.
Lock snapshots do not change target drip cursors.
The daemon request protocol also carries the public per-line attribution query. It validates the
path and change, delegates history reconstruction to `repository::attribution`, and returns a
versioned canonical-CBOR line array with actor, session, change, and agent fields.
