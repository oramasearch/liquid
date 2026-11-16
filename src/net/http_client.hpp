// SPDX-License-Identifier: MIT

#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/runtime.hpp"

namespace liquid::net_detail {

enum class http_method {
    get,
    post,
};

struct parsed_url {
    bool tls{};
    std::string host;
    std::string port;
    std::string path;
    std::string origin;
};

struct http_request {
    http_method method{};
    parsed_url url;
    std::string target;
    std::string bearer;
    std::string content_type;
    std::vector<std::byte> body;
};

struct http_response {
    unsigned status{};
    std::vector<std::byte> body;
};

inline auto http_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

inline auto parse_url(std::string_view input, std::string_view context) -> result<parsed_url> {
    auto output = parsed_url{};
    if (input.starts_with("https://")) {
        output.tls = true;
        output.port = "443";
        input.remove_prefix(8);
    } else if (input.starts_with("http://")) {
        output.port = "80";
        input.remove_prefix(7);
    } else {
        return std::unexpected(http_error(errc::invalid_argument,
                                          std::string{context} + " must use http or https",
                                          std::string{context}));
    }
    const auto slash = input.find('/');
    const auto authority = input.substr(0, slash);
    output.path =
        slash == std::string_view::npos ? std::string{} : std::string{input.substr(slash)};
    while (output.path.size() > 1 && output.path.ends_with('/')) {
        output.path.pop_back();
    }
    if (authority.empty() || authority.find('@') != std::string_view::npos) {
        return std::unexpected(http_error(errc::invalid_argument,
                                          "invalid " + std::string{context} + " authority",
                                          std::string{context}));
    }
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) {
            return std::unexpected(http_error(errc::invalid_argument,
                                              "invalid IPv6 " + std::string{context},
                                              std::string{context}));
        }
        output.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') {
                return std::unexpected(http_error(errc::invalid_argument,
                                                  "invalid " + std::string{context} + " port",
                                                  std::string{context}));
            }
            output.port = authority.substr(close + 2);
        }
    } else {
        const auto colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            output.host = authority.substr(0, colon);
            output.port = authority.substr(colon + 1);
        } else {
            output.host = authority;
        }
    }
    auto port = std::uint32_t{};
    const auto parsed =
        std::from_chars(output.port.data(), output.port.data() + output.port.size(), port);
    if (output.host.empty() || output.port.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != output.port.data() + output.port.size() || port == 0 || port > 65535) {
        return std::unexpected(http_error(errc::invalid_argument,
                                          std::string{context} + " host or port is invalid",
                                          std::string{context}));
    }
    if (output.path.empty()) {
        output.path = "/";
    }
    output.origin = output.tls ? "https://" : "http://";
    if (output.host.find(':') != std::string::npos) {
        output.origin += '[' + output.host + ']';
    } else {
        output.origin += output.host;
    }
    output.origin += ':' + output.port;
    return output;
}

[[nodiscard]] auto request_target(const parsed_url& url, std::string_view suffix) -> std::string;
[[nodiscard]] auto perform_request(http_request request) -> runtime::task<http_response>;

} // namespace liquid::net_detail
