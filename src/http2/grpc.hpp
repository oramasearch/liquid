// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "liquid/error.hpp"

namespace liquid::http2 {

struct grpc_message {
    bool compressed{};
    std::vector<std::byte> bytes;
};

struct grpc_status {
    unsigned code{};
    std::string message;
};

[[nodiscard]] auto encode_grpc_message(const grpc_message& message)
    -> result<std::vector<std::byte>>;
[[nodiscard]] auto decode_grpc_messages(std::span<const std::byte> input)
    -> result<std::vector<grpc_message>>;
[[nodiscard]] auto parse_grpc_status(std::string_view status, std::string_view message)
    -> result<grpc_status>;

// Assembles gRPC messages from a stream's bytes as they arrive, so a server holds one message of
// at most `max_message_bytes` instead of the whole request body (liquid#451). A message whose
// 5-byte prefix declares more is refused before any of its body is kept.
class grpc_message_reader {
public:
    explicit grpc_message_reader(std::size_t max_message_bytes) noexcept;

    // Adds bytes from the stream. Fails when the next message is over the limit or its
    // compression flag is invalid; `exceeded()` then says which.
    [[nodiscard]] auto append(std::span<const std::byte> bytes) -> result<void>;
    // The next complete message, or nothing until more bytes arrive.
    [[nodiscard]] auto next() -> result<std::optional<grpc_message>>;
    // Bytes held that are not yet part of a returned message.
    [[nodiscard]] auto buffered() const noexcept -> std::size_t;
    [[nodiscard]] auto exceeded() const noexcept -> bool;
    [[nodiscard]] auto max_message_bytes() const noexcept -> std::size_t;

private:
    std::size_t max_message_bytes_;
    std::vector<std::byte> buffer_;
    bool exceeded_{};

    [[nodiscard]] auto declared_size() const noexcept -> std::uint32_t;
    [[nodiscard]] auto complete() const noexcept -> bool;
    [[nodiscard]] auto check_header() -> result<void>;
};

} // namespace liquid::http2
