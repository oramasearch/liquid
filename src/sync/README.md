# Repository sync implementation

`codec.cpp` implements the canonical-CBOR metadata stream, optional v1 atomic request and per-ref
outcome fields, and the store-compatible `LQFRAME1` object frame. Decoders bound lengths, reject
trailing or missing end frames, decompress zstd data, and verify object digests before returning
bytes.

`session.cpp` walks references from caller-provided roots. Send asks the peer for its missing set
before uploading; receive range-reads absent objects, verifies each full digest, stores it, and then
continues through its references while preserving each transmitted digest algorithm.

`server.cpp` implements the public transport-neutral `sync_server`. It owns the private codec,
checks repository/store pairing, derives old/new ref updates and fast-forward state from the change
graph, associates new reachability with change callbacks, accumulates named verdicts, imports the
accepted ref set, applies the optional atomic rule, exports pull pages, answers missing-object
queries and logical range reads, verifies pack uploads, and encodes canonical error bodies.

`endpoint.cpp` is the repository-protocol adapter over the shared private HTTP client in
`src/net/http_client.*`. The client owns URL parsing, Corosio connections, TLS, `oramasearch/http`
framing, content-length and chunked responses, and bearer authentication. The sync adapter owns its
media type, CBOR errors, endpoint paths, legacy push outcomes, and object-session composition. The
public types contain no transport implementation types.
