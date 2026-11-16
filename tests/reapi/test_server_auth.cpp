// `liquid serve reapi` with a bearer token refuses every RPC that does not carry it, writes
// included, before the request body is read (liquid#415).
#include <gtest/gtest.h>

#if defined(LIQUID_CLI) && !defined(LIQUID_SHARED_TEST_BUILD)

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "../temporary_directory.hpp"
#include "liquid/remote.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "reapi/client.hpp"
#include "reapi/messages.hpp"

namespace {

constexpr auto capabilities_method =
    "/build.bazel.remote.execution.v2.Capabilities/GetCapabilities";
constexpr auto update_action_result_method =
    "/build.bazel.remote.execution.v2.ActionCache/UpdateActionResult";
constexpr auto batch_update_method =
    "/build.bazel.remote.execution.v2.ContentAddressableStorage/BatchUpdateBlobs";

const auto token = std::string{"reapi-test-token-0123456789abcdefghijklmnop"};

auto free_port() -> std::uint16_t {
    const auto descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) {
        return 0;
    }
    auto address = sockaddr_in{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    auto size = static_cast<socklen_t>(sizeof(address));
    if (::bind(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        ::getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
        ::close(descriptor);
        return 0;
    }
    ::close(descriptor);
    return ntohs(address.sin_port);
}

class reapi_server_process {
public:
    reapi_server_process(const std::filesystem::path& repository,
                         const std::string& listen,
                         const std::filesystem::path& token_file) {
        pid_ = ::fork();
        if (pid_ == 0) {
            if (::chdir(repository.c_str()) != 0) {
                std::_Exit(126);
            }
            ::execl(LIQUID_CLI,
                    LIQUID_CLI,
                    "serve",
                    "reapi",
                    "--listen",
                    listen.c_str(),
                    "--token-file",
                    token_file.c_str(),
                    static_cast<char*>(nullptr));
            std::_Exit(127);
        }
    }

    ~reapi_server_process() {
        if (pid_ > 0) {
            static_cast<void>(::kill(pid_, SIGTERM));
            static_cast<void>(::waitpid(pid_, nullptr, 0));
        }
    }

    reapi_server_process(const reapi_server_process&) = delete;
    auto operator=(const reapi_server_process&) -> reapi_server_process& = delete;

private:
    pid_t pid_{-1};
};

auto client(const std::string& endpoint, std::map<std::string, std::string> headers)
    -> liquid::reapi::grpc_client {
    return liquid::reapi::grpc_client{liquid::remote_config{
        .endpoint = endpoint, .headers = std::move(headers), .deadline = std::chrono::seconds{5}}};
}

auto bearer(const std::string& value) -> std::map<std::string, std::string> {
    return {{"authorization", "Bearer " + value}};
}

auto call(liquid::reapi::grpc_client& peer,
          std::string_view method,
          const std::vector<std::byte>& request) -> liquid::result<void> {
    auto response = liquid::runtime::block_on(peer.call(method, request));
    if (!response) {
        return std::unexpected(response.error());
    }
    return {};
}

auto wait_until_serving(const std::string& endpoint) -> bool {
    auto request = liquid::reapi::encode(liquid::reapi::GetCapabilitiesRequest{});
    if (!request) {
        return false;
    }
    auto peer = client(endpoint, bearer(token));
    for (auto attempt = 0; attempt < 200; ++attempt) {
        if (call(peer, capabilities_method, *request)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return false;
}

} // namespace

TEST(ReapiServerAuthentication, BearerTokenIsRequiredForEveryRpcAndRefusedWritesStoreNothing) {
    const auto root = liquid::test::fixture_path("liquid-reapi-auth-" + std::to_string(::getpid()));
    auto code = std::error_code{};
    std::filesystem::remove_all(root, code);
    auto created = liquid::runtime::block_on(liquid::repository::create(root / "repository"));
    ASSERT_TRUE(created) << created.error().message;
    *created = liquid::repository{};
    const auto token_file = root / "token";
    {
        auto output = std::ofstream{token_file, std::ios::binary};
        output << token << "\n";
    }
    std::filesystem::permissions(token_file,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace);
    const auto port = free_port();
    ASSERT_NE(port, 0);
    const auto endpoint = "127.0.0.1:" + std::to_string(port);
    auto server = reapi_server_process{root / "repository", endpoint, token_file};
    ASSERT_TRUE(wait_until_serving(endpoint));

    const auto action_hash = std::string(64, 'a');
    auto update = liquid::reapi::UpdateActionResultRequest{};
    update.action_digest = liquid::reapi::Digest{.hash = action_hash, .size_bytes = 1};
    update.action_result = liquid::reapi::ActionResult{};
    auto update_bytes = liquid::reapi::encode(update);
    ASSERT_TRUE(update_bytes) << update_bytes.error().message;
    const auto cache_entry =
        root / "repository" / ".liquid-store" / "reapi-action-cache" / action_hash;

    auto anonymous = client(endpoint, {});
    auto refused = call(anonymous, update_action_result_method, *update_bytes);
    EXPECT_FALSE(refused);
    EXPECT_FALSE(std::filesystem::exists(cache_entry));

    auto wrong = client(endpoint, bearer(std::string(token.size(), 'x')));
    refused = call(wrong, update_action_result_method, *update_bytes);
    EXPECT_FALSE(refused);
    EXPECT_FALSE(std::filesystem::exists(cache_entry));

    auto blobs = liquid::reapi::BatchUpdateBlobsRequest{};
    blobs.requests.push_back(liquid::reapi::BatchUpdateBlobsRequestEntry{
        .digest = liquid::reapi::Digest{.hash = std::string(64, 'b'), .size_bytes = 1},
        .data = {std::byte{'x'}}});
    auto blob_bytes = liquid::reapi::encode(blobs);
    ASSERT_TRUE(blob_bytes) << blob_bytes.error().message;
    EXPECT_FALSE(call(anonymous, batch_update_method, *blob_bytes));

    auto capabilities = liquid::reapi::encode(liquid::reapi::GetCapabilitiesRequest{});
    ASSERT_TRUE(capabilities);
    EXPECT_FALSE(call(anonymous, capabilities_method, *capabilities));

    auto authorized = client(endpoint, bearer(token));
    auto accepted = call(authorized, update_action_result_method, *update_bytes);
    EXPECT_TRUE(accepted) << accepted.error().message;
    EXPECT_TRUE(std::filesystem::exists(cache_entry));
    std::filesystem::remove_all(root, code);
}

#endif
