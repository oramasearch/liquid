// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include <boost/capy/ex/run_async.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/openssl_stream.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/tls_context.hpp>

#include "http2/connection.hpp"
#include "http2/frame.hpp"
#include "http2/grpc.hpp"
#include "http2/hpack.hpp"

namespace {

auto bytes(std::initializer_list<unsigned char> input) -> std::vector<std::byte> {
    auto output = std::vector<std::byte>{};
    output.reserve(input.size());
    for (const auto value : input) {
        output.push_back(static_cast<std::byte>(value));
    }
    return output;
}

auto server_session(boost::corosio::tcp_socket& socket) -> liquid::runtime::task<void> {
    auto stream = boost::capy::any_stream(&socket);
    auto server = liquid::http2::connection{stream,
                                            liquid::http2::endpoint_role::server,
                                            liquid::http2::connection_options{1024, 16'384, 4096}};
    auto started = co_await server.start();
    if (!started) {
        co_return std::unexpected(started.error());
    }
    auto headers = co_await server.receive_headers(1);
    if (!headers) {
        co_return std::unexpected(headers.error());
    }
    auto body = co_await server.receive_data(1);
    if (!body) {
        co_return std::unexpected(body.error());
    }
    const auto response_headers =
        std::array{liquid::http2::header_field{":status", "200", false},
                   liquid::http2::header_field{"content-type", "application/grpc", false}};
    auto response = co_await server.send_headers(1, response_headers, false);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    auto sent_body = co_await server.send_data(1, std::move(*body), false);
    if (!sent_body) {
        co_return std::unexpected(sent_body.error());
    }
    const auto trailers = std::array{liquid::http2::header_field{"grpc-status", "0", false}};
    auto sent_trailers = co_await server.send_headers(1, trailers, true);
    if (!sent_trailers) {
        co_return std::unexpected(sent_trailers.error());
    }
    auto peer_goaway = co_await server.wait_for_goaway();
    if (!peer_goaway) {
        co_return std::unexpected(peer_goaway.error());
    }
    co_return {};
}

auto client_session(boost::corosio::tcp_socket& socket) -> liquid::runtime::task<void> {
    auto stream = boost::capy::any_stream(&socket);
    auto client = liquid::http2::connection{stream,
                                            liquid::http2::endpoint_role::client,
                                            liquid::http2::connection_options{1024, 16'384, 4096}};
    auto started = co_await client.start();
    if (!started) {
        co_return std::unexpected(started.error());
    }
    const auto request_headers = std::array{
        liquid::http2::header_field{":method", "POST", false},
        liquid::http2::header_field{":scheme", "http", false},
        liquid::http2::header_field{":path", "/grpc.health.v1.Health/Check", false},
        liquid::http2::header_field{"content-type", "application/grpc", false},
    };
    auto stream_id = co_await client.open_stream(request_headers, false);
    if (!stream_id) {
        co_return std::unexpected(stream_id.error());
    }
    auto sent_empty = co_await client.send_data(*stream_id, {}, false);
    if (!sent_empty) {
        co_return std::unexpected(sent_empty.error());
    }
    auto request = std::vector<std::byte>(4096, std::byte{'x'});
    auto sent = co_await client.send_data(*stream_id, std::move(request), true);
    if (!sent) {
        co_return std::unexpected(sent.error());
    }
    auto response_headers = co_await client.receive_headers(*stream_id);
    if (!response_headers) {
        co_return std::unexpected(response_headers.error());
    }
    auto response = co_await client.receive_data(*stream_id);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->size() != 4096) {
        co_return std::unexpected(liquid::error{
            liquid::errc::corrupt, "flow-controlled response is truncated", "HTTP/2 test"});
    }
    auto trailers = co_await client.receive_headers(*stream_id);
    if (!trailers) {
        co_return std::unexpected(trailers.error());
    }
    auto sent_goaway = co_await client.goaway();
    if (!sent_goaway) {
        co_return std::unexpected(sent_goaway.error());
    }
    co_return {};
}

auto accept_and_serve(boost::corosio::tcp_acceptor& acceptor, boost::corosio::io_context& context)
    -> liquid::runtime::task<void> {
    auto socket = boost::corosio::tcp_socket{context};
    const auto [accept_error] = co_await acceptor.accept(socket);
    if (accept_error) {
        co_return std::unexpected(
            liquid::error{liquid::errc::io, accept_error.message(), "HTTP/2 test accept"});
    }
    co_return co_await server_session(socket);
}

auto connect_and_run(boost::corosio::io_context& context, boost::corosio::endpoint endpoint)
    -> liquid::runtime::task<void> {
    auto socket = boost::corosio::tcp_socket{context};
    const auto [connect_error] = co_await socket.connect(endpoint);
    if (connect_error) {
        co_return std::unexpected(
            liquid::error{liquid::errc::io, connect_error.message(), "HTTP/2 test connect"});
    }
    co_return co_await client_session(socket);
}

auto query_capabilities(boost::capy::any_stream& stream,
                        std::string_view scheme,
                        std::string_view authority) -> liquid::runtime::task<void> {
    auto client = liquid::http2::connection{stream, liquid::http2::endpoint_role::client};
    auto started = co_await client.start();
    if (!started) {
        co_return std::unexpected(started.error());
    }
    const auto headers = std::array{
        liquid::http2::header_field{":method", "POST", false},
        liquid::http2::header_field{":scheme", std::string{scheme}, false},
        liquid::http2::header_field{
            ":path", "/build.bazel.remote.execution.v2.Capabilities/GetCapabilities", false},
        liquid::http2::header_field{":authority", std::string{authority}, false},
        liquid::http2::header_field{"content-type", "application/grpc", false},
        liquid::http2::header_field{"te", "trailers", false},
    };
    auto stream_id = co_await client.open_stream(headers, false);
    if (!stream_id) {
        co_return std::unexpected(stream_id.error());
    }
    const auto request = liquid::http2::encode_grpc_message({false, {}});
    if (!request) {
        co_return std::unexpected(request.error());
    }
    auto sent = co_await client.send_data(*stream_id, *request, true);
    if (!sent) {
        co_return std::unexpected(sent.error());
    }
    auto response_headers = co_await client.receive_headers(*stream_id);
    if (!response_headers) {
        co_return std::unexpected(response_headers.error());
    }
    auto response_body = co_await client.receive_data(*stream_id);
    if (!response_body) {
        co_return std::unexpected(response_body.error());
    }
    const auto response = liquid::http2::decode_grpc_messages(*response_body);
    if (!response || response->empty()) {
        co_return std::unexpected(liquid::error{liquid::errc::corrupt,
                                                "GetCapabilities returned no gRPC message",
                                                "NativeLink HTTP/2 peer"});
    }
    auto trailers = co_await client.receive_headers(*stream_id);
    if (!trailers) {
        co_return std::unexpected(trailers.error());
    }
    const auto status = std::find_if(trailers->begin(), trailers->end(), [](const auto& header) {
        return header.name == "grpc-status";
    });
    if (status == trailers->end() || status->value != "0") {
        co_return std::unexpected(liquid::error{liquid::errc::unavailable,
                                                "GetCapabilities did not finish with grpc-status 0",
                                                "NativeLink HTTP/2 peer"});
    }
    co_return {};
}

auto get_capabilities(boost::corosio::io_context& context, liquid::result<void>* output)
    -> boost::capy::task<> {
    auto socket = boost::corosio::tcp_socket{context};
    const auto [connect_error] = co_await socket.connect(
        boost::corosio::endpoint{boost::corosio::ipv4_address::loopback(), 50051});
    if (connect_error) {
        *output = std::unexpected(liquid::error{
            liquid::errc::unavailable, connect_error.message(), "NativeLink HTTP/2 peer"});
        co_return;
    }
    auto stream = boost::capy::any_stream{&socket};
    *output = co_await query_capabilities(stream, "http", "127.0.0.1:50051");
}

auto get_capabilities_over_tls(boost::corosio::io_context& context, liquid::result<void>* output)
    -> boost::capy::task<> {
    auto tls_context = boost::corosio::tls_context{};
    if (const auto error = tls_context.set_alpn({"h2"})) {
        *output = std::unexpected(
            liquid::error{liquid::errc::io, error.message(), "NativeLink TLS ALPN configuration"});
        co_return;
    }
    auto socket = boost::corosio::tcp_socket{context};
    const auto [connect_error] = co_await socket.connect(
        boost::corosio::endpoint{boost::corosio::ipv4_address::loopback(), 50053});
    if (connect_error) {
        *output = std::unexpected(liquid::error{
            liquid::errc::unavailable, connect_error.message(), "NativeLink TLS HTTP/2 peer"});
        co_return;
    }
    auto tls_stream = boost::corosio::openssl_stream{&socket, tls_context};
    const auto [handshake_error] =
        co_await tls_stream.handshake(boost::corosio::tls_stream::client);
    if (handshake_error) {
        *output = std::unexpected(liquid::error{
            liquid::errc::io, handshake_error.message(), "NativeLink TLS HTTP/2 handshake"});
        co_return;
    }
    const auto protocol = liquid::http2::validate_tls_protocol(tls_stream);
    if (!protocol) {
        *output = std::unexpected(protocol.error());
        co_return;
    }
    auto stream = boost::capy::any_stream{&tls_stream};
    *output = co_await query_capabilities(stream, "https", "127.0.0.1:50053");
}

TEST(Http2Frame, RoundTripsEnvelope) {
    const auto input = liquid::http2::frame{liquid::http2::frame_type::data,
                                            liquid::http2::flag_end_stream,
                                            3,
                                            bytes({0x01, 0x02, 0x03})};
    const auto encoded = liquid::http2::encode_frame(input);
    ASSERT_TRUE(encoded.has_value());
    const auto decoded = liquid::http2::decode_frame(*encoded);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->type, input.type);
    EXPECT_EQ(decoded->flags, input.flags);
    EXPECT_EQ(decoded->stream_id, input.stream_id);
    EXPECT_EQ(decoded->payload, input.payload);
}

TEST(Http2Hpack, DecodesRfcHuffmanExample) {
    const auto encoded =
        bytes({0xf1, 0xe3, 0xc2, 0xe5, 0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4, 0xff});
    const auto decoded = liquid::http2::huffman_decode(encoded);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, "www.example.com");
}

TEST(Http2Hpack, PreservesDynamicTableAcrossBlocks) {
    auto encoder = liquid::http2::hpack_encoder{};
    auto decoder = liquid::http2::hpack_decoder{};
    const auto headers =
        std::array{liquid::http2::header_field{":method", "POST", false},
                   liquid::http2::header_field{"content-type", "application/grpc", false},
                   liquid::http2::header_field{"x-liquid-test", "first", false}};
    const auto first = encoder.encode(headers);
    ASSERT_TRUE(first.has_value());
    const auto decoded_first = decoder.decode(*first);
    ASSERT_TRUE(decoded_first.has_value());
    EXPECT_EQ(*decoded_first,
              std::vector<liquid::http2::header_field>(headers.begin(), headers.end()));

    const auto second = encoder.encode(headers);
    ASSERT_TRUE(second.has_value());
    EXPECT_LT(second->size(), first->size());
    const auto decoded_second = decoder.decode(*second);
    ASSERT_TRUE(decoded_second.has_value());
    EXPECT_EQ(*decoded_second,
              std::vector<liquid::http2::header_field>(headers.begin(), headers.end()));
}

TEST(GrpcFraming, RoundTripsMessagesAndTrailerStatus) {
    const auto input = liquid::http2::grpc_message{false, bytes({0x08, 0x01, 0x12, 0x03})};
    const auto encoded = liquid::http2::encode_grpc_message(input);
    ASSERT_TRUE(encoded.has_value());
    const auto decoded = liquid::http2::decode_grpc_messages(*encoded);
    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->size(), 1U);
    EXPECT_EQ(decoded->front().bytes, input.bytes);
    const auto status = liquid::http2::parse_grpc_status("0", "ok");
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->code, 0U);
    EXPECT_EQ(status->message, "ok");
}

TEST(Http2Connection, ExchangesFlowControlledGrpcStyleStreamOverH2c) {
    auto context = boost::corosio::io_context{};
    auto acceptor = boost::corosio::tcp_acceptor{
        context, boost::corosio::endpoint{boost::corosio::ipv4_address::loopback(), 0}};
    auto server_result = std::optional<liquid::result<void>>{};
    auto client_result = std::optional<liquid::result<void>>{};
    boost::capy::run_async(context.get_executor(), [&server_result](liquid::result<void> result) {
        server_result.emplace(std::move(result));
    })(accept_and_serve(acceptor, context));
    boost::capy::run_async(context.get_executor(), [&client_result](liquid::result<void> result) {
        client_result.emplace(std::move(result));
    })(connect_and_run(context, acceptor.local_endpoint()));
    context.run();
    ASSERT_TRUE(server_result.has_value());
    ASSERT_TRUE(client_result.has_value());
    ASSERT_TRUE(*server_result) << server_result->error().message;
    ASSERT_TRUE(*client_result) << client_result->error().message;
}

TEST(Http2Interop, GetsCapabilitiesFromNativeLinkOverH2c) {
    if (std::getenv("LIQUID_HTTP2_NATIVE_LINK") == nullptr) {
        GTEST_SKIP()
            << "Docker peer unavailable: set LIQUID_HTTP2_NATIVE_LINK after starting NativeLink";
    }
    auto context = boost::corosio::io_context{};
    auto output = liquid::result<void>{};
    boost::capy::run_async(context.get_executor())(get_capabilities(context, &output));
    context.run();
    ASSERT_TRUE(output.has_value()) << (output ? "" : output.error().message);
}

TEST(Http2Interop, GetsCapabilitiesFromNativeLinkOverTls) {
    if (std::getenv("LIQUID_HTTP2_NATIVE_LINK_TLS") == nullptr) {
        GTEST_SKIP() << "Docker TLS peer unavailable: set LIQUID_HTTP2_NATIVE_LINK_TLS after "
                        "starting NativeLink";
    }
    auto context = boost::corosio::io_context{};
    auto output = liquid::result<void>{};
    boost::capy::run_async(context.get_executor())(get_capabilities_over_tls(context, &output));
    context.run();
    ASSERT_TRUE(output.has_value()) << (output ? "" : output.error().message);
}

} // namespace
