#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/export.hpp"
#include "liquid/runtime.hpp"

namespace liquid::serve {

struct http_request {
    std::string method;
    std::string target;
    std::string path;
    std::string query;
    std::string body;
};

struct http_response {
    unsigned status{200};
    std::string content_type{"application/json"};
    std::string body;
};

using route_handler = std::function<runtime::task<http_response>(const http_request&)>;

class LIQUID_EXPORT route_table {
public:
    void add(std::string method, std::string path, route_handler handler);
    [[nodiscard]] runtime::task<http_response> dispatch(const http_request& request) const;

private:
    struct route {
        std::string method;
        std::string path;
        route_handler handler;
    };
    std::vector<route> routes_;
};

[[nodiscard]] LIQUID_EXPORT http_response make_response(unsigned status,
                                                        std::string content_type,
                                                        std::string body);
[[nodiscard]] LIQUID_EXPORT http_response error_response(const error& value);
[[nodiscard]] LIQUID_EXPORT std::string_view json_rpc_id(std::string_view body);

[[nodiscard]] LIQUID_EXPORT runtime::task<void> serve_tcp(std::string listen,
                                                          std::shared_ptr<const route_table> routes,
                                                          std::function<void()> on_bound = {});
[[nodiscard]] LIQUID_EXPORT runtime::task<void>
serve_unix(std::filesystem::path path,
           std::shared_ptr<const route_table> routes,
           std::function<void()> on_bound = {});

} // namespace liquid::serve
