#include "workers/control.hpp"

#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "serve/http.hpp"

namespace liquid::workers {
namespace {

auto openapi(std::string_view component) -> std::string {
    return std::format(
        R"({{"openapi":"3.1.0","info":{{"title":"liquid {}","version":"v1"}},"paths":{{"/healthz":{{"get":{{"responses":{{"200":{{"description":"healthy"}}}}}}}},"/readyz":{{"get":{{"responses":{{"200":{{"description":"ready"}},"503":{{"description":"not ready"}}}}}}}},"/metrics":{{"get":{{"responses":{{"200":{{"description":"Prometheus metrics"}}}}}}}},"/v1":{{"get":{{"responses":{{"200":{{"description":"capabilities"}}}}}}}},"/v1/openapi.json":{{"get":{{"responses":{{"200":{{"description":"OpenAPI document"}}}}}}}},"/mcp":{{"post":{{"responses":{{"200":{{"description":"MCP JSON-RPC response"}}}}}}}}}}}})",
        component);
}

auto mcp_response(std::string_view body, std::string_view component) -> std::string {
    const auto id = serve::json_rpc_id(body);
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

auto make_routes(std::shared_ptr<const control_status> status)
    -> std::shared_ptr<const serve::route_table> {
    auto routes = std::make_shared<serve::route_table>();
    routes->add(
        "GET", "/healthz", [](const serve::http_request&) -> runtime::task<serve::http_response> {
            co_return serve::make_response(200, "text/plain", "ok\n");
        });
    routes->add("GET",
                "/readyz",
                [status](const serve::http_request&) -> runtime::task<serve::http_response> {
                    const auto ready = status->ready();
                    co_return serve::make_response(
                        ready ? 200U : 503U, "text/plain", ready ? "ready\n" : "not ready\n");
                });
    routes->add("GET",
                "/metrics",
                [status](const serve::http_request&) -> runtime::task<serve::http_response> {
                    co_return serve::make_response(
                        200, "text/plain; version=0.0.4", status->metrics());
                });
    routes->add(
        "GET", "/v1", [status](const serve::http_request&) -> runtime::task<serve::http_response> {
            co_return serve::make_response(
                200,
                "application/json",
                std::format(R"({{"component":"{}","version":"v1"}})", status->component));
        });
    routes->add("GET",
                "/v1/openapi.json",
                [status](const serve::http_request&) -> runtime::task<serve::http_response> {
                    co_return serve::make_response(
                        200, "application/json", openapi(status->component));
                });
    routes->add(
        "POST",
        "/mcp",
        [status](const serve::http_request& request) -> runtime::task<serve::http_response> {
            co_return serve::make_response(
                200, "application/json", mcp_response(request.body, status->component));
        });
    return routes;
}

} // namespace

auto serve_control(std::string listen, control_status status) -> runtime::task<void> {
    auto shared_status = std::make_shared<const control_status>(std::move(status));
    co_return co_await serve::serve_tcp(std::move(listen), make_routes(std::move(shared_status)));
}

} // namespace liquid::workers
