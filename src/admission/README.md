# Admission implementation

`policy.cpp` owns the strict version-1 JSON codec. `review.cpp` is the one-image decision path used
by the webhook and verification surfaces. `source.cpp` implements local-store lookup, node-store
HTTP lookup, and the checkpoint-caching log verifier. `server.cpp` owns AdmissionReview parsing,
policy reload, metrics, REST, OpenAPI, MCP, and the TLS or explicitly insecure listener.

All listener, source, and lake work is coroutine-native. Blocking policy and checkpoint file access
runs through `runtime::offload`.
