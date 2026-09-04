# HTTP/2 tests

The HTTP/2 tests exercise frame-envelope validation, HPACK's RFC 7541 Huffman example and
stateful dynamic table, gRPC message and trailer framing, and a flow-controlled TCP loopback h2c
round trip. `Http2Interop.GetsCapabilitiesFromNativeLinkOverH2c` uses the peer supplied by
`tools/reapi-peers`; it skips with a diagnostic unless `LIQUID_HTTP2_NATIVE_LINK` is set.
The loopback exchange finishes through HTTP/2 GOAWAY so both peers complete their final writes.

TLS framing is covered by the common connection code. A negotiated TLS peer test waits for
Corosio #3 to expose the selected ALPN protocol.

The HTTP/2 source files are compiled into these private test executables. This keeps their symbols
out of the shared library's public ABI while exercising the same source in static and shared builds.
