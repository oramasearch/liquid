// Protocol Buffers as liquid's own codec reads them: the wire reader, and the REAPI v2 messages a
// remote execution peer sends. One selector byte picks the message.

#include <cstddef>
#include <cstdint>

#include "fuzz.hpp"
#include "protobuf/wire.hpp"
#include "reapi/messages.hpp"

namespace reapi = liquid::reapi;

namespace {

template <typename Message> void decode(std::span<const std::byte> input) {
    auto message = Message{};
    if (reapi::decode(input, message)) {
        (void)reapi::encode(message);
    }
}

void read_fields(std::span<const std::byte> input) {
    auto reader = liquid::protobuf::reader{input};
    for (;;) {
        const auto field = reader.next();
        if (!field || !*field) {
            return;
        }
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto input = liquid::fuzz::select(liquid::fuzz::bytes(data, size), 13);
    if (!input) {
        return 0;
    }
    const auto payload = input->payload;
    switch (input->decoder) {
    case 0:
        read_fields(payload);
        break;
    case 1:
        decode<reapi::Action>(payload);
        break;
    case 2:
        decode<reapi::Command>(payload);
        break;
    case 3:
        decode<reapi::Directory>(payload);
        break;
    case 4:
        decode<reapi::Tree>(payload);
        break;
    case 5:
        decode<reapi::ActionResult>(payload);
        break;
    case 6:
        decode<reapi::ExecuteRequest>(payload);
        break;
    case 7:
        decode<reapi::Operation>(payload);
        break;
    case 8:
        decode<reapi::BatchUpdateBlobsRequest>(payload);
        break;
    case 9:
        decode<reapi::BatchReadBlobsResponse>(payload);
        break;
    case 10:
        decode<reapi::WriteRequest>(payload);
        break;
    case 11:
        decode<reapi::ReadResponse>(payload);
        break;
    default:
        decode<reapi::ServerCapabilities>(payload);
        break;
    }
    return 0;
}
