// SPDX-License-Identifier: MIT

#include "http2/grpc.hpp"

#include <charconv>
#include <limits>

namespace liquid::http2 {
namespace {

auto byte_value(std::byte value) -> std::uint32_t {
    return std::to_integer<std::uint8_t>(value);
}

} // namespace

auto encode_grpc_message(const grpc_message& message) -> result<std::vector<std::byte>> {
    if (message.bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(error{errc::invalid_argument, "gRPC message exceeds 4 GiB", "gRPC"});
    }
    const auto size = static_cast<std::uint32_t>(message.bytes.size());
    auto output = std::vector<std::byte>{};
    output.reserve(5 + message.bytes.size());
    output.push_back(message.compressed ? std::byte{1} : std::byte{0});
    output.push_back(static_cast<std::byte>((size >> 24) & 0xffU));
    output.push_back(static_cast<std::byte>((size >> 16) & 0xffU));
    output.push_back(static_cast<std::byte>((size >> 8) & 0xffU));
    output.push_back(static_cast<std::byte>(size & 0xffU));
    output.insert(output.end(), message.bytes.begin(), message.bytes.end());
    return output;
}

auto decode_grpc_messages(std::span<const std::byte> input) -> result<std::vector<grpc_message>> {
    auto output = std::vector<grpc_message>{};
    auto offset = std::size_t{};
    while (offset < input.size()) {
        if (input.size() - offset < 5) {
            return std::unexpected(
                error{errc::corrupt, "gRPC message header is incomplete", "gRPC"});
        }
        const auto compressed = byte_value(input[offset]);
        if (compressed > 1) {
            return std::unexpected(
                error{errc::corrupt, "gRPC compression flag is invalid", "gRPC"});
        }
        const auto size = (byte_value(input[offset + 1]) << 24) |
                          (byte_value(input[offset + 2]) << 16) |
                          (byte_value(input[offset + 3]) << 8) | byte_value(input[offset + 4]);
        offset += 5;
        if (size > input.size() - offset) {
            return std::unexpected(error{errc::corrupt, "gRPC message body is incomplete", "gRPC"});
        }
        auto message = grpc_message{};
        message.compressed = compressed == 1;
        message.bytes.assign(input.begin() + static_cast<std::ptrdiff_t>(offset),
                             input.begin() + static_cast<std::ptrdiff_t>(offset + size));
        output.push_back(std::move(message));
        offset += size;
    }
    return output;
}

auto parse_grpc_status(std::string_view status, std::string_view message) -> result<grpc_status> {
    auto code = unsigned{};
    const auto [position, error_code] =
        std::from_chars(status.data(), status.data() + status.size(), code);
    if (error_code != std::errc{} || position != status.data() + status.size()) {
        return std::unexpected(
            error{errc::corrupt, "grpc-status is not an unsigned integer", "gRPC"});
    }
    return grpc_status{code, std::string(message)};
}

} // namespace liquid::http2
