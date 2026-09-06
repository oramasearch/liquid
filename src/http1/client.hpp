#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "liquid/runtime.hpp"

namespace liquid::http1 {

struct response {
    unsigned status{};
    std::vector<std::byte> body;
};

struct request_options {
    std::string url;
    std::string method{"GET"};
    std::string suffix;
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<std::byte> body;
    bool verify_peer{true};
};

[[nodiscard]] auto validate_url(std::string_view url) -> result<void>;
[[nodiscard]] auto request(request_options options) -> runtime::task<response>;

} // namespace liquid::http1
