# Server API reference

These pages map the network capabilities exposed by liquid server components. The image-admission
reference includes named webhook routes and the versioned decision stream. Other wire schemas
remain private implementation types; stable REAPI surfaces use the upstream v2 protocol, and
liquid's worker RPCs are documented as an internal protocol.

| Component | Reference |
|---|---|
| Image admission | [`admission.md`](admission.md), including policy-pinned transparency outcomes |
| Scheduler | [`scheduler.md`](scheduler.md) |
| Worker control listener | [`worker.md`](worker.md) |
| Node store | [`node-store.md`](node-store.md) |
