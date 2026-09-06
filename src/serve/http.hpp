#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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

class route_table {
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

[[nodiscard]] http_response
make_response(unsigned status, std::string content_type, std::string body);
[[nodiscard]] http_response error_response(const error& value);
[[nodiscard]] std::string_view json_rpc_id(std::string_view body);

[[nodiscard]] runtime::task<void> serve_tcp(std::string listen,
                                            std::shared_ptr<const route_table> routes,
                                            std::function<void()> on_bound = {});
[[nodiscard]] runtime::task<void> serve_unix(std::filesystem::path path,
                                             std::shared_ptr<const route_table> routes,
                                             std::function<void()> on_bound = {});

} // namespace liquid::serve
