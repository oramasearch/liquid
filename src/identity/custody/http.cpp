#include "http1/client.hpp"
#include "identity/custody/common.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <optional>

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

namespace liquid::custody_detail {
namespace {

struct parsed_url {
    bool tls{};
    std::string host;
    std::string port;
    std::string target;
};

auto lower(std::string_view value) -> std::string {
    auto output = std::string{value};
    std::ranges::transform(output, output.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return output;
}

auto parse_url(std::string_view input) -> result<parsed_url> {
    auto output = parsed_url{};
    if (input.starts_with("https://")) {
        output.tls = true;
        output.port = "443";
        input.remove_prefix(8);
    } else if (input.starts_with("http://")) {
        output.port = "80";
        input.remove_prefix(7);
    } else {
        return std::unexpected(custody_error(
            errc::invalid_argument, "custody URL must use HTTP or HTTPS", "custody URL"));
    }
    const auto slash = input.find('/');
    const auto authority = input.substr(0, slash);
    output.target = slash == std::string_view::npos ? "/" : std::string{input.substr(slash)};
    if (authority.empty() || authority.find('@') != std::string_view::npos) {
        return std::unexpected(
            custody_error(errc::invalid_argument, "invalid custody URL", "custody URL"));
    }
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) {
            return std::unexpected(
                custody_error(errc::invalid_argument, "invalid IPv6 custody URL", "custody URL"));
        }
        output.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') {
                return std::unexpected(custody_error(
                    errc::invalid_argument, "invalid custody URL port", "custody URL"));
            }
            output.port = authority.substr(close + 2);
        }
    } else {
        const auto colon = authority.rfind(':');
        if (colon == std::string_view::npos) {
            output.host = authority;
        } else {
            output.host = authority.substr(0, colon);
            output.port = authority.substr(colon + 1);
        }
    }
    if (output.host.empty() || output.port.empty()) {
        return std::unexpected(custody_error(
            errc::invalid_argument, "custody URL host or port is empty", "custody URL"));
    }
    return output;
}

auto header_value(std::string_view headers, std::string_view name) -> std::string_view {
    auto offset = headers.find("\r\n") + 2;
    while (offset < headers.size()) {
        const auto end = headers.find("\r\n", offset);
        if (end == std::string_view::npos || end == offset) {
            break;
        }
        const auto colon = headers.find(':', offset);
        if (colon != std::string_view::npos && colon < end &&
            lower(headers.substr(offset, colon - offset)) == lower(name)) {
            auto value = headers.substr(colon + 1, end - colon - 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.remove_prefix(1);
            }
            return value;
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
            return std::unexpected(
                custody_error(errc::corrupt, "truncated HTTP chunk size", "custody endpoint"));
        }
        auto size = std::uint64_t{};
        const auto size_end = input.find(';', offset);
        const auto end =
            size_end != std::string_view::npos && size_end < line_end ? size_end : line_end;
        const auto parsed = std::from_chars(input.data() + offset, input.data() + end, size, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != input.data() + end ||
            size > std::numeric_limits<std::size_t>::max()) {
            return std::unexpected(
                custody_error(errc::corrupt, "invalid HTTP chunk size", "custody endpoint"));
        }
        offset = line_end + 2;
        if (size == 0) {
            return output;
        }
        if (size > input.size() - offset || input.size() - offset - size < 2 ||
            input.substr(offset + size, 2) != "\r\n") {
            return std::unexpected(
                custody_error(errc::corrupt, "truncated HTTP chunk", "custody endpoint"));
        }
        const auto* first = reinterpret_cast<const std::byte*>(input.data() + offset);
        output.insert(output.end(), first, first + size);
        offset += static_cast<std::size_t>(size) + 2;
    }
}

auto parse_response(std::string wire) -> result<http_response> {
    const auto header_end = wire.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return std::unexpected(
            custody_error(errc::corrupt, "HTTP response header is truncated", "custody endpoint"));
    }
    const auto header_text = std::string_view{wire}.substr(0, header_end + 4);
    const auto header = boost::http::response{header_text};
    const auto body = std::string_view{wire}.substr(header_end + 4);
    auto output = http_response{.status = static_cast<unsigned>(header.status())};
    auto offset = header_text.find("\r\n") + 2;
    while (offset < header_text.size()) {
        const auto end = header_text.find("\r\n", offset);
        if (end == std::string_view::npos || end == offset) {
            break;
        }
        const auto colon = header_text.find(':', offset);
        if (colon != std::string_view::npos && colon < end) {
            auto value = header_text.substr(colon + 1, end - colon - 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.remove_prefix(1);
            }
            output.headers.emplace_back(lower(header_text.substr(offset, colon - offset)), value);
        }
        offset = end + 2;
    }
    if (lower(header_value(header_text, "transfer-encoding")) == "chunked") {
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
            return std::unexpected(
                custody_error(errc::corrupt, "invalid HTTP content length", "custody endpoint"));
        }
    }
    const auto* first = reinterpret_cast<const std::byte*>(body.data());
    output.body.assign(first, first + body.size());
    return output;
}

auto make_request(const parsed_url& url,
                  const http_headers& headers,
                  std::span<const std::byte> body) -> std::string {
    auto request = boost::http::request{boost::http::method::post, url.target};
    auto authority = url.host;
    if ((!url.tls && url.port != "80") || (url.tls && url.port != "443")) {
        authority += ':' + url.port;
    }
    request.set(boost::http::field::host, authority);
    request.set(boost::http::field::connection, "close");
    request.set_payload_size(body.size());
    for (const auto& [name, value] : headers) {
        request.set(name, value);
    }
    auto wire = std::string{request.buffer()};
    wire.append(reinterpret_cast<const char*>(body.data()), body.size());
    return wire;
}

template <boost::capy::Stream Stream>
auto exchange(Stream& stream, std::string wire) -> runtime::task<http_response> {
    auto [write_error, written] =
        co_await boost::capy::write(stream, boost::capy::const_buffer(wire.data(), wire.size()));
    if (write_error || written != wire.size()) {
        co_return std::unexpected(
            custody_error(errc::io,
                          write_error ? write_error.message() : "short HTTP write",
                          "custody endpoint"));
    }
    auto response = std::string{};
    auto [read_error, read] =
        co_await boost::capy::read(stream, boost::capy::string_dynamic_buffer(&response));
    static_cast<void>(read);
    if (read_error && read_error != boost::capy::error::eof) {
        co_return std::unexpected(
            custody_error(errc::io, read_error.message(), "custody endpoint"));
    }
    try {
        co_return parse_response(std::move(response));
    } catch (const std::exception& exception) {
        co_return std::unexpected(
            custody_error(errc::corrupt, exception.what(), "custody endpoint"));
    }
}

} // namespace

auto perform_http(std::string method,
                  std::string url,
                  http_headers headers,
                  std::vector<std::byte> body) -> runtime::task<http_response> {
    if (method != "POST") {
        co_return std::unexpected(
            custody_error(errc::unsupported, "unsupported custody HTTP method", std::move(method)));
    }
    auto parsed = parse_url(url);
    if (!parsed) {
        co_return std::unexpected(parsed.error());
    }
    // Every custody request carries key material (a data key to wrap, a wrapped key to open), so
    // plain http goes only where no network attacker sits between (L-10, liquid#392).
    if (!parsed->tls && !http1::plaintext_allowed(parsed->host)) {
        co_return std::unexpected(custody_error(
            errc::invalid_argument,
            "refusing plain http to custody endpoint " + parsed->host + ": use https",
            parsed->host));
    }
    try {
        const auto executor = co_await boost::capy::this_coro::executor;
        auto resolver = boost::corosio::resolver{executor.context()};
        auto [resolve_error, results] = co_await resolver.resolve(parsed->host, parsed->port);
        if (resolve_error) {
            co_return std::unexpected(
                custody_error(errc::unavailable, resolve_error.message(), parsed->host));
        }
        auto socket = boost::corosio::tcp_socket{executor.context()};
        auto [connect_error, endpoint] = co_await boost::corosio::connect(socket, results);
        static_cast<void>(endpoint);
        if (connect_error) {
            co_return std::unexpected(
                custody_error(errc::unavailable, connect_error.message(), parsed->host));
        }
        auto wire = make_request(*parsed, headers, body);
        if (!parsed->tls) {
            co_return co_await exchange(socket, std::move(wire));
        }
        auto context = boost::corosio::tls_context{};
        context.set_hostname(parsed->host);
        if (auto code = context.set_default_verify_paths(); code) {
            co_return std::unexpected(custody_error(errc::io, code.message(), parsed->host));
        }
        if (auto code = context.set_verify_mode(boost::corosio::tls_verify_mode::peer); code) {
            co_return std::unexpected(custody_error(errc::io, code.message(), parsed->host));
        }
        auto secure = boost::corosio::openssl_stream{&socket, context};
        auto [handshake_error] = co_await secure.handshake(boost::corosio::openssl_stream::client);
        if (handshake_error) {
            co_return std::unexpected(
                custody_error(errc::unavailable, handshake_error.message(), parsed->host));
        }
        auto response = co_await exchange(secure, std::move(wire));
        auto [shutdown_error] = co_await secure.shutdown();
        if (shutdown_error && shutdown_error != boost::capy::error::eof && response) {
            co_return std::unexpected(
                custody_error(errc::io, shutdown_error.message(), parsed->host));
        }
        co_return response;
    } catch (const std::exception& exception) {
        co_return std::unexpected(custody_error(errc::internal, exception.what(), parsed->host));
    }
}

} // namespace liquid::custody_detail
