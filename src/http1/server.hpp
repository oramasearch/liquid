// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <boost/http.hpp>

#include "liquid/runtime.hpp"

namespace liquid::http1 {

struct server_options {
    std::string listen;
    std::optional<std::filesystem::path> tls_certificate;
    std::optional<std::filesystem::path> tls_key;
    std::size_t body_limit{64U * 1024U * 1024U};
};

using router = boost::http::router<boost::http::route_params>;

[[nodiscard]] auto read_body(boost::http::route_params& params)
    -> boost::capy::task<std::expected<std::vector<std::byte>, std::error_code>>;
[[nodiscard]] auto send(boost::http::route_params& params,
                        boost::http::status status,
                        std::string content_type,
                        std::string body) -> boost::http::route_task;
[[nodiscard]] auto serve(server_options options, router routes) -> runtime::task<void>;

} // namespace liquid::http1
