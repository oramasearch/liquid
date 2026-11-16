# HTTP/2 implementation

This private implementation owns HTTP/2 framing, HPACK, and gRPC message framing. It is used only
inside libliquid until issue #16 defines the public REAPI surface.

`hpack.*` implements RFC 7541 static and dynamic tables plus Huffman strings. `frame.*` validates
the RFC 9113 wire frame envelope. `grpc.*` implements gRPC's five-byte message envelope and
trailer status parsing. Connection I/O will remain Corosio-only through `runtime/net.hpp`.

`connection.*` enforces what it advertises: concurrent streams, header list and table sizes, frame
size, flow control and a cap on bytes queued for unread streams, and it closes the connection with
GOAWAY ENHANCE_YOUR_CALM on rapid reset, CONTINUATION, PING, SETTINGS and empty-frame floods
(liquid#412). The limits and their defaults are in
[`docs/library/http2.md`](../../docs/library/http2.md).
trailer status parsing, and `grpc_message_reader`, which assembles messages from a stream's DATA
frames (`connection::receive_data_part`) while holding at most one message of a set size
(liquid#451). Connection I/O will remain Corosio-only through `runtime/net.hpp`.
