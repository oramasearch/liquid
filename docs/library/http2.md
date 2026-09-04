# HTTP/2 and gRPC transport

Issue #155 owns liquid's private HTTP/2 transport. The code is under `src/http2/`; no header in
`include/liquid/` exposes it before #16 defines the REAPI surface.

## Design

`connection` runs on a Corosio stream wrapped as a Capy `any_stream`. A client sends the HTTP/2
prior-knowledge preface and SETTINGS; a server validates that preface, accepts SETTINGS, and sends
its own SETTINGS. The connection tracks odd/even stream identifiers, multiplexes frames by stream,
and keeps independent HPACK state in each direction.

Frames supported by the implementation are DATA, HEADERS, CONTINUATION, RST_STREAM, SETTINGS,
PING, GOAWAY, and WINDOW_UPDATE. HEADERS blocks are split and reassembled at the peer's advertised
frame size. Both connection and stream send windows are decremented for DATA and restored when a
receiver consumes it. A message larger than an advertised stream window therefore pauses until the
peer sends WINDOW_UPDATE rather than exceeding the window.

`goaway` begins graceful connection shutdown; `wait_for_goaway` receives the peer's connection
shutdown after active streams have completed.

`hpack` implements the RFC 7541 static table, the dynamic table with eviction and size updates,
indexed and literal header fields, never-indexed sensitive fields, and the Appendix B Huffman code.
`grpc` implements the five-byte compressed-flag and big-endian length envelope. Initial gRPC
headers contain `content-type: application/grpc`; trailing headers carry `grpc-status` and
`grpc-message`. A caller maps cancellation and a deadline expiry to RST_STREAM with CANCEL, then
observes the terminal gRPC status.

## TLS and h2c

h2c is prior knowledge only. HTTP/1 upgrade is not implemented.

TLS callers first configure Corosio's `tls_context` with `set_alpn({"h2"})`, complete the Corosio
TLS handshake, then call `validate_tls_protocol`. That helper is the one selected-protocol hook.
Corosio #3 has not yet exposed its negotiated ALPN value, so the hook currently returns empty and
accepts an endpoint explicitly configured for `h2`. Once the vendored Corosio revision includes
#3, the same helper will require the selected value to be `h2`; no HTTP/2 framing change is needed.
TLS interoperability is deferred to that Corosio update. The current phase interoperability proof
uses plaintext h2c.

## Limits

There is no HTTP/1 upgrade, server push, or priority scheduling. The layer supports no opaque HTTP
semantics: it is the private gRPC and future REAPI transport foundation. It does not introduce
`grpc++`, protobuf, a code generator, or a raw network thread.

## Coroutine rules

Every connection operation is a lazy `runtime::task<T>` and receives frame-owned values. Corosio
objects remain under `src/` and all socket progress happens through Capy awaitables. Callers await
operations in their existing coroutine; `runtime::block_on` is only for synchronous entry points
and is never used from a connection coroutine.

## Verification

The focused portable test starts a Corosio loopback h2c client and server, sends a 4 KiB body over
a 1 KiB advertised stream window, exchanges gRPC-style headers and trailers, and exercises HPACK
Huffman and dynamic-table state:

```sh
build/tests/liquid-tests --gtest_filter='Http2*:*Grpc*'
```

`tools/reapi-peers/run-h2c-interop.sh build-linux` is the Docker proof for the Linux CI container.
It starts the NativeLink configuration with host networking, then runs
`Http2Interop.GetsCapabilitiesFromNativeLinkOverH2c`. The test sends the empty protobuf
`GetCapabilitiesRequest` through this layer and requires a gRPC response and `grpc-status: 0`.
The same script starts a one-request private HTTP/2 test server and invokes it through
`grpcurl -plaintext`, proving the server direction. It prints a skip diagnostic when Docker is
unavailable. TLS peer coverage waits for Corosio #3.
