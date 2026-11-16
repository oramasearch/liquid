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

grpc_message_reader::grpc_message_reader(std::size_t max_message_bytes) noexcept
    : max_message_bytes_(max_message_bytes) {}

auto grpc_message_reader::declared_size() const noexcept -> std::uint32_t {
    return (byte_value(buffer_[1]) << 24) | (byte_value(buffer_[2]) << 16) |
           (byte_value(buffer_[3]) << 8) | byte_value(buffer_[4]);
}

auto grpc_message_reader::complete() const noexcept -> bool {
    return buffer_.size() >= 5 && buffer_.size() - 5 >= declared_size();
}

auto grpc_message_reader::check_header() -> result<void> {
    if (buffer_.size() < 5) {
        return {};
    }
    if (byte_value(buffer_[0]) > 1) {
        return std::unexpected(error{errc::corrupt, "gRPC compression flag is invalid", "gRPC"});
    }
    const auto size = declared_size();
    if (size > max_message_bytes_) {
        exceeded_ = true;
        return std::unexpected(
            error{errc::invalid_argument,
                  "gRPC message of " + std::to_string(size) + " bytes exceeds the limit of " +
                      std::to_string(max_message_bytes_),
                  "gRPC"});
    }
    return {};
}

auto grpc_message_reader::append(std::span<const std::byte> bytes) -> result<void> {
    if (exceeded_) {
        return std::unexpected(
            error{errc::invalid_argument, "gRPC message limit exceeded", "gRPC"});
    }
    // What is held is never more than one unfinished message plus the bytes just added: a caller
    // takes every complete message with next() before adding more.
    if (complete()) {
        return std::unexpected(
            error{errc::internal, "gRPC reader still holds a complete message", "gRPC"});
    }
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    return check_header();
}

auto grpc_message_reader::next() -> result<std::optional<grpc_message>> {
    auto checked = check_header();
    if (!checked) {
        return std::unexpected(checked.error());
    }
    if (!complete()) {
        return std::optional<grpc_message>{};
    }
    const auto size = declared_size();
    auto message = grpc_message{};
    message.compressed = byte_value(buffer_[0]) == 1;
    const auto body = buffer_.begin() + 5;
    message.bytes.assign(body, body + static_cast<std::ptrdiff_t>(size));
    buffer_.erase(buffer_.begin(), body + static_cast<std::ptrdiff_t>(size));
    return std::optional<grpc_message>{std::move(message)};
}

auto grpc_message_reader::buffered() const noexcept -> std::size_t {
    return buffer_.size();
}

auto grpc_message_reader::exceeded() const noexcept -> bool {
    return exceeded_;
}

auto grpc_message_reader::max_message_bytes() const noexcept -> std::size_t {
    return max_message_bytes_;
}

} // namespace liquid::http2
