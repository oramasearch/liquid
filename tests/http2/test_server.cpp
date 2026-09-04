#include <array>
#include <iostream>
#include <vector>

#include <boost/capy/ex/run_async.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>

#include "http2/connection.hpp"

namespace {

auto serve_one(boost::corosio::tcp_acceptor& acceptor, boost::corosio::io_context& context)
    -> liquid::runtime::task<void> {
    auto socket = boost::corosio::tcp_socket{context};
    const auto [accept_error] = co_await acceptor.accept(socket);
    if (accept_error) {
        co_return std::unexpected(
            liquid::error{liquid::errc::io, accept_error.message(), "HTTP/2 test server accept"});
    }
    auto stream = boost::capy::any_stream{&socket};
    auto server = liquid::http2::connection{stream, liquid::http2::endpoint_role::server};
    auto started = co_await server.start();
    if (!started) {
        co_return std::unexpected(started.error());
    }
    auto headers = co_await server.receive_headers(1);
    if (!headers) {
        co_return std::unexpected(headers.error());
    }
    auto request = co_await server.receive_data(1);
    if (!request) {
        co_return std::unexpected(request.error());
    }
    const auto response_headers =
        std::array{liquid::http2::header_field{":status", "200", false},
                   liquid::http2::header_field{"content-type", "application/grpc", false}};
    auto sent_headers = co_await server.send_headers(1, response_headers, false);
    if (!sent_headers) {
        co_return std::unexpected(sent_headers.error());
    }
    auto sent_body = co_await server.send_data(1, std::move(*request), false);
    if (!sent_body) {
        co_return std::unexpected(sent_body.error());
    }
    const auto trailers = std::array{liquid::http2::header_field{"grpc-status", "0", false}};
    auto sent_trailers = co_await server.send_headers(1, trailers, true);
    if (!sent_trailers) {
        co_return std::unexpected(sent_trailers.error());
    }
    co_return {};
}

} // namespace

int main() {
    auto context = boost::corosio::io_context{};
    auto acceptor = boost::corosio::tcp_acceptor{
        context, boost::corosio::endpoint{boost::corosio::ipv4_address::loopback(), 50052}};
    auto output = liquid::result<void>{};
    std::cout << "READY" << std::endl;
    boost::capy::run_async(context.get_executor(), [&output](liquid::result<void> result) {
        output = std::move(result);
    })(serve_one(acceptor, context));
    context.run();
    if (!output) {
        std::cerr << output.error().message << '\n';
        return 1;
    }
    return 0;
}
