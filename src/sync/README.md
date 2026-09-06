# Repository sync implementation

`codec.cpp` implements the canonical-CBOR metadata stream, optional v1 atomic request and per-ref
outcome fields, and the store-compatible `LQFRAME1` object frame. Decoders bound lengths, reject
trailing or missing end frames, decompress zstd data, and verify object digests before returning
bytes.

`session.cpp` walks references from caller-provided roots. Send asks the peer for its missing set
before uploading; receive range-reads absent objects, verifies each full digest, stores it, and then
continues through its references while preserving each transmitted digest algorithm.
`receive_metadata` follows only tree and blob records, registers each blob's whole-content digest
against the session's source and instance, and counts bytes whose chunks remain remote without
requesting any content chunk.

`server.cpp` implements the public transport-neutral `sync_server`. It owns the private codec,
checks repository/store pairing, derives old/new ref updates and fast-forward state from the change
graph, associates new reachability with change callbacks, accumulates named verdicts, imports the
accepted ref set, applies the optional atomic rule, exports pull pages, answers missing-object
queries and logical range reads, verifies pack uploads, applies per-author flow limits before
metadata import, and encodes canonical error bodies including retry scope and delay.

`endpoint.cpp` is the repository-protocol adapter over the private HTTP/1.1 client in
`src/http1/client.cpp`. The client owns URL parsing, Corosio connections, TLS, request and response
framing, and bearer authentication. The sync adapter owns its media type, CBOR errors, endpoint
paths, legacy push outcomes, and object-session composition. HTTP 429 responses become explicit
retry fields in the public sync report. The public types contain no transport implementation types.
