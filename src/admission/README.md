# Admission implementation

`policy.cpp` owns the strict version-1 JSON policy codec. `decision.cpp` owns the public canonical
CBOR and JSON decision codecs. `review.cpp` is the one-image decision path used by the webhook and
verification surfaces. `source.cpp` implements local-store lookup, node-store HTTP lookup, and the
shared-verifier Lake adapter with policy-pinned checkpoint and witness trust. `server.cpp` owns
AdmissionReview parsing, named policy routing, atomic directory reload, bounded decision replay,
metrics, REST, OpenAPI, MCP, and the TLS or explicitly insecure listener.

All listener, reload, stream, source, and lake work is coroutine-native. Blocking policy and
checkpoint file access runs through `runtime::offload`. The Lake adapter checks the signed body's
`issued_at` after proof verification on every request, with a clock injected through the private
factory for deterministic tests. Positive age bounds fail closed with `checkpoint stale` for
missing, expired, or excessively future-dated time. Policy decoding rejects zero; omission is
represented by an empty optional and disables freshness enforcement.
