# Private network request layer

`http_client.*` provides URL decomposition and protocol-neutral request and response values for
private clients. Its only transport is `src/http1/client.cpp`, which owns Corosio connection, TLS
peer verification, HTTP/1.1 framing, and chunked transfer decoding. Forge builds its JSON and Lake
envelope mapping over these request values; sealed secrets use them for their public-key lookup.

Public headers expose only Capy tasks and liquid values. Boost.HTTP and Corosio remain private.
