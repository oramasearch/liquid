// SPDX-License-Identifier: MIT

#include "http2/frame.hpp"

#include <limits>

namespace liquid::http2 {
namespace {

auto fail(std::string message) -> result<frame> {
    return std::unexpected(error{errc::corrupt, std::move(message), "HTTP/2 frame"});
}

auto byte_value(std::byte value) -> std::uint32_t {
    return std::to_integer<std::uint8_t>(value);
}

} // namespace

auto encode_frame(const frame& value) -> result<std::vector<std::byte>> {
    if (value.payload.size() > 0x00ff'ffffU) {
        return std::unexpected(
            error{errc::invalid_argument, "frame payload exceeds 24-bit length", "HTTP/2 frame"});
    }
    if (value.stream_id > 0x7fff'ffffU) {
        return std::unexpected(
            error{errc::invalid_argument, "frame stream ID exceeds 31 bits", "HTTP/2 frame"});
    }
    auto output = std::vector<std::byte>{};
    output.reserve(9 + value.payload.size());
    const auto length = static_cast<std::uint32_t>(value.payload.size());
    output.push_back(static_cast<std::byte>((length >> 16) & 0xffU));
    output.push_back(static_cast<std::byte>((length >> 8) & 0xffU));
    output.push_back(static_cast<std::byte>(length & 0xffU));
    output.push_back(static_cast<std::byte>(value.type));
    output.push_back(static_cast<std::byte>(value.flags));
    output.push_back(static_cast<std::byte>((value.stream_id >> 24) & 0x7fU));
    output.push_back(static_cast<std::byte>((value.stream_id >> 16) & 0xffU));
    output.push_back(static_cast<std::byte>((value.stream_id >> 8) & 0xffU));
    output.push_back(static_cast<std::byte>(value.stream_id & 0xffU));
    output.insert(output.end(), value.payload.begin(), value.payload.end());
    return output;
}

auto decode_frame(std::span<const std::byte> input) -> result<frame> {
    if (input.size() < 9) {
        return fail("frame header is incomplete");
    }
    const auto length =
        (byte_value(input[0]) << 16) | (byte_value(input[1]) << 8) | byte_value(input[2]);
    if (input.size() != 9 + length) {
        return fail("frame length does not match payload");
    }
    const auto stream_id = ((byte_value(input[5]) & 0x7fU) << 24) | (byte_value(input[6]) << 16) |
                           (byte_value(input[7]) << 8) | byte_value(input[8]);
    auto output = frame{};
    output.type = static_cast<frame_type>(byte_value(input[3]));
    output.flags = static_cast<std::uint8_t>(byte_value(input[4]));
    output.stream_id = stream_id;
    output.payload.assign(input.begin() + 9, input.end());
    return output;
}

auto setting_payload(std::uint16_t id, std::uint32_t value) -> std::array<std::byte, 6> {
    return {static_cast<std::byte>((id >> 8) & 0xffU),
            static_cast<std::byte>(id & 0xffU),
            static_cast<std::byte>((value >> 24) & 0xffU),
            static_cast<std::byte>((value >> 16) & 0xffU),
            static_cast<std::byte>((value >> 8) & 0xffU),
            static_cast<std::byte>(value & 0xffU)};
}

} // namespace liquid::http2
