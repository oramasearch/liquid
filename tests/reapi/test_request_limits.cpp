// SPDX-License-Identifier: MIT

// liquid#451: the REAPI server never reads a request stream whole. A unary request is one message
// within a limit, and a ByteStream Write is applied one bounded message at a time.

#include <gtest/gtest.h>

#if defined(LIQUID_CLI) && !defined(_WIN32)

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <boost/capy/io/any_stream.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/tcp_socket.hpp>

#include "../temporary_directory.hpp"
#include "http2/connection.hpp"
#include "http2/grpc.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "reapi/client.hpp"
#include "reapi/conversion.hpp"
#include "reapi/messages.hpp"

namespace {

using namespace std::chrono_literals;

constexpr auto find_missing_method =
    "/build.bazel.remote.execution.v2.ContentAddressableStorage/FindMissingBlobs";
constexpr auto write_method = "/google.bytestream.ByteStream/Write";
constexpr auto query_write_method = "/google.bytestream.ByteStream/QueryWriteStatus";

// The server's limit for one message: the 4 MiB batch size it advertises plus 1 MiB.
constexpr auto message_limit = std::size_t{5} * 1024 * 1024;

// A gRPC message prefix that declares `size` bytes of uncompressed message.
auto prefix(std::uint32_t size) -> std::vector<std::byte> {
    return {std::byte{0},
            static_cast<std::byte>((size >> 24U) & 0xffU),
            static_cast<std::byte>((size >> 16U) & 0xffU),
            static_cast<std::byte>((size >> 8U) & 0xffU),
            static_cast<std::byte>(size & 0xffU)};
}

class ReapiRequestLimits : public testing::Test {
protected:
    liquid::test::detail::process_fixture_directory root;
    std::unique_ptr<liquid::reapi::grpc_client> client;
    pid_t pid{-1};
    std::string listen;

    void SetUp() override {
        ASSERT_TRUE(liquid::runtime::block_on(liquid::repository::create(root.path())));
        const auto socket = ::socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(socket, 0);
        auto address = sockaddr_in{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ASSERT_EQ(::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
        auto length = static_cast<socklen_t>(sizeof(address));
        ASSERT_EQ(::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length), 0);
        ASSERT_EQ(::close(socket), 0);
        listen = "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
        pid = ::fork();
        ASSERT_GE(pid, 0);
        if (pid == 0) {
            if (::chdir(root.path().c_str()) != 0) {
                std::_Exit(126);
            }
            ::execl(LIQUID_CLI, LIQUID_CLI, "serve", "reapi", "--listen", listen.c_str(), nullptr);
            std::_Exit(127);
        }
        client = std::make_unique<liquid::reapi::grpc_client>(
            liquid::remote_config{.endpoint = listen, .deadline = 10s});
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (serving()) {
                return;
            }
            std::this_thread::sleep_for(10ms);
        }
        FAIL() << "REAPI server failed to start";
    }

    void TearDown() override {
        client.reset();
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            auto status = 0;
            ::waitpid(pid, &status, 0);
        }
    }

    // A unary call the server answers: QueryWriteStatus for an unknown upload is NOT_FOUND.
    auto serving() -> bool {
        auto encoded = liquid::reapi::encode(liquid::reapi::QueryWriteStatusRequest{
            .resource_name = "uploads/probe/blobs/" + std::string(64, 'a') + "/1"});
        auto response =
            liquid::runtime::block_on(client->call(query_write_method, encoded.value()));
        return !response && response.error().code == liquid::errc::not_found;
    }

    // Sends `body` on one stream of a new connection, ending the stream only when `end` is set,
    // and returns the gRPC status the server answers with.
    liquid::runtime::task<unsigned> raw_call(std::string method,
                                             std::vector<std::byte> body,
                                             bool end) {
        const auto executor = co_await boost::capy::this_coro::executor;
        auto endpoint = boost::corosio::endpoint{};
        if (const auto code = boost::corosio::parse_endpoint(listen, endpoint)) {
            co_return std::unexpected(liquid::error{liquid::errc::io, code.message(), {}});
        }
        auto socket = boost::corosio::tcp_socket{executor.context()};
        const auto [code] = co_await socket.connect(endpoint);
        if (code) {
            co_return std::unexpected(liquid::error{liquid::errc::io, code.message(), {}});
        }
        auto transport = boost::capy::any_stream{&socket};
        auto connection =
            liquid::http2::connection{transport, liquid::http2::endpoint_role::client};
        auto started = co_await connection.start();
        if (!started) {
            co_return std::unexpected(started.error());
        }
        const auto headers =
            std::vector<liquid::http2::header_field>{{":method", "POST", false},
                                                     {":scheme", "http", false},
                                                     {":authority", listen, false},
                                                     {":path", method, false},
                                                     {"content-type", "application/grpc", false}};
        auto stream = co_await connection.open_stream(headers, false);
        if (!stream) {
            co_return std::unexpected(stream.error());
        }
        auto sent = co_await connection.send_data(*stream, std::move(body), end);
        if (!sent) {
            co_return std::unexpected(sent.error());
        }
        auto response = co_await connection.receive_headers(*stream);
        auto data = co_await connection.receive_data(*stream);
        auto trailers = co_await connection.receive_headers(*stream);
        if (!response || !data || !trailers) {
            co_return std::unexpected(liquid::error{liquid::errc::io, "missing RPC response", {}});
        }
        for (const auto& field : *trailers) {
            if (field.name == "grpc-status") {
                auto status = liquid::http2::parse_grpc_status(field.value, {});
                if (!status) {
                    co_return std::unexpected(status.error());
                }
                co_return status->code;
            }
        }
        co_return std::unexpected(liquid::error{liquid::errc::corrupt, "missing gRPC status", {}});
    }

    liquid::runtime::task<unsigned> bounded_call(std::string method,
                                                 std::vector<std::byte> body,
                                                 bool end) {
        auto [deadline_error, outcome] = co_await liquid::runtime::timeout(
            liquid::runtime::as_io(raw_call(std::move(method), std::move(body), end)), 10s);
        if (deadline_error) {
            co_return std::unexpected(
                liquid::error{liquid::errc::unavailable, "RPC timed out", {}});
        }
        co_return std::move(outcome);
    }

    auto call(std::string method, std::vector<std::byte> body, bool end = true)
        -> liquid::result<unsigned> {
        return liquid::runtime::block_on(bounded_call(std::move(method), std::move(body), end));
    }
};

} // namespace

TEST_F(ReapiRequestLimits, an_oversize_unary_request_is_refused_from_its_prefix) {
    // Only the prefix and a few bytes are sent, and the stream is left open: the server answers
    // RESOURCE_EXHAUSTED without waiting for, or holding, the declared body.
    auto body = prefix(static_cast<std::uint32_t>(message_limit + 1));
    body.resize(body.size() + 64, std::byte{0x0a});
    auto status = call(find_missing_method, std::move(body), false);
    ASSERT_TRUE(status) << status.error().message;
    EXPECT_EQ(*status, 8U);
    // The connection was closed; the server still answers new ones.
    EXPECT_TRUE(serving());
}

TEST_F(ReapiRequestLimits, an_oversize_bytestream_write_message_is_refused_from_its_prefix) {
    auto body = prefix(static_cast<std::uint32_t>(message_limit + 1));
    body.resize(body.size() + 64, std::byte{0x0a});
    auto status = call(write_method, std::move(body), false);
    ASSERT_TRUE(status) << status.error().message;
    EXPECT_EQ(*status, 8U);
    EXPECT_TRUE(serving());
}

TEST_F(ReapiRequestLimits, a_write_larger_than_one_message_is_stored_from_bounded_messages) {
    // Seven 1 MiB messages on one stream: more than one message may hold, applied one at a time.
    auto contents = std::vector<std::byte>(std::size_t{7} * 1024 * 1024);
    for (auto index = std::size_t{}; index < contents.size(); ++index) {
        contents[index] = static_cast<std::byte>((index * 31U) & 0xffU);
    }
    auto digest = liquid::reapi::make_digest(liquid::algorithm::sha256, contents);
    ASSERT_TRUE(digest) << digest.error().message;
    const auto resource = "uploads/large/blobs/" + digest->hash + "/" +
                          std::to_string(static_cast<std::int64_t>(contents.size()));
    const auto chunk = std::size_t{1024} * 1024;
    auto body = std::vector<std::byte>{};
    for (auto offset = std::size_t{}; offset < contents.size(); offset += chunk) {
        const auto begin = contents.begin() + static_cast<std::ptrdiff_t>(offset);
        auto encoded = liquid::reapi::encode(liquid::reapi::WriteRequest{
            .resource_name = offset == 0 ? resource : std::string{},
            .write_offset = static_cast<std::int64_t>(offset),
            .finish_write = offset + chunk >= contents.size(),
            .data = std::vector<std::byte>{begin, begin + static_cast<std::ptrdiff_t>(chunk)}});
        ASSERT_TRUE(encoded) << encoded.error().message;
        auto envelope = liquid::http2::encode_grpc_message({false, std::move(*encoded)});
        ASSERT_TRUE(envelope) << envelope.error().message;
        body.insert(body.end(), envelope->begin(), envelope->end());
    }
    auto status = call(write_method, std::move(body));
    ASSERT_TRUE(status) << status.error().message;
    EXPECT_EQ(*status, 0U);

    // The same upload as one 7 MiB message is refused.
    auto single = liquid::reapi::encode(liquid::reapi::WriteRequest{
        .resource_name = "uploads/single/blobs/" + digest->hash + "/" +
                         std::to_string(static_cast<std::int64_t>(contents.size())),
        .write_offset = 0,
        .finish_write = true,
        .data = contents});
    ASSERT_TRUE(single) << single.error().message;
    auto envelope = liquid::http2::encode_grpc_message({false, std::move(*single)});
    ASSERT_TRUE(envelope) << envelope.error().message;
    envelope->resize(1024);
    auto refused = call(write_method, std::move(*envelope), false);
    ASSERT_TRUE(refused) << refused.error().message;
    EXPECT_EQ(*refused, 8U);
}

TEST_F(ReapiRequestLimits, the_client_uploads_a_large_blob_as_bounded_messages) {
    // grpc_client::call_messages sends each request as its own message on one stream, which is
    // how remote_client::write sends a blob above the batch limit.
    auto contents = std::vector<std::byte>(std::size_t{6} * 1024 * 1024 + 17, std::byte{0x5a});
    auto digest = liquid::reapi::make_digest(liquid::algorithm::sha256, contents);
    ASSERT_TRUE(digest) << digest.error().message;
    const auto resource = "uploads/client/blobs/" + digest->hash + "/" +
                          std::to_string(static_cast<std::int64_t>(contents.size()));
    const auto chunk = std::size_t{1024} * 1024;
    auto requests = std::vector<std::vector<std::byte>>{};
    for (auto offset = std::size_t{}; offset < contents.size(); offset += chunk) {
        const auto count = std::min(chunk, contents.size() - offset);
        const auto begin = contents.begin() + static_cast<std::ptrdiff_t>(offset);
        auto encoded = liquid::reapi::encode(liquid::reapi::WriteRequest{
            .resource_name = offset == 0 ? resource : std::string{},
            .write_offset = static_cast<std::int64_t>(offset),
            .finish_write = offset + count == contents.size(),
            .data = std::vector<std::byte>{begin, begin + static_cast<std::ptrdiff_t>(count)}});
        ASSERT_TRUE(encoded) << encoded.error().message;
        requests.push_back(std::move(*encoded));
    }
    auto messages =
        liquid::runtime::block_on(client->call_messages(write_method, std::move(requests)));
    ASSERT_TRUE(messages) << messages.error().message;
    ASSERT_EQ(messages->size(), 1U);
    auto response =
        liquid::reapi::decode_message<liquid::reapi::WriteResponse>(messages->front());
    ASSERT_TRUE(response) << response.error().message;
    EXPECT_EQ(response->committed_size, static_cast<std::int64_t>(contents.size()));
}

#endif
