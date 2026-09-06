#include "serve/http.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <format>
#include <memory>
#include <print>
#include <string>
#include <system_error>
#include <utility>

#include <boost/capy/buffers.hpp>
#include <boost/capy/write.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/local_stream_acceptor.hpp>
#include <boost/corosio/local_stream_socket.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>

namespace liquid::serve {
namespace {

auto http_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto reason_phrase(unsigned status) -> std::string_view {
    switch (status) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 404:
        return "Not Found";
    case 409:
        return "Conflict";
    case 423:
        return "Locked";
    case 429:
        return "Too Many Requests";
    case 503:
        return "Unavailable";
    default:
        return "Internal Server Error";
    }
}

auto encode_response(const http_response& value) -> std::string {
    return std::format("HTTP/1.1 {} {}\r\n"
                       "Content-Type: {}\r\n"
                       "Content-Length: {}\r\n"
                       "Connection: close\r\n\r\n{}",
                       value.status,
                       reason_phrase(value.status),
                       value.content_type,
                       value.body.size(),
                       value.body);
}

auto json_escape(std::string_view input) -> std::string {
    auto output = std::string{};
    for (const auto character : input) {
        switch (character) {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\b':
            output += "\\b";
            break;
        case '\f':
            output += "\\f";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20U) {
                output += std::format("\\u{:04x}", static_cast<unsigned char>(character));
            } else {
                output.push_back(character);
            }
            break;
        }
    }
    return output;
}

auto parse_request(std::string value) -> result<http_request> {
    const auto header_end = value.find("\r\n\r\n");
    const auto first_space = value.find(' ');
    const auto second_space =
        first_space == std::string::npos ? std::string::npos : value.find(' ', first_space + 1);
    if (header_end == std::string::npos || first_space == std::string::npos ||
        second_space == std::string::npos || second_space > value.find("\r\n")) {
        return std::unexpected(
            http_error(errc::invalid_argument, "control HTTP request line is invalid"));
    }
    auto output = http_request{};
    output.method = value.substr(0, first_space);
    output.target = value.substr(first_space + 1, second_space - first_space - 1);
    const auto query = output.target.find('?');
    output.path = output.target.substr(0, query);
    if (query != std::string::npos) {
        output.query = output.target.substr(query + 1);
    }
    output.body = value.substr(header_end + 4);
    return output;
}

template <typename Socket> auto read_request(Socket& socket) -> runtime::task<http_request> {
    constexpr auto maximum_request_size = std::size_t{1024U * 1024U};
    auto request = std::string{};
    auto buffer = std::array<char, 4096>{};
    while (request.find("\r\n\r\n") == std::string::npos) {
        const auto [read_error, count] =
            co_await socket.read_some(boost::capy::mutable_buffer(buffer.data(), buffer.size()));
        if (read_error) {
            co_return std::unexpected(
                http_error(errc::io, read_error.message(), "control HTTP request"));
        }
        if (count == 0 || request.size() + count > maximum_request_size) {
            co_return std::unexpected(
                http_error(errc::invalid_argument, "control HTTP request is invalid"));
        }
        request.append(buffer.data(), count);
    }
    const auto header_end = request.find("\r\n\r\n");
    auto content_length = std::size_t{};
    if (const auto header = request.find("\r\nContent-Length:"); header != std::string::npos) {
        const auto begin = request.find_first_not_of(" \t", header + 17);
        const auto end = request.find("\r\n", begin);
        const auto parsed =
            std::from_chars(request.data() + begin, request.data() + end, content_length);
        if (parsed.ec != std::errc{} || parsed.ptr != request.data() + end) {
            co_return std::unexpected(
                http_error(errc::invalid_argument, "control Content-Length is invalid"));
        }
    }
    const auto complete_size = header_end + 4 + content_length;
    if (complete_size > maximum_request_size) {
        co_return std::unexpected(
            http_error(errc::invalid_argument, "control HTTP request is too large"));
    }
    while (request.size() < complete_size) {
        const auto remaining = std::min(buffer.size(), complete_size - request.size());
        const auto [read_error, count] =
            co_await socket.read_some(boost::capy::mutable_buffer(buffer.data(), remaining));
        if (read_error || count == 0) {
            co_return std::unexpected(http_error(
                errc::io, read_error ? read_error.message() : "short control HTTP request"));
        }
        request.append(buffer.data(), count);
    }
    request.resize(complete_size);
    co_return parse_request(std::move(request));
}

template <typename Socket>
auto session(std::shared_ptr<Socket> socket, std::shared_ptr<const route_table> routes)
    -> runtime::task<void> {
    auto request = co_await read_request(*socket);
    auto response = request ? co_await routes->dispatch(*request)
                            : result<http_response>{error_response(request.error())};
    if (!response) {
        response = error_response(response.error());
    }
    const auto output = encode_response(*response);
    const auto [write_error, written] = co_await boost::capy::write(
        *socket, boost::capy::const_buffer(output.data(), output.size()));
    if (write_error || written != output.size()) {
        co_return std::unexpected(http_error(
            errc::io, write_error ? write_error.message() : "short control HTTP response"));
    }
    socket->shutdown(boost::corosio::shutdown_both);
    co_return result<void>{};
}

template <typename Socket>
void spawn_session(std::shared_ptr<Socket> socket, std::shared_ptr<const route_table> routes) {
    runtime::spawn(session(std::move(socket), std::move(routes)), [](result<void> outcome) {
        if (!outcome) {
            std::println(stderr,
                         "control connection error: {} ({})",
                         outcome.error().message,
                         outcome.error().context);
        }
    });
}

} // namespace

void route_table::add(std::string method, std::string path, route_handler handler) {
    routes_.push_back({std::move(method), std::move(path), std::move(handler)});
}

runtime::task<http_response> route_table::dispatch(const http_request& request) const {
    const auto found = std::ranges::find_if(routes_, [&](const auto& route) {
        return route.method == request.method && route.path == request.path;
    });
    if (found == routes_.end()) {
        co_return make_response(404, "text/plain", "not found\n");
    }
    co_return co_await found->handler(request);
}

http_response make_response(unsigned status, std::string content_type, std::string body) {
    return {status, std::move(content_type), std::move(body)};
}

http_response error_response(const error& value) {
    const auto status = value.code == errc::invalid_argument || value.code == errc::corrupt  ? 400U
                        : value.code == errc::unauthorized                                   ? 401U
                        : value.code == errc::not_found                                      ? 404U
                        : value.code == errc::already_exists || value.code == errc::conflict ? 409U
                        : value.code == errc::locked                                         ? 423U
                        : value.code == errc::busy                                           ? 429U
                        : value.code == errc::unavailable                                    ? 503U
                                                                                             : 500U;
    return make_response(status,
                         "application/json",
                         std::format(R"({{"error":{{"code":{},"message":"{}","context":"{}"}}}})",
                                     static_cast<unsigned>(value.code),
                                     json_escape(value.message),
                                     json_escape(value.context)));
}

std::string_view json_rpc_id(std::string_view body) {
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

runtime::task<void> serve_tcp(std::string listen,
                              std::shared_ptr<const route_table> routes,
                              std::function<void()> on_bound) {
    auto endpoint = boost::corosio::endpoint{};
    if (const auto code = boost::corosio::parse_endpoint(listen, endpoint)) {
        co_return std::unexpected(http_error(errc::invalid_argument, code.message(), listen));
    }
    if (endpoint.port() == 0) {
        co_return std::unexpected(
            http_error(errc::invalid_argument, "control listen port must be non-zero"));
    }
    const auto executor = co_await boost::capy::this_coro::executor;
    auto acceptor = boost::corosio::tcp_acceptor{executor.context(), endpoint};
    if (on_bound) {
        on_bound();
    }
    for (;;) {
        auto socket = std::make_shared<boost::corosio::tcp_socket>(executor.context());
        const auto [accept_error] = co_await acceptor.accept(*socket);
        if (accept_error) {
            co_return std::unexpected(http_error(errc::io, accept_error.message(), listen));
        }
        spawn_session(std::move(socket), routes);
    }
}

runtime::task<void> serve_unix(std::filesystem::path path,
                               std::shared_ptr<const route_table> routes,
                               std::function<void()> on_bound) {
#if defined(_WIN32)
    static_cast<void>(path);
    static_cast<void>(routes);
    static_cast<void>(on_bound);
    co_return std::unexpected(
        http_error(errc::unsupported, "Unix control sockets are unavailable on Windows"));
#else
    auto prepared = co_await runtime::offload([parent = path.parent_path()]() -> result<void> {
        auto code = std::error_code{};
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, code);
        }
        if (code) {
            return std::unexpected(http_error(errc::io, code.message(), parent.string()));
        }
        return {};
    });
    if (!prepared) {
        co_return std::unexpected(prepared.error());
    }
    if (!*prepared) {
        co_return std::unexpected(prepared->error());
    }
    const auto executor = co_await boost::capy::this_coro::executor;
    auto acceptor = boost::corosio::local_stream_acceptor{executor.context()};
    acceptor.open();
    const auto endpoint = boost::corosio::local_endpoint{path.string()};
    if (const auto code = acceptor.bind(endpoint, boost::corosio::bind_option::unlink_existing)) {
        co_return std::unexpected(http_error(errc::io, code.message(), path.string()));
    }
    if (const auto code = acceptor.listen()) {
        co_return std::unexpected(http_error(errc::io, code.message(), path.string()));
    }
    if (on_bound) {
        on_bound();
    }
    for (;;) {
        auto socket = std::make_shared<boost::corosio::local_stream_socket>(executor.context());
        const auto [accept_error] = co_await acceptor.accept(*socket);
        if (accept_error) {
            co_return std::unexpected(http_error(errc::io, accept_error.message(), path.string()));
        }
        spawn_session(std::move(socket), routes);
    }
#endif
}

} // namespace liquid::serve
