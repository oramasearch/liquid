# Hot-update demo

`liquid-hot-demo` is a Capy coroutine loop. Every resumption begins with
`liquid_hot_quiescent()`, then calls a stable entry trampoline whose target is registered with the
`liquid-hot` C library. `liquid-hot-demo-patch` exports the replacement leaf definition.
`liquid-hot-demo-restart-hook` records the four arguments liquid appends for a restart outcome.

The integration test starts the loop, observes `value=1`, delivers a signed tier-1 patch through
the local endpoint, and observes `value=2` without restarting the process. A tier-2 check loads an
additive export without an existing entry slot. The tier-3 fixture runs the configured restart hook
instead. Commands and transcripts are in
[`docs/library/hot-update.md`](../../docs/library/hot-update.md).
