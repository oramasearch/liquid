# Admission tests

`test_review.cpp` isolates policy encoding and every one-image decision with injected evidence and
log doubles. `test_server.cpp` mints and verifies an image statement through the built CLI, starts
the TLS and explicitly insecure admission listeners, posts Kubernetes fixtures, reloads policy,
and checks REST, readiness, metrics, and MCP. A separate process case proves that an unreachable
node-store source leaves readiness false and refuses admission.
