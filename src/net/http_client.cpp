#include "net/http_client.hpp"

#include <utility>

#include "http1/client.hpp"
#include "liquid/version.hpp"

namespace liquid::net_detail {

auto request_target(const parsed_url& url, std::string_view suffix) -> std::string {
    auto output = url.path == "/" ? std::string{} : url.path;
    output += suffix;
    return output.empty() ? "/" : output;
}

auto perform_request(http_request request) -> runtime::task<http_response> {
    auto headers = std::vector<std::pair<std::string, std::string>>{};
    if (!request.content_type.empty()) {
        headers.emplace_back("Content-Type", std::move(request.content_type));
    }
    // Lake reads this to refuse a liquid outside the range it supports (docs/library/lakes.md).
    headers.emplace_back("Liquid-Version", std::string{version_string()});
    if (!request.bearer.empty()) {
        headers.emplace_back("Authorization", "Bearer " + std::move(request.bearer));
    }
    auto response = co_await http1::request({
        .url = request.url.origin,
        .method = request.method == http_method::get ? "GET" : "POST",
        .suffix = std::move(request.target),
        .headers = std::move(headers),
        .body = std::move(request.body),
    });
    if (!response) {
        co_return std::unexpected(
            http_error(response.error().code, response.error().message, request.url.host));
    }
    co_return http_response{.status = response->status, .body = std::move(response->body)};
}

} // namespace liquid::net_detail
