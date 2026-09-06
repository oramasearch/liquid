#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
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

[[nodiscard]] auto parse_url(std::string_view input, std::string context) -> result<parsed_url>;
[[nodiscard]] auto endpoint_target(const parsed_url& url, std::string_view suffix) -> std::string;
[[nodiscard]] auto perform_request(http_request request) -> runtime::task<http_response>;

} // namespace liquid::net_detail
