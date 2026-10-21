// HTTP/2 framing, HPACK header blocks, Huffman strings and gRPC message framing, the transport
// that REAPI and liquid's workers speak. Read-only use of src/http2: this harness does not change
// the protocol code (liquid #412 is changing it).

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "fuzz.hpp"
#include "http2/frame.hpp"
#include "http2/grpc.hpp"
#include "http2/hpack.hpp"

namespace http2 = liquid::http2;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto input = liquid::fuzz::select(liquid::fuzz::bytes(data, size), 5);
    if (!input) {
        return 0;
    }
    switch (input->decoder) {
    case 0: {
        const auto frame = http2::decode_frame(input->payload);
        if (frame) {
            (void)http2::encode_frame(*frame);
        }
        break;
    }
    case 1: {
        // One decoder across several header blocks, so the dynamic table carries state between
        // them as it does on a connection.
        auto decoder = http2::hpack_decoder{};
        for (const auto block : liquid::fuzz::parts(input->payload, 16)) {
            if (!decoder.decode(block)) {
                break;
            }
        }
        break;
    }
    case 2:
        (void)http2::huffman_decode(input->payload);
        break;
    case 3:
        (void)http2::decode_grpc_messages(input->payload);
        break;
    default: {
        const auto text = liquid::fuzz::text(input->payload);
        const auto separator = text.find('\n');
        const auto status = text.substr(0, separator);
        const auto message =
            separator == std::string_view::npos ? std::string_view{} : text.substr(separator + 1);
        (void)http2::parse_grpc_status(status, message);
        break;
    }
    }
    return 0;
}
