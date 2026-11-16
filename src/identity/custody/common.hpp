// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "formats/json.hpp"
#include "liquid/identity.hpp"

namespace liquid::custody_detail {

using sha256_digest = std::array<std::byte, 32>;
using http_headers = std::vector<std::pair<std::string, std::string>>;

struct http_response {
    unsigned status{};
    http_headers headers;
    std::vector<std::byte> body;
};

[[nodiscard]] auto custody_error(errc code, std::string message, std::string context) -> error;
[[nodiscard]] auto reference(const wrap_context& context) -> std::string;
[[nodiscard]] auto as_bytes(std::string_view value) -> std::span<const std::byte>;
[[nodiscard]] auto bytes(std::string_view value) -> std::vector<std::byte>;
[[nodiscard]] auto text(std::span<const std::byte> value) -> std::string;

[[nodiscard]] auto base64_encode(std::span<const std::byte> input, bool url = false) -> std::string;
[[nodiscard]] auto base64_decode(std::string_view input, bool url = false, bool padded = false)
    -> result<std::vector<std::byte>>;
[[nodiscard]] auto hex(std::span<const std::byte> input) -> std::string;
[[nodiscard]] auto sha256(std::span<const std::byte> input) -> sha256_digest;
[[nodiscard]] auto hmac_sha256(std::span<const std::byte> key, std::span<const std::byte> input)
    -> sha256_digest;
[[nodiscard]] auto hkdf_sha256(std::span<const std::byte> input,
                               std::span<const std::byte> salt,
                               std::span<const std::byte> info) -> sha256_digest;
[[nodiscard]] auto chacha20_poly1305_seal(std::span<const std::byte, 32> key_bytes,
                                          std::span<const std::byte, 12> nonce,
                                          std::span<const std::byte> additional_data,
                                          std::span<const std::byte> plaintext)
    -> std::vector<std::byte>;
[[nodiscard]] auto chacha20_poly1305_open(std::span<const std::byte, 32> key_bytes,
                                          std::span<const std::byte, 12> nonce,
                                          std::span<const std::byte> additional_data,
                                          std::span<const std::byte> boxed)
    -> result<std::vector<std::byte>>;

[[nodiscard]] auto json(std::initializer_list<std::pair<std::string, formats::json::value>> fields)
    -> result<std::string>;
[[nodiscard]] auto json_field(const formats::json::value& value, std::string_view name)
    -> const formats::json::value*;
[[nodiscard]] auto json_string_field(const formats::json::value& value, std::string_view name)
    -> result<std::string>;

[[nodiscard]] auto
perform_http(std::string method, std::string url, http_headers headers, std::vector<std::byte> body)
    -> runtime::task<http_response>;
[[nodiscard]] auto require_success(http_response response, const wrap_context& context)
    -> result<http_response>;

[[nodiscard]] auto environment(std::string_view name) -> result<std::string>;
[[nodiscard]] auto optional_environment(std::string_view name) -> std::string;

} // namespace liquid::custody_detail
