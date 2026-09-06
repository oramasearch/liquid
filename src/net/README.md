# Private HTTP client

`http_client.*` owns liquid's reusable HTTP/1.1 client transport: URL parsing, request framing,
Corosio connection and TLS setup, response framing, and chunked transfer decoding. Protocol clients
remain responsible for their media types and response envelopes. Public APIs expose Capy tasks and
standard liquid values; Boost.HTTP and Corosio stay behind this private boundary.
