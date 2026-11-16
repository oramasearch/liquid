#include <gtest/gtest.h>

#include <chrono>
#include <limits>
#include <string>
#include <thread>

#include "cli/reapi_options.hpp"
#include "reapi/uploads.hpp"

namespace {
using namespace std::chrono_literals;
using liquid::reapi::partial_uploads;

auto bytes(std::size_t count) -> std::vector<std::byte> {
    return std::vector<std::byte>(count, std::byte{'x'});
}

auto limits() -> liquid::reapi_upload_limits {
    return {.max_upload_bytes = 16,
            .max_buffered_bytes = 24,
            .max_uploads = 2,
            .max_resource_bytes = 128,
            .idle_timeout = 10s};
}
} // namespace

TEST(ReapiUploads, RejectsInvalidLimits) {
    EXPECT_TRUE(liquid::reapi::validate_upload_limits({}));
    auto policy = limits();
    policy.max_uploads = 0;
    EXPECT_FALSE(liquid::reapi::validate_upload_limits(policy));
    policy = limits();
    policy.max_buffered_bytes = 0;
    EXPECT_FALSE(liquid::reapi::validate_upload_limits(policy));
    policy = limits();
    policy.idle_timeout = 0ms;
    EXPECT_FALSE(liquid::reapi::validate_upload_limits(policy));
}

TEST(ReapiUploads, BoundsDeclaredSizeResourceAndActualBytesBeforeRetention) {
    auto uploads = partial_uploads{limits()};
    EXPECT_FALSE(uploads.begin("large", 17));
    EXPECT_FALSE(uploads.begin("negative", -1));
    EXPECT_FALSE(uploads.begin(std::string(129, 'a'), 1));
    auto started = uploads.begin("small", 4);
    ASSERT_TRUE(started);
    EXPECT_FALSE((*started)->append(0, bytes(5)));
    EXPECT_EQ(*uploads.committed("small"), 0);
    started->reset();
    EXPECT_FALSE(uploads.committed("small"));
}

TEST(ReapiUploads, CountsEmptyUploadsAndReclaimsCancelledWrites) {
    auto uploads = partial_uploads{limits()};
    auto first = uploads.begin("a", 16);
    auto second = uploads.begin("b", 16);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    (*first)->retain();
    (*second)->retain();
    EXPECT_FALSE(uploads.begin("c", 16));
    auto resumed = uploads.begin("a", 16);
    ASSERT_TRUE(resumed);
    EXPECT_FALSE((*resumed)->append(1, {}));
    resumed->reset();
    EXPECT_TRUE(uploads.begin("c", 16));
}

TEST(ReapiUploads, ChargesCapacityAndFreesItAfterCancellation) {
    auto uploads = partial_uploads{limits()};
    auto first = uploads.begin("a", 16);
    auto second = uploads.begin("b", 16);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    ASSERT_TRUE((*first)->append(0, bytes(16)));
    ASSERT_TRUE((*second)->append(0, bytes(8)));
    EXPECT_FALSE((*second)->append(8, bytes(1)));
    first->reset();
    EXPECT_TRUE((*second)->append(8, bytes(1)));
}

TEST(ReapiUploads, ChargesCompletedBytesUntilStorageAndResponseEnd) {
    auto uploads = partial_uploads{limits()};
    auto first = uploads.begin("a", 16);
    ASSERT_TRUE(first);
    ASSERT_TRUE((*first)->append(0, bytes(16)));
    auto complete = (*first)->finish();
    ASSERT_TRUE(complete);
    EXPECT_EQ(*complete, bytes(16));
    auto second = uploads.begin("b", 16);
    ASSERT_TRUE(second);
    EXPECT_FALSE((*second)->append(0, bytes(9)));
    *complete = std::vector<std::byte>{};
    (*first)->retain();
    EXPECT_TRUE((*second)->append(0, bytes(9)));
    EXPECT_FALSE(uploads.committed("a"));
}

TEST(ReapiUploads, ExpiresIdleUploadsWithoutStatusPollingExtendingLifetime) {
    auto uploads = partial_uploads{limits()};
    const auto now = partial_uploads::clock::now();
    auto started = uploads.begin("a", 16, now);
    ASSERT_TRUE(started);
    ASSERT_TRUE((*started)->append(0, bytes(16), now));
    (*started)->retain(now);
    ASSERT_EQ(*uploads.committed("a", now + 9s), 16);
    uploads.expire(now + 10s);
    EXPECT_FALSE(uploads.committed("a", now + 10s));
    auto restarted = uploads.begin("a", 16, now + 10s);
    ASSERT_TRUE(restarted);
    EXPECT_FALSE((*restarted)->append(16, {}, now + 10s));
    EXPECT_TRUE((*restarted)->append(0, bytes(16), now + 10s));
}

TEST(ReapiUploads, ResumeRefreshesIdleTimeAndRejectsConcurrentWriters) {
    auto uploads = partial_uploads{limits()};
    const auto now = partial_uploads::clock::now();
    auto first = uploads.begin("a", 16, now);
    ASSERT_TRUE(first);
    ASSERT_TRUE((*first)->append(0, bytes(8), now));
    EXPECT_FALSE(uploads.begin("a", 16, now));
    // Active Writes hold their charge even if storage or the transport is slow.
    uploads.expire(now + 20s);
    EXPECT_EQ(*uploads.committed("a", now + 20s), 8);
    (*first)->retain(now + 20s);
    auto second = uploads.begin("a", 16, now + 21s);
    ASSERT_TRUE(second);
    first->reset();
    ASSERT_TRUE((*second)->append(8, bytes(8), now + 21s));
    (*second)->retain(now + 21s);
    EXPECT_EQ(*uploads.committed("a", now + 30s), 16);
    EXPECT_FALSE(uploads.committed("a", now + 31s));
}

TEST(ReapiUploads, RejectsOffsetsAndIncompleteFinishWithoutLeakingQuota) {
    auto uploads = partial_uploads{limits()};
    auto started = uploads.begin("a", 4);
    ASSERT_TRUE(started);
    EXPECT_FALSE((*started)->append(-1, {}));
    EXPECT_FALSE((*started)->append(std::numeric_limits<std::int64_t>::max(), bytes(1)));
    ASSERT_TRUE((*started)->append(0, bytes(2)));
    EXPECT_FALSE((*started)->finish());
    EXPECT_FALSE((*started)->append(2, bytes(3)));
    started->reset();
    EXPECT_FALSE(uploads.committed("a"));
}

#if defined(LIQUID_CLI) && !defined(_WIN32)
#include <arpa/inet.h>
#include <csignal>
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
class ReapiUploadServer : public testing::Test {
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
        const auto endpoint = "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
        listen = endpoint;
        pid = ::fork();
        ASSERT_GE(pid, 0);
        if (pid == 0) {
            if (::chdir(root.path().c_str()) != 0) {
                std::_Exit(126);
            }
            ::setenv("LIQUID_REAPI_MAX_UPLOAD_BYTES", "16", 1);
            ::setenv("LIQUID_REAPI_MAX_BUFFERED_BYTES", "24", 1);
            ::setenv("LIQUID_REAPI_MAX_UPLOADS", "2", 1);
            ::setenv("LIQUID_REAPI_MAX_RESOURCE_BYTES", "128", 1);
            ::setenv("LIQUID_REAPI_UPLOAD_IDLE_MS", "2000", 1);
            ::execl(
                LIQUID_CLI, LIQUID_CLI, "serve", "reapi", "--listen", endpoint.c_str(), nullptr);
            std::_Exit(127);
        }
        client = std::make_unique<liquid::reapi::grpc_client>(
            liquid::remote_config{.endpoint = endpoint, .deadline = 2s});
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        while (std::chrono::steady_clock::now() < deadline) {
            auto ready = query("probe");
            if (!ready && ready.error().code == liquid::errc::not_found) {
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

    static auto name(std::string_view id, std::int64_t size = 16) -> std::string {
        return "uploads/" + std::string{id} + "/blobs/" + std::string(64, 'a') + "/" +
               std::to_string(size);
    }

    auto send(std::string resource, std::int64_t offset, std::size_t count, bool finish = false) {
        auto encoded =
            liquid::reapi::encode(liquid::reapi::WriteRequest{.resource_name = std::move(resource),
                                                              .write_offset = offset,
                                                              .finish_write = finish,
                                                              .data = bytes(count)});
        return liquid::runtime::block_on(
            client->call("/google.bytestream.ByteStream/Write", *encoded));
    }

    liquid::runtime::task<unsigned>
    raw_call(std::vector<std::byte> body, bool no_response_credit, bool end_stream) {
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
        auto connection = liquid::http2::connection{
            transport,
            liquid::http2::endpoint_role::client,
            {.initial_window_size = no_response_credit ? 0 : liquid::http2::default_window_size}};
        auto started = co_await connection.start();
        if (!started) {
            co_return std::unexpected(started.error());
        }
        const auto headers = std::vector<liquid::http2::header_field>{
            {":method", "POST", false},
            {":scheme", "http", false},
            {":authority", listen, false},
            {":path", "/google.bytestream.ByteStream/Write", false},
            {"content-type", "application/grpc", false}};
        auto stream = co_await connection.open_stream(headers, false);
        if (!stream) {
            co_return std::unexpected(stream.error());
        }
        auto sent = co_await connection.send_data(*stream, std::move(body), end_stream);
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

    liquid::runtime::task<unsigned>
    bounded_raw_call(std::vector<std::byte> body, bool no_response_credit, bool end_stream) {
        auto [deadline_error, outcome] = co_await liquid::runtime::timeout(
            liquid::runtime::as_io(raw_call(std::move(body), no_response_credit, end_stream)), 5s);
        if (deadline_error) {
            co_return std::unexpected(
                liquid::error{liquid::errc::unavailable, "RPC timed out", {}});
        }
        co_return std::move(outcome);
    }

    auto send_many(std::vector<liquid::http2::grpc_message> messages,
                   bool no_response_credit = false,
                   bool end_stream = true) -> liquid::result<unsigned> {
        auto body = std::vector<std::byte>{};
        for (const auto& message : messages) {
            auto encoded = liquid::http2::encode_grpc_message(message);
            if (!encoded) {
                return std::unexpected(encoded.error());
            }
            body.insert(body.end(), encoded->begin(), encoded->end());
        }
        return liquid::runtime::block_on(
            bounded_raw_call(std::move(body), no_response_credit, end_stream));
    }

    auto query(std::string_view id) -> liquid::result<liquid::reapi::QueryWriteStatusResponse> {
        auto encoded = liquid::reapi::encode(
            liquid::reapi::QueryWriteStatusRequest{.resource_name = name(id)});
        auto response = liquid::runtime::block_on(
            client->call("/google.bytestream.ByteStream/QueryWriteStatus", *encoded));
        if (!response) {
            return std::unexpected(response.error());
        }
        if (response->size() != 1) {
            return std::unexpected(liquid::error{liquid::errc::corrupt, "missing response", {}});
        }
        return liquid::reapi::decode_message<liquid::reapi::QueryWriteStatusResponse>(
            response->front());
    }
};
} // namespace

TEST_F(ReapiUploadServer, LimitsPerUploadAndDeclaredSize) {
    auto large = send(name("a", 17), 0, 1);
    ASSERT_FALSE(large);
    EXPECT_NE(large.error().message.find("limit"), std::string::npos);
    EXPECT_FALSE(send(name("a", 1), 0, 2));
    EXPECT_FALSE(send("uploads/" + std::string(129, 'a') + "/blobs/a/1", 0, 1));
}

TEST_F(ReapiUploadServer, LimitsTotalBytesAndReleasesFailedResume) {
    ASSERT_TRUE(send(name("a"), 0, 16));
    ASSERT_TRUE(send(name("b"), 0, 8));
    auto excess = send(name("b"), 8, 1);
    ASSERT_FALSE(excess);
    EXPECT_NE(excess.error().message.find("limit"), std::string::npos);
    EXPECT_FALSE(query("b"));
    EXPECT_FALSE(send(name("a"), 15, 1));
    EXPECT_FALSE(query("a"));
    EXPECT_TRUE(send(name("c"), 0, 16));
}

TEST_F(ReapiUploadServer, LimitsEmptyUploadCount) {
    ASSERT_TRUE(send(name("a"), 0, 0));
    ASSERT_TRUE(send(name("b"), 0, 0));
    auto third = send(name("c"), 0, 0);
    ASSERT_FALSE(third);
    EXPECT_NE(third.error().message.find("limit"), std::string::npos);
}

TEST_F(ReapiUploadServer, PartialRpcSurvivesDisconnectThenExpires) {
    ASSERT_TRUE(send(name("a"), 0, 8));
    auto status = query("a");
    ASSERT_TRUE(status);
    EXPECT_EQ(status->committed_size, 8);
    EXPECT_FALSE(status->complete);
    ASSERT_TRUE(send(name("a"), 8, 8));
    std::this_thread::sleep_for(3200ms);
    // A fresh partial Write first exercises cleanup without querying the expired one.
    EXPECT_TRUE(send(name("b"), 0, 16));
    EXPECT_FALSE(query("a"));
    EXPECT_FALSE(send(name("a"), 16, 0));
    EXPECT_TRUE(send(name("a"), 0, 1));
}
TEST_F(ReapiUploadServer, MultiMessageErrorsDiscardEveryPreviouslyAppendedByte) {
    auto first = liquid::reapi::encode(
        liquid::reapi::WriteRequest{.resource_name = name("a"), .data = bytes(8)});
    ASSERT_TRUE(first);
    for (auto invalid : std::vector<liquid::http2::grpc_message>{
             {true, {}},
             {false, {std::byte{0xff}}},
             {false,
              *liquid::reapi::encode(liquid::reapi::WriteRequest{
                  .resource_name = name("b"), .write_offset = 8, .data = bytes(1)})},
             {false,
              *liquid::reapi::encode(
                  liquid::reapi::WriteRequest{.write_offset = 8, .finish_write = true})}}) {
        auto outcome = send_many({{false, *first}, std::move(invalid)});
        ASSERT_TRUE(outcome);
        EXPECT_NE(*outcome, 0);
        EXPECT_FALSE(query("a"));
    }
}

TEST_F(ReapiUploadServer, FinishingMustBeLastAndDigestFailureDiscardsTheUpload) {
    auto digest = liquid::reapi::make_digest(liquid::algorithm::sha256, bytes(8));
    ASSERT_TRUE(digest);
    auto first = liquid::reapi::encode(
        liquid::reapi::WriteRequest{.resource_name = "uploads/a/blobs/" + digest->hash + "/8",
                                    .finish_write = true,
                                    .data = bytes(8)});
    auto extra = liquid::reapi::encode(
        liquid::reapi::WriteRequest{.resource_name = name("b"), .data = bytes(8)});
    auto outcome = send_many({{false, *first}, {false, *extra}});
    ASSERT_TRUE(outcome);
    EXPECT_EQ(*outcome, 3);
    EXPECT_FALSE(query("b"));
    EXPECT_FALSE(send(name("a"), 0, 16, true));
    EXPECT_FALSE(query("a"));
    EXPECT_TRUE(send(name("b"), 0, 16));
}

TEST_F(ReapiUploadServer, ValidFinishStoresBlobAndReleasesItsQuota) {
    const auto payload = bytes(16);
    auto digest = liquid::reapi::make_digest(liquid::algorithm::sha256, payload);
    ASSERT_TRUE(digest);
    const auto resource = "uploads/complete/blobs/" + digest->hash + "/16";
    ASSERT_TRUE(send(resource, 0, 8));
    auto continued = liquid::reapi::encode(liquid::reapi::WriteRequest{
        .resource_name = resource, .write_offset = 8, .data = bytes(4)});
    auto finished = liquid::reapi::encode(
        liquid::reapi::WriteRequest{.write_offset = 12, .finish_write = true, .data = bytes(4)});
    auto completed = send_many({{false, *continued}, {false, *finished}});
    ASSERT_TRUE(completed);
    ASSERT_EQ(*completed, 0);
    auto encoded =
        liquid::reapi::encode(liquid::reapi::QueryWriteStatusRequest{.resource_name = resource});
    auto response = liquid::runtime::block_on(
        client->call("/google.bytestream.ByteStream/QueryWriteStatus", *encoded));
    ASSERT_TRUE(response);
    ASSERT_EQ(response->size(), 1);
    auto status =
        liquid::reapi::decode_message<liquid::reapi::QueryWriteStatusResponse>(response->front());
    ASSERT_TRUE(status);
    EXPECT_TRUE(status->complete);
    EXPECT_EQ(status->committed_size, 16);
    EXPECT_TRUE(send(name("next"), 0, 16));
}

TEST_F(ReapiUploadServer, StalledResponseExpiresActiveWriteAndReleasesItsQuota) {
    auto message = liquid::reapi::encode(
        liquid::reapi::WriteRequest{.resource_name = name("stalled"), .data = bytes(16)});
    ASSERT_TRUE(message);
    auto outcome = send_many({{false, *message}}, true);
    ASSERT_FALSE(outcome);
    // The server must close at its own handling deadline, before our client's fallback deadline.
    EXPECT_NE(outcome.error().message, "RPC timed out");
    EXPECT_FALSE(query("stalled"));
    EXPECT_TRUE(send(name("next"), 0, 16));
}

TEST_F(ReapiUploadServer, StalledStreamExpiresActiveWriteAndReleasesItsQuota) {
    // Write messages are read one at a time, so a client that sends part of a Write and then
    // stops holds an active upload; the upload timeout bounds the wait for its next message.
    auto message = liquid::reapi::encode(
        liquid::reapi::WriteRequest{.resource_name = name("open"), .data = bytes(16)});
    ASSERT_TRUE(message);
    auto outcome = send_many({{false, *message}}, false, false);
    ASSERT_FALSE(outcome);
    EXPECT_NE(outcome.error().message, "RPC timed out");
    EXPECT_FALSE(query("open"));
    EXPECT_TRUE(send(name("next"), 0, 16));
}

TEST_F(ReapiUploadServer, EmptyWriteIsRefusedWithoutAnUpload) {
    auto outcome = send_many({});
    ASSERT_TRUE(outcome);
    EXPECT_EQ(*outcome, 3);
    EXPECT_TRUE(send(name("a"), 0, 16));
    EXPECT_TRUE(send(name("b"), 0, 8));
}

#endif
