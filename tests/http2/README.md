# HTTP/2 tests

The HTTP/2 tests exercise frame-envelope validation, HPACK's RFC 7541 Huffman example and
stateful dynamic table, gRPC message and trailer framing, and a flow-controlled TCP loopback h2c
round trip. `Http2Interop.GetsCapabilitiesFromNativeLinkOverH2c` uses the peer supplied by
`tools/reapi-peers`; it skips with a diagnostic unless `LIQUID_HTTP2_NATIVE_LINK` is set.
The loopback exchange finishes through HTTP/2 GOAWAY so both peers complete their final writes.

`Http2Interop.GetsCapabilitiesFromNativeLinkOverTls` offers `h2` to the configured TLS peer and
requires Corosio to report that selected protocol before it exchanges gRPC frames. It skips with a
diagnostic unless `LIQUID_HTTP2_NATIVE_LINK_TLS` is set.

`test_limits.cpp` (liquid#412) plays a hostile peer over a Capy in-memory stream pair and checks
each denial-of-service limit: the concurrent-stream cap, header block and list sizes, HPACK table
size in both directions, the frame size, flow control and the queued-bytes cap, and the rapid
reset, CONTINUATION, PING, SETTINGS and empty-frame floods. `fuzz_input.hpp` is the one fuzz input
path, shared by the opt-in libFuzzer harness `fuzz_connection.cpp` (`-DLIQUID_BUILD_FUZZERS=ON`)
and a seeded random-frame corpus test that runs in every ctest. See
[`docs/library/http2.md`](../../docs/library/http2.md).

The HTTP/2 source files are compiled into these private test executables. This keeps their symbols
out of the shared library's public ABI while exercising the same source in static and shared builds.
