# Private HTTP/1.1 support

`client.cpp` is the one coroutine HTTP/1.1 client used by repository sync, remote admission
attestation lookup, and transparency-checkpoint retrieval. It resolves names through Corosio and
supports verified HTTPS without exposing networking types in public headers.

`server.cpp` adapts accepted Corosio sockets, optionally through `openssl_stream`, to the vendored
`oramasearch/http` parser and router. Admission and worker control listeners use this adapter, so
request coroutines remain on the listener executor.
