#include "net/http_client.hpp"

#include <cctype>
#include <charconv>
#include <cstdint>
#include <limits>
#include <system_error>
#include <utility>

#include <boost/capy/buffers.hpp>
#include <boost/capy/buffers/string_dynamic_buffer.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/this_coro.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/write.hpp>
#include <boost/corosio/connect.hpp>
#include <boost/corosio/openssl_stream.hpp>
#include <boost/corosio/resolver.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/tls_context.hpp>
#include <boost/http.hpp>

namespace liquid::net_detail {
namespace {

auto http_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto header_value(std::string_view header, std::string_view name) -> std::string_view {
    auto offset = header.find("\r\n") + 2;
    while (offset < header.size()) {
        const auto end = header.find("\r\n", offset);
        if (end == offset || end == std::string_view::npos) {
            break;
        }
        const auto colon = header.find(':', offset);
        if (colon != std::string_view::npos && colon < end) {
            auto field = header.substr(offset, colon - offset);
            auto matches = field.size() == name.size();
            for (auto index = std::size_t{}; matches && index < field.size(); ++index) {
                const auto left = static_cast<unsigned char>(field[index]);
                const auto right = static_cast<unsigned char>(name[index]);
                matches = std::tolower(left) == std::tolower(right);
            }
            if (matches) {
                auto value = header.substr(colon + 1, end - colon - 1);
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                    value.remove_prefix(1);
                }
                return value;
            }
        }
        offset = end + 2;
    }
    return {};
}

auto decode_chunked(std::string_view input) -> result<std::vector<std::byte>> {
    auto output = std::vector<std::byte>{};
    auto offset = std::size_t{};
    for (;;) {
        const auto line_end = input.find("\r\n", offset);
        if (line_end == std::string_view::npos) {
            return std::unexpected(http_error(errc::corrupt, "truncated HTTP chunk size"));
        }
        const auto extension = input.find(';', offset);
        const auto size_end =
            extension != std::string_view::npos && extension < line_end ? extension : line_end;
        auto size = std::uint64_t{};
        const auto parsed =
            std::from_chars(input.data() + offset, input.data() + size_end, size, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != input.data() + size_end ||
            size > std::numeric_limits<std::size_t>::max()) {
            return std::unexpected(http_error(errc::corrupt, "invalid HTTP chunk size"));
        }
        offset = line_end + 2;
        if (size == 0) {
            return output;
        }
        if (size > input.size() - offset || input.size() - offset - size < 2 ||
            input.substr(offset + size, 2) != "\r\n") {
            return std::unexpected(http_error(errc::corrupt, "truncated HTTP chunk"));
        }
        const auto* first = reinterpret_cast<const std::byte*>(input.data() + offset);
        output.insert(output.end(), first, first + size);
        offset += static_cast<std::size_t>(size) + 2;
    }
}

auto parse_response(std::string wire) -> result<http_response> {
    const auto header_end = wire.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return std::unexpected(http_error(errc::corrupt, "HTTP response header is truncated"));
    }
    const auto header_text = std::string_view{wire}.substr(0, header_end + 4);
    auto header = boost::http::response{header_text};
    const auto body = std::string_view{wire}.substr(header_end + 4);
    auto output = http_response{.status = static_cast<unsigned>(header.status())};
    const auto transfer_encoding = header_value(header_text, "transfer-encoding");
    if (transfer_encoding == "chunked") {
        auto decoded = decode_chunked(body);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        output.body = std::move(*decoded);
        return output;
    }
    const auto length_text = header_value(header_text, "content-length");
    if (!length_text.empty()) {
        auto length = std::uint64_t{};
        const auto parsed =
            std::from_chars(length_text.data(), length_text.data() + length_text.size(), length);
        if (parsed.ec != std::errc{} || parsed.ptr != length_text.data() + length_text.size() ||
            length != body.size()) {
            return std::unexpected(http_error(errc::corrupt, "invalid HTTP content length"));
        }
    }
    const auto* first = reinterpret_cast<const std::byte*>(body.data());
    output.body.assign(first, first + body.size());
    return output;
}

auto make_request(const http_request& input) -> std::string {
    const auto method =
        input.method == http_method::get ? boost::http::method::get : boost::http::method::post;
    auto request = boost::http::request{method, input.target};
    auto authority = input.url.host;
    if ((!input.url.tls && input.url.port != "80") || (input.url.tls && input.url.port != "443")) {
        authority += ':' + input.url.port;
    }
    request.set(boost::http::field::host, authority);
    request.set(boost::http::field::connection, "close");
    if (!input.content_type.empty()) {
        request.set(boost::http::field::content_type, input.content_type);
    }
    request.set_payload_size(input.body.size());
    if (!input.bearer.empty()) {
        request.set(boost::http::field::authorization, "Bearer " + input.bearer);
    }
    auto wire = std::string{request.buffer()};
    wire.append(reinterpret_cast<const char*>(input.body.data()), input.body.size());
    return wire;
}

template <boost::capy::Stream Stream>
auto exchange_http(Stream& stream, std::string wire, std::string_view context)
    -> runtime::task<http_response> {
    auto [write_error, written] =
        co_await boost::capy::write(stream, boost::capy::const_buffer(wire.data(), wire.size()));
    if (write_error || written != wire.size()) {
        co_return std::unexpected(
            http_error(errc::io,
                       write_error ? write_error.message() : "short HTTP write",
                       std::string{context}));
    }
    auto response = std::string{};
    auto [read_error, read] =
        co_await boost::capy::read(stream, boost::capy::string_dynamic_buffer(&response));
    static_cast<void>(read);
    if (read_error && read_error != boost::capy::error::eof) {
        co_return std::unexpected(http_error(errc::io, read_error.message(), std::string{context}));
    }
    try {
        co_return parse_response(std::move(response));
    } catch (const std::exception& exception) {
        co_return std::unexpected(
            http_error(errc::corrupt, exception.what(), std::string{context}));
    }
}

} // namespace

auto parse_url(std::string_view input, std::string context) -> result<parsed_url> {
    auto output = parsed_url{};
    if (input.starts_with("https://")) {
        output.tls = true;
        output.port = "443";
        input.remove_prefix(8);
    } else if (input.starts_with("http://")) {
        output.port = "80";
        input.remove_prefix(7);
    } else {
        return std::unexpected(
            http_error(errc::invalid_argument, "URL must use http or https", std::move(context)));
    }
    const auto slash = input.find('/');
    auto authority = input.substr(0, slash);
    output.path =
        slash == std::string_view::npos ? std::string{} : std::string{input.substr(slash)};
    while (output.path.size() > 1 && output.path.ends_with('/')) {
        output.path.pop_back();
    }
    if (authority.empty() || authority.find('@') != std::string_view::npos) {
        return std::unexpected(
            http_error(errc::invalid_argument, "invalid URL authority", std::move(context)));
    }
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) {
            return std::unexpected(
                http_error(errc::invalid_argument, "invalid IPv6 URL", std::move(context)));
        }
        output.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') {
                return std::unexpected(
                    http_error(errc::invalid_argument, "invalid URL port", std::move(context)));
            }
            output.port = authority.substr(close + 2);
        }
    } else {
        const auto colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            output.host = authority.substr(0, colon);
            output.port = authority.substr(colon + 1);
        } else {
            output.host = authority;
        }
    }
    if (output.host.empty() || output.port.empty()) {
        return std::unexpected(
            http_error(errc::invalid_argument, "URL host or port is empty", std::move(context)));
    }
    if (output.path.empty()) {
        output.path = "/";
    }
    return output;
}

auto endpoint_target(const parsed_url& url, std::string_view suffix) -> std::string {
    auto output = url.path == "/" ? std::string{} : url.path;
    output += suffix;
    return output;
}

auto perform_request(http_request request) -> runtime::task<http_response> {
    try {
        const auto executor = co_await boost::capy::this_coro::executor;
        auto resolver = boost::corosio::resolver{executor.context()};
        auto [resolve_error, results] =
            co_await resolver.resolve(request.url.host, request.url.port);
        if (resolve_error) {
            co_return std::unexpected(
                http_error(errc::unavailable, resolve_error.message(), request.url.host));
        }
        auto socket = boost::corosio::tcp_socket{executor.context()};
        auto [connect_error, endpoint] = co_await boost::corosio::connect(socket, results);
        static_cast<void>(endpoint);
        if (connect_error) {
            co_return std::unexpected(
                http_error(errc::unavailable, connect_error.message(), request.url.host));
        }
        auto wire = make_request(request);
        if (!request.url.tls) {
            co_return co_await exchange_http(socket, std::move(wire), request.url.host);
        }
        auto context = boost::corosio::tls_context{};
        context.set_hostname(request.url.host);
        if (auto code = context.set_default_verify_paths(); code) {
            co_return std::unexpected(http_error(errc::io, code.message(), request.url.host));
        }
        if (auto code = context.set_verify_mode(boost::corosio::tls_verify_mode::peer); code) {
            co_return std::unexpected(http_error(errc::io, code.message(), request.url.host));
        }
        auto secure = boost::corosio::openssl_stream{&socket, context};
        auto [handshake_error] = co_await secure.handshake(boost::corosio::openssl_stream::client);
        if (handshake_error) {
            co_return std::unexpected(
                http_error(errc::unavailable, handshake_error.message(), request.url.host));
        }
        auto response = co_await exchange_http(secure, std::move(wire), request.url.host);
        auto [shutdown_error] = co_await secure.shutdown();
        if (shutdown_error && shutdown_error != boost::capy::error::eof && response) {
            co_return std::unexpected(
                http_error(errc::io, shutdown_error.message(), request.url.host));
        }
        co_return response;
    } catch (const std::exception& exception) {
        co_return std::unexpected(http_error(errc::internal, exception.what(), request.url.host));
    }
}

} // namespace liquid::net_detail
