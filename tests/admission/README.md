# Admission tests

`test_review.cpp` isolates policy and CBOR/JSON decision encoding plus every one-image decision with
injected evidence and log doubles. `test_log.cpp` runs the admission adapter against the existing
Lake transparency fixture: the valid checkpoint, parent tiles, and inclusion proof pass; a forged
parent tile and an untrusted checkpoint fail with exact reasons. The legacy fixture carries no
issuance time, so its positive proof control uses an omitted age bound; the reviewer regression
sets a one-second bound and requires refusal. `test_server.cpp` mints and
verifies an image statement through the built CLI, starts the TLS and explicitly insecure admission
listeners, posts Kubernetes fixtures, exercises named routing, automatic atomic reload,
invalid-file retention, structured status, bounded decision replay, REST, readiness, metrics, and
MCP. A separate process case proves that an unreachable node-store source leaves readiness false
and refuses admission.

Issue #290 adds the reviewer's missing-time regression in `test_log.cpp`, deterministic clock
coverage for authenticated checkpoint age, and `checkpoint_fixture.hpp` to sign controlled bodies
with the existing checkpoint format. The process test serves a one-entry signed log over local
HTTP and posts to `/admit/default`: fresh evidence passes, the same checkpoint expires and remains
refused when replayed, stricter reloads apply to cached evidence, zero-bound reloads retain the
previous policy, and excessive future skew fails closed. The log double uses Python's standard
HTTP server and the real CLI mints the image attestation and serves admission.
