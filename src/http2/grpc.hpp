#pragma once

#include <cstddef>
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

} // namespace liquid::http2
