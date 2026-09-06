#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <boost/http.hpp>
#include <nlohmann/json.hpp>

#include "http1/client.hpp"
#include "liquid/error.hpp"
#include "liquid/runtime.hpp"

namespace liquid::net_detail {

struct parsed_url {
    bool tls{};
    std::string host;
    std::string port;
    std::string path;
    std::string origin;
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

inline auto perform_request(const parsed_url& url,
                            std::string_view token,
                            boost::http::method method,
                            std::string target,
                            std::string_view content_type,
                            std::span<const std::byte> body,
                            std::string context) -> runtime::task<http1::response> {
    auto headers = std::vector<std::pair<std::string, std::string>>{};
    if (!content_type.empty()) {
        headers.emplace_back("Content-Type", std::string{content_type});
    }
    if (!token.empty()) {
        headers.emplace_back("Authorization", "Bearer " + std::string{token});
    }
    auto response =
        co_await http1::request({.url = url.origin,
                                 .method = method == boost::http::method::get ? "GET" : "POST",
                                 .suffix = std::move(target),
                                 .headers = std::move(headers),
                                 .body = {body.begin(), body.end()}});
    if (!response) {
        co_return std::unexpected(
            http_error(response.error().code, response.error().message, std::move(context)));
    }
    co_return std::move(*response);
}

inline auto json_request(const parsed_url& url,
                         std::string_view token,
                         boost::http::method method,
                         std::string target,
                         const nlohmann::json& input,
                         std::string context) -> runtime::task<nlohmann::json> {
    auto encoded = std::string{};
    if (method != boost::http::method::get) {
        encoded = input.dump();
    }
    const auto* first = reinterpret_cast<const std::byte*>(encoded.data());
    auto response = co_await perform_request(url,
                                             token,
                                             method,
                                             std::move(target),
                                             "application/json",
                                             std::span{first, encoded.size()},
                                             context);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    try {
        const auto text = std::string_view{reinterpret_cast<const char*>(response->body.data()),
                                           response->body.size()};
        auto envelope = nlohmann::json::parse(text);
        if (!envelope.is_object() || !envelope.contains("ok") || !envelope.at("ok").is_boolean()) {
            co_return std::unexpected(
                http_error(errc::corrupt, "invalid JSON response envelope", std::move(context)));
        }
        if (envelope.at("ok").get<bool>()) {
            if (!envelope.contains("value")) {
                co_return std::unexpected(http_error(
                    errc::corrupt, "successful JSON envelope has no value", std::move(context)));
            }
            co_return envelope.at("value");
        }
        const auto failure = envelope.value("error", nlohmann::json::object());
        const auto code_name = failure.value("code", "internal");
        auto code = errc::internal;
        if (code_name == "invalid_input") {
            code = errc::invalid_argument;
        } else if (code_name == "unauthorized" || code_name == "mfa_required") {
            code = errc::unauthorized;
        } else if (code_name == "not_found") {
            code = errc::not_found;
        } else if (code_name == "conflict") {
            code = errc::conflict;
        } else if (code_name == "store_unavailable") {
            code = errc::unavailable;
        }
        co_return std::unexpected(http_error(code,
                                             failure.value("message", "lake request failed"),
                                             failure.value("path", std::string{})));
    } catch (const nlohmann::json::exception& exception) {
        co_return std::unexpected(http_error(errc::corrupt, exception.what(), std::move(context)));
    }
}

} // namespace liquid::net_detail
