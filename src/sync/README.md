# Repository sync implementation

`codec.cpp` implements the canonical-CBOR metadata stream and the store-compatible `LQFRAME1`
object frame. Decoders bound lengths, reject trailing or missing end frames, decompress zstd data,
and verify object digests before returning bytes.

`session.cpp` walks references from caller-provided roots. Send asks the peer for its missing set
before uploading; receive range-reads absent objects, verifies each full digest, stores it, and then
continues through its references.

`endpoint.cpp` is the private Corosio and `oramasearch/http` adapter. It parses HTTP/HTTPS repository
URLs, resolves and connects on the caller's Capy executor, supports content-length and chunked
responses, supplies bearer authentication, and composes metadata with the object session for push
and pull. The public types contain no transport implementation types.
