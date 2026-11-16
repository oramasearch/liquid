# HTTP/2 and gRPC transport

Issue #155 owns liquid's private HTTP/2 transport. The code is under `src/http2/`; #16 exposes only
the REAPI client and server handles, never a transport type.

## Design

`connection` runs on a Corosio stream wrapped as a Capy `any_stream`. A client sends the HTTP/2
prior-knowledge preface and SETTINGS. A server validates the client preface and sends its own
SETTINGS before acknowledging the client's SETTINGS, as the server connection preface requires.
The connection tracks odd/even stream identifiers, multiplexes frames by stream,
and keeps independent HPACK state in each direction.

Frames supported by the implementation are DATA, HEADERS, CONTINUATION, RST_STREAM, SETTINGS,
PING, GOAWAY, and WINDOW_UPDATE; PRIORITY and unknown frame types are ignored. HEADERS blocks are
split at the peer's advertised frame size. Both connection and stream send windows are decremented
for DATA and restored when a receiver consumes it. A message larger than an advertised stream window
therefore pauses until the peer sends WINDOW_UPDATE rather than exceeding the window.

Every received frame passes through one function, `receive_inbound`, which applies connection
frames and every limit below. Header blocks are reassembled from HEADERS and CONTINUATION and
decoded there, in the order they arrive, then queued for their stream; HPACK state therefore stays
in step with the peer even when the caller reads streams in a different order, or a block belongs to
a stream that was refused or already closed. A stream is forgotten once both directions have ended
and the caller has read everything, or as soon as either side resets it. A server whose response is
complete resets a request still being sent (RST_STREAM NO_ERROR) and drops what it did not read.

`goaway` begins graceful connection shutdown; `wait_for_goaway` receives the peer's connection
shutdown after active streams have completed.

`hpack` implements the RFC 7541 static table, the dynamic table with eviction and size updates,
indexed and literal header fields, never-indexed sensitive fields, and the Appendix B Huffman code.
`grpc` implements the five-byte compressed-flag and big-endian length envelope. Initial gRPC
headers contain `content-type: application/grpc`; trailing headers carry `grpc-status` and
`grpc-message`. A caller maps cancellation and a deadline expiry to RST_STREAM with CANCEL, then
observes the terminal gRPC status.

## Limits on the peer (liquid#412)

The October 2026 security review (finding L-30) found that a peer could exhaust memory: two
settings were applied in the wrong direction, nothing capped streams, header blocks or queued
frames, and floods went unanswered. Each SETTINGS value this endpoint advertises now limits the
peer, and each value the peer advertises limits only what this endpoint sends.

| `connection_options` field | Default | Advertised as | Enforced on the peer |
|---|---|---|---|
| `initial_window_size` | 65,535 | `SETTINGS_INITIAL_WINDOW_SIZE` | DATA beyond a stream's receive window closes the connection with FLOW_CONTROL_ERROR. |
| `max_frame_size` | 16,384 | `SETTINGS_MAX_FRAME_SIZE` | A larger frame closes the connection with FRAME_SIZE_ERROR, whatever the peer's own setting. |
| `header_table_size` | 4,096 | `SETTINGS_HEADER_TABLE_SIZE` | The decoder refuses a dynamic table size update above it (COMPRESSION_ERROR). The peer's value only sets this endpoint's encoder table, capped at this field. |
| `max_concurrent_streams` | 100 | `SETTINGS_MAX_CONCURRENT_STREAMS` | A stream opened past it is refused with RST_STREAM REFUSED_STREAM; its header block is still decoded. |
| `max_header_list_size` | 16 KiB | `SETTINGS_MAX_HEADER_LIST_SIZE` | A compressed block (HEADERS plus CONTINUATION) or a decoded list larger than this closes the connection with ENHANCE_YOUR_CALM. |
| `max_pending_bytes` | 16 MiB | none | Frames queued for streams the caller is not reading past this close the connection with ENHANCE_YOUR_CALM. |

Flow control is honoured in both directions. Received DATA consumes the connection and stream
windows when it arrives, and the credit goes back to the peer (WINDOW_UPDATE) only when the caller
reads that DATA, so unread data is bounded by the windows advertised. At start the connection window
is raised to one stream window more than `max_concurrent_streams` stream windows (bounded by
`max_pending_bytes`): the streams the caller is not reading can never use it all, so a caller that
reads one stream at a time, as `liquid serve reapi` does, cannot deadlock behind them. Padding is
stripped from DATA and counts against the windows.

Floods are counted with token buckets (`flood_limit`: a burst, refilled per second). Spending a
bucket faster than it refills closes the connection with GOAWAY ENHANCE_YOUR_CALM:

| Field | Default | What it counts |
|---|---|---|
| `resets` | 200, 100/s | Streams the peer resets (rapid reset, CVE-2023-44487) and streams refused over the concurrent-stream limit |
| `pings` | 50, 10/s | PING frames that need an acknowledgement |
| `settings` | 50, 10/s | SETTINGS frames that need an acknowledgement |
| `empty_frames` | 100, 50/s | Empty DATA or CONTINUATION without an end flag, PRIORITY, unknown frame types, unsolicited acknowledgements, RST_STREAM on closed streams |

A header block interrupted by any other frame is a PROTOCOL_ERROR (RFC 9113 section 6.10), which
with the block-size cap ends the CONTINUATION flood. Every connection error sends GOAWAY with the
error code and the last peer stream identifier before the connection's operation returns the error;
ENHANCE_YOUR_CALM returns `errc::busy`, the others `errc::corrupt`.

## TLS and h2c

h2c is prior knowledge only. HTTP/1 upgrade is not implemented.

TLS callers first configure Corosio's `tls_context` with `set_alpn({"h2"})`, complete the Corosio
TLS handshake, then call `validate_tls_protocol`. That helper is the one selected-protocol hook.
The vendored Corosio revision exposes the negotiated value through `tls_stream::selected_alpn()`;
the hook requires it to be `h2` before HTTP/2 processes any frames. TLS and h2c therefore share the
same framing layer, while TLS rejects a handshake that selects no application protocol or one other
than `h2`.

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

`tests/http2/test_limits.cpp` plays a hostile peer for each limit above: it writes raw frames into
one end of a Capy in-memory stream pair, runs a server connection on the other until the input ends,
and checks the frames the server wrote back (the REFUSED_STREAM resets, the GOAWAY code) and the
state it kept. It covers the concurrent-stream cap, both directions of the two settings, an
oversize frame, CONTINUATION and empty-CONTINUATION floods, an interrupted header block, indexed
header expansion, rapid reset, stream state being forgotten, PING, SETTINGS and empty-DATA floods,
window refill only on read, DATA beyond the stream window, the queued-bytes cap, and header blocks
decoded in arrival order. `Http2Limits.SurvivesASeededRandomFrameCorpus` runs 2,000 seeded inputs of
random frames through the fuzz harness's entry point on every ctest run.

The libFuzzer harness `tests/http2/fuzz_connection.cpp` is built only on request, with Clang:

```sh
cmake -S . -B build-fuzz -G Ninja -DLIQUID_BUILD_FUZZERS=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-fuzz --target liquid-fuzz-http2
build-fuzz/tests/liquid-fuzz-http2 -max_total_time=60
```

It compiles the HTTP/2 sources with the fuzzer, address and undefined-behaviour sanitizers and
serves each input the way the REAPI server does. Fuzzing liquid's other parsers, and running the
harnesses on a schedule, is liquid#407 (review finding L-25).

`tools/reapi-peers/run-h2c-interop.sh build-linux` is the Docker proof for the Linux CI container.
It starts the NativeLink configuration with host networking, then runs
`Http2Interop.GetsCapabilitiesFromNativeLinkOverH2c`. The test sends the empty protobuf
`GetCapabilitiesRequest` through this layer and requires a gRPC response and `grpc-status: 0`.
The same script starts a one-request private HTTP/2 test server and invokes it through
`grpcurl -plaintext`, proving the server direction.

`tools/reapi-peers/run-tls-interop.sh build-linux` starts NativeLink with the committed
self-signed certificate and offers `h2` from liquid's TLS client. The client requires Corosio to
report `h2` after the handshake before sending `GetCapabilities`; it then requires a gRPC response
and `grpc-status: 0`. The script starts liquid's private server with the same certificate and uses
`grpcurl -insecure` to reach it, which proves server-side ALPN selection and TLS gRPC framing. It
prints a skip diagnostic when Docker is unavailable.

`tools/reapi-peers/run-reapi-interop.sh` extends that proof through the complete #16 client and
server. NativeLink and BuildBarn exercise the client; stock Bazel 7.4.1 exercises strict server
preface ordering, client-streamed ByteStream writes, CAS, and ActionCache. The client recognizes a
trailers-only gRPC response, including an ActionCache miss, without waiting for a DATA frame that
will never arrive. Zero-length DATA frames consume no flow-control credit and do not produce an
invalid zero-increment WINDOW_UPDATE response.
