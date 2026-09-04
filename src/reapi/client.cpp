#include "reapi/client.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <limits>
#include <string>
#include <utility>

#include <boost/capy/io/any_stream.hpp>
#include <boost/corosio/connect.hpp>
#include <boost/corosio/openssl_stream.hpp>
#include <boost/corosio/resolver.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/tls_context.hpp>

#include "http2/connection.hpp"
#include "http2/grpc.hpp"

namespace liquid::reapi {
namespace {

struct parsed_endpoint {
    std::string host;
    std::string port;
    std::string authority;
    bool tls{};
};

auto rpc_error(errc code, std::string message, std::string context) -> error {
    return {code, std::move(message), std::move(context)};
}

auto parse_endpoint(const remote_config& config) -> result<parsed_endpoint> {
    auto endpoint = std::string_view{config.endpoint};
    auto scheme_tls = false;
    if (endpoint.starts_with("http://")) {
        endpoint.remove_prefix(7);
    } else if (endpoint.starts_with("https://")) {
        endpoint.remove_prefix(8);
        scheme_tls = true;
    }
    if (endpoint.empty() || endpoint.contains('/')) {
        return std::unexpected(rpc_error(
            errc::invalid_argument, "remote endpoint must be host:port", config.endpoint));
    }
    auto host = std::string_view{};
    auto port = std::string_view{};
    if (endpoint.front() == '[') {
        const auto close = endpoint.find(']');
        if (close == std::string_view::npos || close + 2 > endpoint.size() ||
            endpoint[close + 1] != ':') {
            return std::unexpected(
                rpc_error(errc::invalid_argument, "invalid IPv6 remote endpoint", config.endpoint));
        }
        host = endpoint.substr(1, close - 1);
        port = endpoint.substr(close + 2);
    } else {
        const auto separator = endpoint.rfind(':');
        if (separator == std::string_view::npos) {
            return std::unexpected(rpc_error(
                errc::invalid_argument, "remote endpoint requires a port", config.endpoint));
        }
        host = endpoint.substr(0, separator);
        port = endpoint.substr(separator + 1);
    }
    auto number = std::uint32_t{};
    const auto parsed = std::from_chars(port.data(), port.data() + port.size(), number);
    if (host.empty() || port.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != port.data() + port.size() || number == 0 || number > 65535) {
        return std::unexpected(
            rpc_error(errc::invalid_argument, "invalid remote endpoint", config.endpoint));
    }
    const auto tls = config.tls.enabled || scheme_tls;
    return parsed_endpoint{std::string{host}, std::string{port}, std::string{endpoint}, tls};
}

auto grpc_timeout(std::chrono::milliseconds value) -> std::string {
    const auto count = std::clamp<std::int64_t>(value.count(), 1, 99'999'999);
    return std::to_string(count) + "m";
}

auto status_code(std::span<const http2::header_field> headers) -> result<std::uint32_t> {
    const auto status =
        std::ranges::find(headers, std::string{"grpc-status"}, &http2::header_field::name);
    if (status == headers.end()) {
        return std::unexpected(
            rpc_error(errc::corrupt, "gRPC response has no terminal status", "REAPI client"));
    }
    const auto message =
        std::ranges::find(headers, std::string{"grpc-message"}, &http2::header_field::name);
    auto parsed = http2::parse_grpc_status(
        status->value, message == headers.end() ? std::string_view{} : message->value);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    if (parsed->code == 0) {
        return 0;
    }
    const auto code = parsed->code == 5    ? errc::not_found
                      : parsed->code == 7  ? errc::unauthorized
                      : parsed->code == 12 ? errc::unsupported
                                           : errc::unavailable;
    return std::unexpected(rpc_error(
        code,
        parsed->message.empty() ? std::format("gRPC status {}", parsed->code) : parsed->message,
        "REAPI client"));
}

auto configure_tls(boost::corosio::tls_context& context,
                   const parsed_endpoint& endpoint,
                   const remote_tls_config& config) -> result<void> {
    context.set_hostname(endpoint.host);
    if (const auto code = context.set_alpn({"h2"})) {
        return std::unexpected(rpc_error(errc::io, code.message(), "TLS ALPN"));
    }
    context.set_verify_mode(config.verify_peer ? boost::corosio::tls_verify_mode::peer
                                               : boost::corosio::tls_verify_mode::none);
    if (config.verify_peer) {
        const auto code = config.certificate_authority
                              ? context.load_verify_file(config.certificate_authority->string())
                              : context.set_default_verify_paths();
        if (code) {
            return std::unexpected(rpc_error(errc::io, code.message(), "TLS trust anchors"));
        }
    }
    if (config.client_certificate) {
        const auto code = context.use_certificate_chain_file(config.client_certificate->string());
        if (code) {
            return std::unexpected(rpc_error(errc::io, code.message(), "TLS certificate"));
        }
    }
    if (config.client_key) {
        const auto code = context.use_private_key_file(config.client_key->string(),
                                                       boost::corosio::tls_file_format::pem);
        if (code) {
            return std::unexpected(rpc_error(errc::io, code.message(), "TLS private key"));
        }
    }
    return {};
}

runtime::task<std::vector<std::vector<std::byte>>> exchange(boost::capy::any_stream& stream,
                                                            const remote_config& config,
                                                            const parsed_endpoint& endpoint,
                                                            std::string_view method,
                                                            std::span<const std::byte> request) {
    auto connection = http2::connection{stream, http2::endpoint_role::client};
    auto started = co_await connection.start();
    if (!started) {
        co_return std::unexpected(started.error());
    }
    auto headers = std::vector<http2::header_field>{
        {":method", "POST", false},
        {":scheme", endpoint.tls ? "https" : "http", false},
        {":path", std::string{method}, false},
        {":authority", endpoint.authority, false},
        {"content-type", "application/grpc", false},
        {"te", "trailers", false},
        {"grpc-timeout", grpc_timeout(config.deadline), false},
    };
    for (const auto& [name, value] : config.headers) {
        if (name.empty() || name.front() == ':' || name == "content-type" || name == "te") {
            co_return std::unexpected(
                rpc_error(errc::invalid_argument, "reserved or empty remote header", name));
        }
        const auto sensitive = name == "authorization" || name == "proxy-authorization" ||
                               name == "cookie" || name == "set-cookie";
        headers.push_back({name, value, sensitive});
    }
    auto stream_id = co_await connection.open_stream(headers, false);
    if (!stream_id) {
        co_return std::unexpected(stream_id.error());
    }
    auto envelope =
        http2::encode_grpc_message({false, std::vector<std::byte>{request.begin(), request.end()}});
    if (!envelope) {
        co_return std::unexpected(envelope.error());
    }
    auto sent = co_await connection.send_data(*stream_id, std::move(*envelope), true);
    if (!sent) {
        co_return std::unexpected(sent.error());
    }
    auto response_headers = co_await connection.receive_headers(*stream_id);
    if (!response_headers) {
        co_return std::unexpected(response_headers.error());
    }
    const auto http_status =
        std::ranges::find(*response_headers, std::string{":status"}, &http2::header_field::name);
    if (http_status == response_headers->end() || http_status->value != "200") {
        co_return std::unexpected(rpc_error(
            errc::unavailable, "REAPI peer returned a non-200 HTTP status", endpoint.authority));
    }
    const auto trailers_only = std::ranges::find(
        *response_headers, std::string{"grpc-status"}, &http2::header_field::name);
    if (trailers_only != response_headers->end()) {
        auto status = status_code(*response_headers);
        if (!status) {
            co_return std::unexpected(status.error());
        }
        co_return std::vector<std::vector<std::byte>>{};
    }
    auto body = co_await connection.receive_data(*stream_id);
    if (!body) {
        co_return std::unexpected(body.error());
    }
    auto messages = http2::decode_grpc_messages(*body);
    if (!messages) {
        co_return std::unexpected(messages.error());
    }
    auto output = std::vector<std::vector<std::byte>>{};
    output.reserve(messages->size());
    for (auto& message : *messages) {
        if (message.compressed) {
            co_return std::unexpected(rpc_error(
                errc::unsupported, "compressed gRPC messages are unsupported", endpoint.authority));
        }
        output.push_back(std::move(message.bytes));
    }
    auto trailers = co_await connection.receive_headers(*stream_id);
    if (!trailers) {
        co_return std::unexpected(trailers.error());
    }
    auto status = status_code(*trailers);
    if (!status) {
        co_return std::unexpected(status.error());
    }
    co_return output;
}

} // namespace

grpc_client::grpc_client(remote_config config) : config_(std::move(config)) {}

runtime::task<std::vector<std::vector<std::byte>>>
grpc_client::call(std::string_view method, std::span<const std::byte> request, bool) {
    auto owned_request = std::vector<std::byte>{request.begin(), request.end()};
    auto [deadline_error, outcome] = co_await runtime::timeout(
        runtime::as_io(call_unbounded(std::string{method}, std::move(owned_request))),
        config_.deadline);
    if (deadline_error) {
        co_return std::unexpected(rpc_error(errc::unavailable,
                                            "remote request deadline exceeded",
                                            std::string{method} + " at " + config_.endpoint));
    }
    co_return std::move(outcome);
}

runtime::task<std::vector<std::vector<std::byte>>>
grpc_client::call_unbounded(std::string method, std::vector<std::byte> request) {
    auto endpoint = parse_endpoint(config_);
    if (!endpoint) {
        co_return std::unexpected(endpoint.error());
    }
    const auto executor = co_await boost::capy::this_coro::executor;
    auto resolver = boost::corosio::resolver{executor.context()};
    auto [resolve_error, endpoints] = co_await resolver.resolve(endpoint->host, endpoint->port);
    if (resolve_error) {
        co_return std::unexpected(
            rpc_error(errc::unavailable, resolve_error.message(), endpoint->authority));
    }
    auto socket = boost::corosio::tcp_socket{executor.context()};
    auto [connect_error, connected] = co_await boost::corosio::connect(socket, endpoints);
    if (connect_error) {
        co_return std::unexpected(
            rpc_error(errc::unavailable, connect_error.message(), endpoint->authority));
    }
    if (!endpoint->tls) {
        auto stream = boost::capy::any_stream{&socket};
        co_return co_await exchange(stream, config_, *endpoint, method, request);
    }
#if defined(BOOST_COROSIO_HAS_OPENSSL)
    auto context = boost::corosio::tls_context{};
    auto configured = configure_tls(context, *endpoint, config_.tls);
    if (!configured) {
        co_return std::unexpected(configured.error());
    }
    auto secure = boost::corosio::openssl_stream{&socket, context};
    auto [handshake_error] = co_await secure.handshake(boost::corosio::tls_stream::client);
    if (handshake_error) {
        co_return std::unexpected(
            rpc_error(errc::unavailable, handshake_error.message(), endpoint->authority));
    }
    auto valid_protocol = http2::validate_tls_protocol(secure, true);
    if (!valid_protocol) {
        co_return std::unexpected(valid_protocol.error());
    }
    auto stream = boost::capy::any_stream{&secure};
    co_return co_await exchange(stream, config_, *endpoint, method, request);
#else
    co_return std::unexpected(rpc_error(
        errc::unsupported, "TLS support is unavailable in this build", endpoint->authority));
#endif
}

const remote_config& grpc_client::config() const noexcept {
    return config_;
}

} // namespace liquid::reapi
