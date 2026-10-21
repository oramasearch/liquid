// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "liquid/export.hpp"
#include "liquid/runtime.hpp"

namespace liquid::http1 {

struct response {
    unsigned status{};
    std::vector<std::byte> body;
    /// Every response header field in arrival order, names as received.
    std::vector<std::pair<std::string, std::string>> headers;
};

/// The value of the first header field named `name` (compared case-insensitively), or empty.
[[nodiscard]] auto header_value(std::span<const std::pair<std::string, std::string>> headers,
                                std::string_view name) -> std::string_view;

struct request_options {
    std::string url;
    std::string method{"GET"};
    std::string suffix;
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<std::byte> body;
    bool verify_peer{true};
};

[[nodiscard]] auto validate_url(std::string_view url) -> result<void>;
[[nodiscard]] LIQUID_EXPORT auto request(request_options options) -> runtime::task<response>;

} // namespace liquid::http1
