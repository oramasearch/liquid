# HTTP/2 tests

The HTTP/2 tests exercise frame-envelope validation, HPACK's RFC 7541 Huffman example and
stateful dynamic table, gRPC message and trailer framing, and a flow-controlled socket-level h2c
round trip. `Http2Interop.GetsCapabilitiesFromNativeLinkOverH2c` uses the peer supplied by
`tools/reapi-peers`; it skips with a diagnostic unless `LIQUID_HTTP2_NATIVE_LINK` is set.

TLS framing is covered by the common connection code. A negotiated TLS peer test waits for
Corosio #3 to expose the selected ALPN protocol.
