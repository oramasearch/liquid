# Admission tests

`test_review.cpp` isolates policy and CBOR/JSON decision encoding plus every one-image decision with
injected evidence and log doubles. `test_server.cpp` mints and verifies an image statement through
the built CLI, starts the TLS and explicitly insecure admission listeners, posts Kubernetes
fixtures, exercises named routing, automatic atomic reload, invalid-file retention, structured
status, bounded decision replay, REST, readiness, metrics, and MCP. A separate process case proves
that an unreachable node-store source leaves readiness false and refuses admission.
