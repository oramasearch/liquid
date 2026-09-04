# HTTP/2 implementation

This private implementation owns HTTP/2 framing, HPACK, and gRPC message framing. It is used only
inside libliquid until issue #16 defines the public REAPI surface.

`hpack.*` implements RFC 7541 static and dynamic tables plus Huffman strings. `frame.*` validates
the RFC 9113 wire frame envelope. `grpc.*` implements gRPC's five-byte message envelope and
trailer status parsing. Connection I/O will remain Corosio-only through `runtime/net.hpp`.
