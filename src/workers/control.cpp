#include "workers/control.hpp"

#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <boost/http.hpp>

#include "http1/server.hpp"

namespace liquid::workers {
namespace {

auto openapi(std::string_view component) -> std::string {
    return std::format(
        R"({{"openapi":"3.1.0","info":{{"title":"liquid {}","version":"v1"}},"paths":{{"/healthz":{{"get":{{"responses":{{"200":{{"description":"healthy"}}}}}}}},"/readyz":{{"get":{{"responses":{{"200":{{"description":"ready"}},"503":{{"description":"not ready"}}}}}}}},"/metrics":{{"get":{{"responses":{{"200":{{"description":"Prometheus metrics"}}}}}}}},"/v1":{{"get":{{"responses":{{"200":{{"description":"capabilities"}}}}}}}},"/v1/openapi.json":{{"get":{{"responses":{{"200":{{"description":"OpenAPI document"}}}}}}}},"/mcp":{{"post":{{"responses":{{"200":{{"description":"MCP JSON-RPC response"}}}}}}}}}}}})",
        component);
}

auto json_id(std::string_view body) -> std::string_view {
    const auto field = body.find("\"id\"");
    const auto colon = field == std::string_view::npos ? field : body.find(':', field + 4);
    if (colon == std::string_view::npos) {
        return "null";
    }
    const auto begin = body.find_first_not_of(" \t\r\n", colon + 1);
    if (begin == std::string_view::npos) {
        return "null";
    }
    if (body[begin] == '"') {
        const auto end = body.find('"', begin + 1);
        return end == std::string_view::npos ? std::string_view{"null"}
                                             : body.substr(begin, end - begin + 1);
    }
    const auto end = body.find_first_of(",}\r\n \t", begin);
    return body.substr(begin, end == std::string_view::npos ? body.size() - begin : end - begin);
}

auto mcp_response(std::string_view body, std::string_view component) -> std::string {
    const auto id = json_id(body);
    if (body.find("\"method\":\"initialize\"") != std::string_view::npos) {
        return std::format(
            R"({{"jsonrpc":"2.0","id":{},"result":{{"protocolVersion":"2025-06-18","capabilities":{{}},"serverInfo":{{"name":"liquid-{}","version":"v1"}}}}}})",
            id,
            component);
    }
    if (body.find("\"method\":\"tools/list\"") != std::string_view::npos) {
        return std::format(R"({{"jsonrpc":"2.0","id":{},"result":{{"tools":[]}}}})", id);
    }
    return std::format(
        R"({{"jsonrpc":"2.0","id":{},"error":{{"code":-32601,"message":"method not found"}}}})",
        id);
}

auto make_router(std::shared_ptr<const control_status> status) -> http1::router {
    auto routes = http1::router{};
    routes.add(boost::http::method::get,
               "/healthz",
               [](boost::http::route_params& params) -> boost::http::route_task {
                   co_return co_await http1::send(
                       params, boost::http::status::ok, "text/plain", "ok\n");
               });
    routes.add(boost::http::method::get,
               "/readyz",
               [status](boost::http::route_params& params) -> boost::http::route_task {
                   const auto ready = status->ready();
                   co_return co_await http1::send(params,
                                                  ready ? boost::http::status::ok
                                                        : boost::http::status::service_unavailable,
                                                  "text/plain",
                                                  ready ? "ready\n" : "not ready\n");
               });
    routes.add(boost::http::method::get,
               "/metrics",
               [status](boost::http::route_params& params) -> boost::http::route_task {
                   co_return co_await http1::send(params,
                                                  boost::http::status::ok,
                                                  "text/plain; version=0.0.4",
                                                  status->metrics());
               });
    routes.add(boost::http::method::get,
               "/v1",
               [status](boost::http::route_params& params) -> boost::http::route_task {
                   co_return co_await http1::send(
                       params,
                       boost::http::status::ok,
                       "application/json",
                       std::format(R"({{"component":"{}","version":"v1"}})", status->component));
               });
    routes.add(boost::http::method::get,
               "/v1/openapi.json",
               [status](boost::http::route_params& params) -> boost::http::route_task {
                   co_return co_await http1::send(params,
                                                  boost::http::status::ok,
                                                  "application/json",
                                                  openapi(status->component));
               });
    routes.add(boost::http::method::post,
               "/mcp",
               [status](boost::http::route_params& params) -> boost::http::route_task {
                   auto body = co_await http1::read_body(params);
                   if (!body) {
                       co_return boost::http::route_error(body.error());
                   }
                   const auto text =
                       std::string{reinterpret_cast<const char*>(body->data()), body->size()};
                   co_return co_await http1::send(params,
                                                  boost::http::status::ok,
                                                  "application/json",
                                                  mcp_response(text, status->component));
               });
    return routes;
}

} // namespace

runtime::task<void> serve_control(std::string listen, control_status status) {
    auto shared_status = std::make_shared<const control_status>(std::move(status));
    co_return co_await http1::serve(http1::server_options{.listen = std::move(listen)},
                                    make_router(std::move(shared_status)));
}

} // namespace liquid::workers
