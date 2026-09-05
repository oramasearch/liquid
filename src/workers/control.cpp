#include "workers/control.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <format>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <utility>

#include <boost/capy/buffers.hpp>
#include <boost/capy/write.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>

namespace liquid::workers {
namespace {

auto control_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto response(int status, std::string_view type, std::string body) -> std::string {
    const auto reason = status == 200 ? "OK" : status == 404 ? "Not Found" : "Unavailable";
    return std::format("HTTP/1.1 {} {}\r\n"
                       "Content-Type: {}\r\n"
                       "Content-Length: {}\r\n"
                       "Connection: close\r\n\r\n{}",
                       status,
                       reason,
                       type,
                       body.size(),
                       body);
}

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
    auto begin = body.find_first_not_of(" \t\r\n", colon + 1);
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

auto route(std::string_view method,
           std::string_view target,
           std::string_view body,
           const control_status& status) -> std::string {
    if (method == "GET" && target == "/healthz") {
        return response(200, "text/plain", "ok\n");
    }
    if (method == "GET" && target == "/readyz") {
        const auto ready = status.ready();
        return response(ready ? 200 : 503, "text/plain", ready ? "ready\n" : "not ready\n");
    }
    if (method == "GET" && target == "/metrics") {
        return response(200, "text/plain; version=0.0.4", status.metrics());
    }
    if (method == "GET" && target == "/v1") {
        return response(200,
                        "application/json",
                        std::format(R"({{"component":"{}","version":"v1"}})", status.component));
    }
    if (method == "GET" && target == "/v1/openapi.json") {
        return response(200, "application/json", openapi(status.component));
    }
    if (method == "POST" && target == "/mcp") {
        return response(200, "application/json", mcp_response(body, status.component));
    }
    return response(404, "text/plain", "not found\n");
}

auto session(std::shared_ptr<boost::corosio::tcp_socket> socket,
             std::shared_ptr<const control_status> status) -> runtime::task<void> {
    auto request = std::string{};
    auto buffer = std::array<char, 4096>{};
    while (request.find("\r\n\r\n") == std::string::npos) {
        const auto [read_error, count] =
            co_await socket->read_some(boost::capy::mutable_buffer(buffer.data(), buffer.size()));
        if (read_error) {
            co_return std::unexpected(
                control_error(errc::io, read_error.message(), "control HTTP request"));
        }
        if (count == 0 || request.size() + count > 16U * 1024U) {
            co_return std::unexpected(
                control_error(errc::invalid_argument, "control HTTP request is invalid"));
        }
        request.append(buffer.data(), count);
    }
    const auto header_end = request.find("\r\n\r\n");
    auto content_length = std::size_t{};
    if (const auto header = request.find("\r\nContent-Length:"); header != std::string::npos) {
        const auto begin = request.find_first_not_of(" \t", header + 17);
        const auto end = request.find("\r\n", begin);
        auto parsed = std::from_chars(request.data() + begin, request.data() + end, content_length);
        if (parsed.ec != std::errc{} || parsed.ptr != request.data() + end) {
            co_return std::unexpected(
                control_error(errc::invalid_argument, "control Content-Length is invalid"));
        }
    }
    const auto complete_size = header_end + 4 + content_length;
    while (request.size() < complete_size) {
        const auto remaining = std::min(buffer.size(), complete_size - request.size());
        const auto [read_error, count] =
            co_await socket->read_some(boost::capy::mutable_buffer(buffer.data(), remaining));
        if (read_error || count == 0) {
            co_return std::unexpected(control_error(
                errc::io, read_error ? read_error.message() : "short control HTTP request"));
        }
        request.append(buffer.data(), count);
    }
    const auto first_space = request.find(' ');
    const auto second_space =
        first_space == std::string::npos ? std::string::npos : request.find(' ', first_space + 1);
    if (first_space == std::string::npos || second_space == std::string::npos) {
        co_return std::unexpected(
            control_error(errc::invalid_argument, "control HTTP request line is invalid"));
    }
    const auto output =
        route(std::string_view{request}.substr(0, first_space),
              std::string_view{request}.substr(first_space + 1, second_space - first_space - 1),
              std::string_view{request}.substr(header_end + 4, content_length),
              *status);
    const auto [write_error, written] = co_await boost::capy::write(
        *socket, boost::capy::const_buffer(output.data(), output.size()));
    if (write_error || written != output.size()) {
        co_return std::unexpected(control_error(
            errc::io, write_error ? write_error.message() : "short control HTTP response"));
    }
    socket->shutdown(boost::corosio::shutdown_both);
    co_return result<void>{};
}

} // namespace

auto serve_control(std::string listen, control_status status) -> runtime::task<void> {
    auto endpoint = boost::corosio::endpoint{};
    if (const auto code = boost::corosio::parse_endpoint(listen, endpoint)) {
        co_return std::unexpected(control_error(errc::invalid_argument, code.message(), listen));
    }
    if (endpoint.port() == 0) {
        co_return std::unexpected(
            control_error(errc::invalid_argument, "control listen port must be non-zero"));
    }
    const auto executor = co_await boost::capy::this_coro::executor;
    auto acceptor = boost::corosio::tcp_acceptor{executor.context(), endpoint};
    auto shared_status = std::make_shared<const control_status>(std::move(status));
    for (;;) {
        auto socket = std::make_shared<boost::corosio::tcp_socket>(executor.context());
        const auto [accept_error] = co_await acceptor.accept(*socket);
        if (accept_error) {
            co_return std::unexpected(
                control_error(errc::io, accept_error.message(), std::move(listen)));
        }
        runtime::spawn(session(std::move(socket), shared_status), [](result<void> outcome) {
            if (!outcome) {
                std::println(stderr,
                             "control connection error: {} ({})",
                             outcome.error().message,
                             outcome.error().context);
            }
        });
    }
}

} // namespace liquid::workers
