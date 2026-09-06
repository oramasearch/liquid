# Private network client

`http_client.hpp` owns liquid's reusable Corosio HTTP/1.1 request path: URL parsing, executor-bound
resolution and connection, TLS peer verification, bearer headers, response framing, chunked
decoding, and lake JSON envelope errors. It is private to libliquid; public headers expose only
Capy tasks and liquid values.
