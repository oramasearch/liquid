#pragma once

#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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
#include <nlohmann/json.hpp>

#include "liquid/error.hpp"
#include "liquid/runtime.hpp"

namespace liquid::net_detail {

struct parsed_url {
    bool tls{};
    std::string host;
    std::string port;
    std::string path;
};

struct http_response {
    unsigned status{};
    std::vector<std::byte> body;
};

inline auto http_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

inline auto parse_url(std::string_view input, std::string_view context) -> result<parsed_url> {
    auto output = parsed_url{};
    if (input.starts_with("https://")) {
        output.tls = true;
        output.port = "443";
        input.remove_prefix(8);
    } else if (input.starts_with("http://")) {
        output.port = "80";
        input.remove_prefix(7);
    } else {
        return std::unexpected(http_error(errc::invalid_argument,
                                          std::string{context} + " must use http or https",
                                          std::string{context}));
    }
    const auto slash = input.find('/');
    const auto authority = input.substr(0, slash);
    output.path =
        slash == std::string_view::npos ? std::string{} : std::string{input.substr(slash)};
    while (output.path.size() > 1 && output.path.ends_with('/')) {
        output.path.pop_back();
    }
    if (authority.empty() || authority.find('@') != std::string_view::npos) {
        return std::unexpected(http_error(errc::invalid_argument,
                                          "invalid " + std::string{context} + " authority",
                                          std::string{context}));
    }
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) {
            return std::unexpected(http_error(errc::invalid_argument,
                                              "invalid IPv6 " + std::string{context},
                                              std::string{context}));
        }
        output.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') {
                return std::unexpected(http_error(errc::invalid_argument,
                                                  "invalid " + std::string{context} + " port",
                                                  std::string{context}));
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
        return std::unexpected(http_error(errc::invalid_argument,
                                          std::string{context} + " host or port is empty",
                                          std::string{context}));
    }
    if (output.path.empty()) {
        output.path = "/";
    }
    return output;
}

inline auto header_value(std::string_view header, std::string_view name) -> std::string_view {
    auto offset = header.find("\r\n") + 2;
    while (offset < header.size()) {
        const auto end = header.find("\r\n", offset);
        if (end == offset || end == std::string_view::npos) {
            break;
        }
        const auto colon = header.find(':', offset);
        if (colon != std::string_view::npos && colon < end) {
            const auto field = header.substr(offset, colon - offset);
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

inline auto decode_chunked(std::string_view input) -> result<std::vector<std::byte>> {
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

inline auto parse_response(std::string wire) -> result<http_response> {
    const auto header_end = wire.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return std::unexpected(http_error(errc::corrupt, "HTTP response header is truncated"));
    }
    const auto header_text = std::string_view{wire}.substr(0, header_end + 4);
    const auto header = boost::http::response{header_text};
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

inline auto make_request(boost::http::method method,
                         const parsed_url& url,
                         std::string_view target,
                         std::string_view token,
                         std::string_view content_type,
                         std::span<const std::byte> body) -> std::string {
    auto request = boost::http::request{method, target};
    auto authority = url.host;
    if ((!url.tls && url.port != "80") || (url.tls && url.port != "443")) {
        authority += ':' + url.port;
    }
    request.set(boost::http::field::host, authority);
    request.set(boost::http::field::connection, "close");
    if (!content_type.empty()) {
        request.set(boost::http::field::content_type, content_type);
    }
    request.set_payload_size(body.size());
    if (!token.empty()) {
        request.set(boost::http::field::authorization, "Bearer " + std::string{token});
    }
    auto wire = std::string{request.buffer()};
    wire.append(reinterpret_cast<const char*>(body.data()), body.size());
    return wire;
}

template <boost::capy::Stream Stream>
inline auto exchange_http(Stream& stream, std::string wire, std::string context)
    -> runtime::task<http_response> {
    auto [write_error, written] =
        co_await boost::capy::write(stream, boost::capy::const_buffer(wire.data(), wire.size()));
    if (write_error || written != wire.size()) {
        co_return std::unexpected(
            http_error(errc::io,
                       write_error ? write_error.message() : "short HTTP write",
                       std::move(context)));
    }
    auto response = std::string{};
    auto [read_error, read] =
        co_await boost::capy::read(stream, boost::capy::string_dynamic_buffer(&response));
    static_cast<void>(read);
    if (read_error && read_error != boost::capy::error::eof) {
        co_return std::unexpected(http_error(errc::io, read_error.message(), std::move(context)));
    }
    try {
        co_return parse_response(std::move(response));
    } catch (const std::exception& exception) {
        co_return std::unexpected(http_error(errc::corrupt, exception.what(), std::move(context)));
    }
}

inline auto perform_request(const parsed_url& url,
                            std::string_view token,
                            boost::http::method method,
                            std::string target,
                            std::string_view content_type,
                            std::span<const std::byte> body,
                            std::string context) -> runtime::task<http_response> {
    try {
        const auto executor = co_await boost::capy::this_coro::executor;
        auto resolver = boost::corosio::resolver{executor.context()};
        auto [resolve_error, results] = co_await resolver.resolve(url.host, url.port);
        if (resolve_error) {
            co_return std::unexpected(
                http_error(errc::unavailable, resolve_error.message(), url.host));
        }
        auto socket = boost::corosio::tcp_socket{executor.context()};
        auto [connect_error, endpoint] = co_await boost::corosio::connect(socket, results);
        static_cast<void>(endpoint);
        if (connect_error) {
            co_return std::unexpected(
                http_error(errc::unavailable, connect_error.message(), url.host));
        }
        auto wire = make_request(method, url, target, token, content_type, body);
        if (!url.tls) {
            co_return co_await exchange_http(socket, std::move(wire), std::move(context));
        }
        auto tls = boost::corosio::tls_context{};
        tls.set_hostname(url.host);
        const auto* certificate_file = std::getenv("SSL_CERT_FILE");
        const auto trust_error = certificate_file != nullptr && certificate_file[0] != '\0'
                                     ? tls.load_verify_file(certificate_file)
                                     : tls.set_default_verify_paths();
        if (trust_error) {
            co_return std::unexpected(http_error(errc::io, trust_error.message(), url.host));
        }
        if (auto code = tls.set_verify_mode(boost::corosio::tls_verify_mode::peer); code) {
            co_return std::unexpected(http_error(errc::io, code.message(), url.host));
        }
        auto secure = boost::corosio::openssl_stream{&socket, tls};
        auto [handshake_error] = co_await secure.handshake(boost::corosio::openssl_stream::client);
        if (handshake_error) {
            co_return std::unexpected(
                http_error(errc::unavailable, handshake_error.message(), url.host));
        }
        auto response = co_await exchange_http(secure, std::move(wire), std::move(context));
        auto [shutdown_error] = co_await secure.shutdown();
        if (shutdown_error && shutdown_error != boost::capy::error::eof && response) {
            co_return std::unexpected(http_error(errc::io, shutdown_error.message(), url.host));
        }
        co_return response;
    } catch (const std::exception& exception) {
        co_return std::unexpected(http_error(errc::internal, exception.what(), url.host));
    }
}

inline auto json_request(const parsed_url& url,
                         std::string_view token,
                         boost::http::method method,
                         std::string target,
                         const nlohmann::json& input,
                         std::string context) -> runtime::task<nlohmann::json> {
    auto encoded = std::string{};
    if (method != boost::http::method::get) {
        encoded = input.dump();
    }
    const auto* first = reinterpret_cast<const std::byte*>(encoded.data());
    auto response = co_await perform_request(url,
                                             token,
                                             method,
                                             std::move(target),
                                             "application/json",
                                             std::span{first, encoded.size()},
                                             context);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    try {
        const auto text = std::string_view{reinterpret_cast<const char*>(response->body.data()),
                                           response->body.size()};
        auto envelope = nlohmann::json::parse(text);
        if (!envelope.is_object() || !envelope.contains("ok") || !envelope.at("ok").is_boolean()) {
            co_return std::unexpected(
                http_error(errc::corrupt, "invalid JSON response envelope", std::move(context)));
        }
        if (envelope.at("ok").get<bool>()) {
            if (!envelope.contains("value")) {
                co_return std::unexpected(http_error(
                    errc::corrupt, "successful JSON envelope has no value", std::move(context)));
            }
            co_return envelope.at("value");
        }
        const auto failure = envelope.value("error", nlohmann::json::object());
        const auto code_name = failure.value("code", "internal");
        auto code = errc::internal;
        if (code_name == "invalid_input") {
            code = errc::invalid_argument;
        } else if (code_name == "unauthorized" || code_name == "mfa_required") {
            code = errc::unauthorized;
        } else if (code_name == "not_found") {
            code = errc::not_found;
        } else if (code_name == "conflict") {
            code = errc::conflict;
        } else if (code_name == "store_unavailable") {
            code = errc::unavailable;
        }
        co_return std::unexpected(http_error(code,
                                             failure.value("message", "lake request failed"),
                                             failure.value("path", std::string{})));
    } catch (const nlohmann::json::exception& exception) {
        co_return std::unexpected(http_error(errc::corrupt, exception.what(), std::move(context)));
    }
}

} // namespace liquid::net_detail
