# Private network request layer

`http_client.*` provides URL decomposition and protocol-neutral request and response values for
private clients. Its only transport is `src/http1/client.cpp`, which owns Corosio connection, TLS
peer verification, HTTP/1.1 framing, and chunked transfer decoding. Forge builds its JSON and Lake
envelope mapping over these request values; sealed secrets use them for their public-key lookup.
A response carries its header fields as well as its status and body.

`retry_after.*` reads a `Retry-After` value (delay-seconds or any of the three HTTP-date forms of
RFC 9110) into the wait it asks for. Repository sync uses it to retry a `503` or `429` that
carries the header ([`docs/library/sync.md`](../../docs/library/sync.md)).

`listener.cpp` implements the public `liquid/listener.hpp` checks shared by the REAPI and sync
listeners: loopback detection through Corosio's address parsers, the rule that a non-loopback
listener needs TLS plus a bearer token or a client CA, owner-only token files, and the
constant-time bearer comparison (SHA-256 of both tokens, compared without an early exit).

Public headers expose only Capy tasks and liquid values. Boost.HTTP and Corosio remain private.
