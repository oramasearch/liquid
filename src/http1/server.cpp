#include "http1/server.hpp"

#include <array>
#include <expected>
#include <memory>
#include <print>
#include <system_error>
#include <utility>

#include <boost/capy/buffers.hpp>
#include <boost/capy/buffers/buffer_copy.hpp>
#include <boost/capy/buffers/vector_dynamic_buffer.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/read.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/openssl_stream.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/tls_context.hpp>
#include <boost/http/server/http_worker.hpp>

namespace liquid::http1 {
namespace {

auto server_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

template <boost::capy::Stream Stream>
auto http_session(Stream& stream,
                  router routes,
                  boost::http::shared_parser_config parser,
                  boost::http::shared_serializer_config serializer) -> runtime::task<void> {
    auto worker = boost::http::http_worker{
        stream, std::move(routes), std::move(parser), std::move(serializer)};
    co_await worker.do_http_session();
    co_return result<void>{};
}

auto session(std::shared_ptr<boost::corosio::tcp_socket> socket,
             std::shared_ptr<boost::corosio::tls_context> tls,
             router routes,
             boost::http::shared_parser_config parser,
             boost::http::shared_serializer_config serializer) -> runtime::task<void> {
    if (!tls) {
        co_return co_await http_session(
            *socket, std::move(routes), std::move(parser), std::move(serializer));
    }
    auto secure = boost::corosio::openssl_stream{socket.get(), *tls};
    auto [handshake_error] = co_await secure.handshake(boost::corosio::openssl_stream::server);
    if (handshake_error) {
        co_return std::unexpected(
            server_error(errc::io, handshake_error.message(), "HTTP/1.1 TLS handshake"));
    }
    co_return co_await http_session(
        secure, std::move(routes), std::move(parser), std::move(serializer));
}

auto detached(runtime::task<void> operation) -> boost::capy::task<> {
    auto outcome = co_await std::move(operation);
    if (!outcome && outcome.error().message != "End of file" && outcome.error().message != "eof") {
        std::println(stderr,
                     "HTTP/1.1 connection error: {} ({})",
                     outcome.error().message,
                     outcome.error().context);
    }
    co_return;
}

} // namespace

boost::capy::task<std::expected<std::vector<std::byte>, std::error_code>>
read_body(boost::http::route_params& params) {
    auto bytes = std::vector<unsigned char>{};
    auto [error, size] =
        co_await boost::capy::read(params.req_body, boost::capy::dynamic_buffer(bytes));
    static_cast<void>(size);
    if (error) {
        co_return std::unexpected(error);
    }
    auto output = std::vector<std::byte>(bytes.size());
    for (auto index = std::size_t{}; index < bytes.size(); ++index) {
        output[index] = std::byte{bytes[index]};
    }
    co_return output;
}

boost::http::route_task send(boost::http::route_params& params,
                             boost::http::status status,
                             std::string content_type,
                             std::string body) {
    params.res.set_status(status);
    params.res.set_keep_alive(false);
    params.res.set(boost::http::field::content_type, content_type);
    params.res.set_payload_size(body.size());
    auto offset = std::size_t{};
    for (;;) {
        auto buffers = std::array<boost::capy::mutable_buffer, 16>{};
        const auto output = params.res_body.prepare(buffers);
        const auto remaining = std::string_view{body}.substr(offset);
        const auto copied = boost::capy::buffer_copy(
            output, boost::capy::const_buffer(remaining.data(), remaining.size()));
        offset += copied;
        if (offset == body.size()) {
            auto [error] = co_await params.res_body.commit_eof(copied);
            if (error) {
                co_return boost::http::route_error(error);
            }
            co_return boost::http::route_done;
        }
        auto [error] = co_await params.res_body.commit(copied);
        if (error) {
            co_return boost::http::route_error(error);
        }
    }
}

runtime::task<void> serve(server_options options, router routes) {
    auto endpoint = boost::corosio::endpoint{};
    if (const auto code = boost::corosio::parse_endpoint(options.listen, endpoint)) {
        co_return std::unexpected(
            server_error(errc::invalid_argument, code.message(), options.listen));
    }
    if (endpoint.port() == 0) {
        co_return std::unexpected(
            server_error(errc::invalid_argument, "HTTP listen port must be non-zero"));
    }
    if (options.tls_certificate.has_value() != options.tls_key.has_value()) {
        co_return std::unexpected(server_error(
            errc::invalid_argument, "TLS certificate and key must be provided together"));
    }
    auto tls = std::shared_ptr<boost::corosio::tls_context>{};
    if (options.tls_certificate) {
        tls = std::make_shared<boost::corosio::tls_context>();
        if (const auto code = tls->use_certificate_chain_file(options.tls_certificate->string())) {
            co_return std::unexpected(server_error(
                errc::invalid_argument, code.message(), options.tls_certificate->string()));
        }
        if (const auto code = tls->use_private_key_file(options.tls_key->string(),
                                                        boost::corosio::tls_file_format::pem)) {
            co_return std::unexpected(
                server_error(errc::invalid_argument, code.message(), options.tls_key->string()));
        }
    }
    auto parser_options = boost::http::parser_config{true};
    parser_options.body_limit = options.body_limit;
    auto parser = boost::http::make_parser_config(parser_options);
    auto serializer = boost::http::make_serializer_config(boost::http::serializer_config{});
    try {
        const auto executor = co_await boost::capy::this_coro::executor;
        auto acceptor = boost::corosio::tcp_acceptor{executor.context(), endpoint};
        for (;;) {
            auto socket = std::make_shared<boost::corosio::tcp_socket>(executor.context());
            const auto [accept_error] = co_await acceptor.accept(*socket);
            if (accept_error) {
                co_return std::unexpected(
                    server_error(errc::io, accept_error.message(), options.listen));
            }
            boost::capy::run_async(executor)(
                detached(session(std::move(socket), tls, routes, parser, serializer)));
        }
    } catch (const std::exception& exception) {
        co_return std::unexpected(
            server_error(errc::io, exception.what(), std::move(options.listen)));
    }
}

} // namespace liquid::http1
