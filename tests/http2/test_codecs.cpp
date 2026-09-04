#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <string>
#include <vector>

#include <boost/capy/ex/run_async.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/test/socket_pair.hpp>

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
    co_return {};
}

auto h2c_round_trip(boost::corosio::io_context& context,
                    boost::corosio::tcp_socket& server_socket,
                    boost::corosio::tcp_socket& client_socket,
                    liquid::result<void>* output) -> boost::capy::task<> {
    auto server_result = std::optional<liquid::result<void>>{};
    boost::capy::run_async(context.get_executor(), [&server_result](liquid::result<void> result) {
        server_result.emplace(std::move(result));
    })(server_session(server_socket));
    *output = co_await client_session(client_socket);
    if (*output && (!server_result || !*server_result)) {
        *output = std::unexpected(
            server_result
                ? server_result->error()
                : liquid::error{liquid::errc::internal, "server did not complete", "HTTP/2 test"});
    }
    co_return;
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
    auto [server_socket, client_socket] = boost::corosio::test::make_socket_pair(context);
    auto output = liquid::result<void>{};
    boost::capy::run_async(context.get_executor())(
        h2c_round_trip(context, server_socket, client_socket, &output));
    context.run();
    ASSERT_TRUE(output.has_value()) << (output ? "" : output.error().message);
}

} // namespace
