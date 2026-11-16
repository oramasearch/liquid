// SPDX-License-Identifier: MIT

// `liquid serve reapi` and `liquid scheduler` with a bearer token refuse every RPC that does not
// carry it, writes included, before the request body is read (liquid#415, liquid#520).
#include <gtest/gtest.h>

#if defined(LIQUID_CLI) && !defined(LIQUID_SHARED_TEST_BUILD)

#include <arpa/inet.h>
#include <fcntl.h>
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
    // Runs `liquid <arguments>` in `directory`.
    reapi_server_process(const std::filesystem::path& directory,
                         std::vector<std::string> arguments) {
        arguments.insert(arguments.begin(), LIQUID_CLI);
        auto argv = std::vector<char*>{};
        for (auto& argument : arguments) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);
        pid_ = ::fork();
        if (pid_ == 0) {
            if (::chdir(directory.c_str()) != 0) {
                std::_Exit(126);
            }
            ::execv(LIQUID_CLI, argv.data());
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

// Writes this machine's identity document, as `liquid identity show --document` prints it, to
// `path`: the scheduler trusts only the workers whose documents it is given (liquid#416).
auto write_identity_document(const std::filesystem::path& path) -> bool {
    const auto child = ::fork();
    if (child == 0) {
        const auto descriptor =
            ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600);
        if (descriptor < 0 || ::dup2(descriptor, STDOUT_FILENO) < 0) {
            std::_Exit(126);
        }
        ::execl(
            LIQUID_CLI, LIQUID_CLI, "identity", "show", "--document", static_cast<char*>(nullptr));
        std::_Exit(127);
    }
    auto status = 0;
    return child > 0 && ::waitpid(child, &status, 0) == child && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
}

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

// Writes `token` to an owner-only file.
auto write_token(const std::filesystem::path& file) -> void {
    {
        auto output = std::ofstream{file, std::ios::binary};
        output << token << "\n";
    }
    std::filesystem::permissions(file,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace);
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
    write_token(token_file);
    const auto port = free_port();
    ASSERT_NE(port, 0);
    const auto endpoint = "127.0.0.1:" + std::to_string(port);
    auto server = reapi_server_process{
        root / "repository",
        {"serve", "reapi", "--listen", endpoint, "--token-file", token_file.string()}};
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

TEST(ReapiServerAuthentication, SchedulerRequiresTheBearerTokenOnEveryRpc) {
    const auto root =
        liquid::test::fixture_path("liquid-scheduler-auth-" + std::to_string(::getpid()));
    auto code = std::error_code{};
    std::filesystem::remove_all(root, code);
    std::filesystem::create_directories(root / "store");
    const auto token_file = root / "token";
    write_token(token_file);
    const auto port = free_port();
    const auto control_port = free_port();
    ASSERT_NE(port, 0);
    ASSERT_NE(control_port, 0);
    const auto endpoint = "127.0.0.1:" + std::to_string(port);
    const auto worker_document = root / "worker.document";
    ASSERT_TRUE(write_identity_document(worker_document));
    auto server = reapi_server_process{root,
                                       {"scheduler",
                                        "--trust-worker",
                                        worker_document.string(),
                                        "--listen",
                                        endpoint,
                                        "--control-listen",
                                        "127.0.0.1:" + std::to_string(control_port),
                                        "--store",
                                        (root / "store").string(),
                                        "--token-file",
                                        token_file.string()}};
    ASSERT_TRUE(wait_until_serving(endpoint));

    const auto action_hash = std::string(64, 'c');
    auto update = liquid::reapi::UpdateActionResultRequest{};
    update.action_digest = liquid::reapi::Digest{.hash = action_hash, .size_bytes = 1};
    update.action_result = liquid::reapi::ActionResult{};
    auto update_bytes = liquid::reapi::encode(update);
    ASSERT_TRUE(update_bytes) << update_bytes.error().message;
    const auto cache_entry = root / "store" / "reapi-action-cache" / action_hash;

    auto anonymous = client(endpoint, {});
    EXPECT_FALSE(call(anonymous, update_action_result_method, *update_bytes));
    EXPECT_FALSE(std::filesystem::exists(cache_entry));
    auto wrong = client(endpoint, bearer(std::string(token.size(), 'x')));
    EXPECT_FALSE(call(wrong, update_action_result_method, *update_bytes));
    EXPECT_FALSE(std::filesystem::exists(cache_entry));
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
