# Private HTTP/1.1 support

`client.cpp` is the one coroutine HTTP/1.1 client used by repository sync, the forge JSON layer,
remote admission attestation lookup, and transparency-checkpoint retrieval. It resolves names
through Corosio and supports verified HTTPS without exposing networking types in public headers.
`SSL_CERT_FILE` selects an explicit PEM trust bundle, every certificate in it trusted; otherwise it
uses OpenSSL's default trust store (the paths the linked OpenSSL was built with, which also honour
`SSL_CERT_DIR`). A failed handshake reports OpenSSL's reason, for example
`certificate verify failed: unable to get local issuer certificate`. Until corosio applied the
default store (liquid#368) every verified HTTPS request failed, rendered on macOS as
`unspecified system_category error`; `tests/test_forge.cpp` covers the default store, a bundle,
and the failure message.

`server.cpp` adapts accepted Corosio sockets, optionally through `openssl_stream`, to the vendored
`oramasearch/http` parser and router. Admission and worker control listeners use this adapter, so
request coroutines remain on the listener executor.
