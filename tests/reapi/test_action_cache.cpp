// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "../temporary_directory.hpp"
#include "reapi/action_cache.hpp"
#include "reapi/messages.hpp"

#if defined(LIQUID_CLI) && !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>

#include "liquid/runtime.hpp"
#include "reapi/client.hpp"
#endif

// liquid #428 (LSR-034): the REAPI ActionCache must not let an action digest name a file outside
// its cache directory.
namespace {

auto bytes(std::string_view value) -> std::vector<std::byte> {
    const auto raw = std::as_bytes(std::span{value});
    return {raw.begin(), raw.end()};
}

auto read_text(const std::filesystem::path& path) -> std::string {
    auto input = std::ifstream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

auto write_text(const std::filesystem::path& path, std::string_view value) -> void {
    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
    output << value;
}

const auto valid_hash = std::string(64, 'a');

auto digest(std::string hash) -> liquid::reapi::Digest {
    return {.hash = std::move(hash), .size_bytes = 3};
}

TEST(ReapiActionCache, accepts_only_lowercase_hex_of_the_algorithm_length) {
    for (const auto hash_algorithm : {liquid::algorithm::blake3, liquid::algorithm::sha256}) {
        auto name = liquid::reapi::action_cache_name(digest(valid_hash), hash_algorithm);
        ASSERT_TRUE(name) << name.error().message;
        EXPECT_EQ(*name, valid_hash);
    }
    const auto refused = std::vector<std::string>{
        "",
        "../../etc/passwd",
        "/etc/passwd",
        "..",
        std::string(62, 'a') + "/x",
        std::string(61, 'a') + "/..",
        std::string(63, 'a'),
        std::string(65, 'a'),
        std::string(128, 'a'),
        std::string(64, 'A'),
        std::string(63, 'a') + "g",
        std::string(63, 'a') + std::string(1, '\0'),
        std::string(63, 'a') + "\\",
    };
    for (const auto& hash : refused) {
        auto name = liquid::reapi::action_cache_name(digest(hash), liquid::algorithm::sha256);
        ASSERT_FALSE(name) << "accepted " << hash;
        EXPECT_EQ(name.error().code, liquid::errc::invalid_argument);
    }
    auto negative = liquid::reapi::action_cache_name(
        liquid::reapi::Digest{.hash = valid_hash, .size_bytes = -1}, liquid::algorithm::sha256);
    EXPECT_FALSE(negative);
    auto keyed =
        liquid::reapi::action_cache_name(digest(valid_hash), liquid::algorithm::blake3_keyed);
    EXPECT_FALSE(keyed);
}

TEST(ReapiActionCache, entries_round_trip_inside_the_cache_root) {
    const auto root = liquid::test::fixture_path("reapi-action-cache-round-trip");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "cache");
    ASSERT_TRUE(liquid::reapi::write_action_cache(root / "cache", valid_hash, bytes("first")));
    ASSERT_TRUE(liquid::reapi::write_action_cache(root / "cache", valid_hash, bytes("second")));
    auto read = liquid::reapi::read_action_cache(root / "cache", valid_hash);
    ASSERT_TRUE(read) << read.error().message;
    EXPECT_EQ(*read, bytes("second"));
    EXPECT_EQ(read_text(root / "cache" / valid_hash), "second");
    auto entries = std::vector<std::filesystem::path>{};
    for (const auto& entry : std::filesystem::directory_iterator{root / "cache"}) {
        entries.push_back(entry.path().filename());
    }
    EXPECT_EQ(entries, std::vector<std::filesystem::path>{valid_hash});
    auto missing = liquid::reapi::read_action_cache(root / "cache", std::string(64, 'b'));
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, liquid::errc::not_found);
}

TEST(ReapiActionCache, file_operations_refuse_a_name_that_is_not_hex) {
    const auto root = liquid::test::fixture_path("reapi-action-cache-names");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "cache");
    for (const auto* name : {"../escaped", "/tmp/escaped", "", ".", "a/b"}) {
        auto written = liquid::reapi::write_action_cache(root / "cache", name, bytes("x"));
        ASSERT_FALSE(written) << name;
        EXPECT_EQ(written.error().code, liquid::errc::invalid_argument);
        EXPECT_FALSE(liquid::reapi::read_action_cache(root / "cache", name));
    }
    EXPECT_FALSE(std::filesystem::exists(root / "escaped"));
}

#if !defined(_WIN32)
TEST(ReapiActionCache, a_symbolic_link_is_never_followed) {
    const auto root = liquid::test::fixture_path("reapi-action-cache-links");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "cache");
    write_text(root / "outside", "victim");
    std::filesystem::create_symlink(root / "outside", root / "cache" / valid_hash);

    auto read = liquid::reapi::read_action_cache(root / "cache", valid_hash);
    ASSERT_FALSE(read);
    EXPECT_NE(read.error().code, liquid::errc::not_found);

    // Writing replaces the link itself; the file it pointed at is untouched.
    ASSERT_TRUE(liquid::reapi::write_action_cache(root / "cache", valid_hash, bytes("result")));
    EXPECT_EQ(read_text(root / "outside"), "victim");
    EXPECT_FALSE(std::filesystem::is_symlink(root / "cache" / valid_hash));
    EXPECT_EQ(read_text(root / "cache" / valid_hash), "result");

    // A cache root that is itself a link is refused.
    std::filesystem::create_directories(root / "elsewhere");
    std::filesystem::create_directory_symlink(root / "elsewhere", root / "linked-cache");
    EXPECT_FALSE(liquid::reapi::write_action_cache(root / "linked-cache", valid_hash, bytes("x")));
    EXPECT_FALSE(std::filesystem::exists(root / "elsewhere" / valid_hash));
}
#endif

#if defined(LIQUID_CLI) && !defined(_WIN32)

constexpr auto update_method = "/build.bazel.remote.execution.v2.ActionCache/UpdateActionResult";
constexpr auto get_method = "/build.bazel.remote.execution.v2.ActionCache/GetActionResult";
constexpr auto capabilities_method =
    "/build.bazel.remote.execution.v2.Capabilities/GetCapabilities";

auto free_port() -> std::uint16_t {
    const auto descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(descriptor, 0);
    auto address = sockaddr_in{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    EXPECT_EQ(::bind(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
    auto size = static_cast<socklen_t>(sizeof(address));
    EXPECT_EQ(::getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &size), 0);
    EXPECT_EQ(::close(descriptor), 0);
    return ntohs(address.sin_port);
}

class reapi_server {
public:
    reapi_server(const std::filesystem::path& directory, std::uint16_t port) {
        pid_ = ::fork();
        if (pid_ == 0) {
            if (::chdir(directory.c_str()) != 0) {
                std::_Exit(126);
            }
            const auto listen = "127.0.0.1:" + std::to_string(port);
            ::execl(LIQUID_CLI,
                    LIQUID_CLI,
                    "serve",
                    "reapi",
                    "--listen",
                    listen.c_str(),
                    static_cast<char*>(nullptr));
            std::_Exit(127);
        }
        EXPECT_GT(pid_, 0);
    }
    ~reapi_server() {
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            auto status = 0;
            ::waitpid(pid_, &status, 0);
        }
    }
    reapi_server(const reapi_server&) = delete;
    auto operator=(const reapi_server&) -> reapi_server& = delete;

private:
    pid_t pid_{};
};

template <typename Message>
auto call(liquid::reapi::grpc_client& client, std::string_view method, const Message& request)
    -> liquid::result<std::vector<std::vector<std::byte>>> {
    auto encoded = liquid::reapi::encode(request);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return liquid::runtime::block_on(client.call(method, *encoded));
}

auto update(liquid::reapi::grpc_client& client, std::string hash)
    -> liquid::result<std::vector<std::vector<std::byte>>> {
    return call(client,
                update_method,
                liquid::reapi::UpdateActionResultRequest{
                    .action_digest = digest(std::move(hash)),
                    .action_result = liquid::reapi::ActionResult{.exit_code = 7}});
}

auto get(liquid::reapi::grpc_client& client, std::string hash)
    -> liquid::result<std::vector<std::vector<std::byte>>> {
    return call(client,
                get_method,
                liquid::reapi::GetActionResultRequest{.action_digest = digest(std::move(hash))});
}

TEST(ReapiActionCache, the_server_refuses_a_malformed_action_digest_before_touching_a_file) {
    const auto root = liquid::test::fixture_path("reapi-action-cache-server");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "repository" / ".liquid-store");
    write_text(root / "repository" / "victim", "do not overwrite");
    const auto port = free_port();
    auto server = reapi_server{root / "repository", port};
    auto client = liquid::reapi::grpc_client{liquid::remote_config{
        .endpoint = "127.0.0.1:" + std::to_string(port), .deadline = std::chrono::seconds{5}}};
    auto ready = false;
    for (auto attempt = 0; attempt < 200 && !ready; ++attempt) {
        ready = static_cast<bool>(
            call(client, capabilities_method, liquid::reapi::GetCapabilitiesRequest{}));
        if (!ready) {
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
    }
    ASSERT_TRUE(ready) << "REAPI listener did not start";
    const auto cache = root / "repository" / ".liquid-store" / "reapi-action-cache";

    // ../../escaped and ../../victim resolve from the cache directory to the repository root.
    const auto absolute = (root / "absolute-escaped").string();
    for (const auto& hash : {std::string{"../../escaped"}, std::string{"../../victim"}, absolute}) {
        auto written = update(client, hash);
        ASSERT_FALSE(written) << "update accepted " << hash;
        EXPECT_NE(written.error().message.find("action digest"), std::string::npos)
            << written.error().message;
        auto read = get(client, hash);
        ASSERT_FALSE(read) << "get accepted " << hash;
        EXPECT_NE(read.error().message.find("action digest"), std::string::npos)
            << read.error().message;
    }
    EXPECT_FALSE(std::filesystem::exists(root / "repository" / "escaped"));
    EXPECT_FALSE(std::filesystem::exists(absolute));
    EXPECT_EQ(read_text(root / "repository" / "victim"), "do not overwrite");
    EXPECT_FALSE(update(client, std::string(64, 'A')));

    // A valid digest is still cached, inside the cache directory only.
    auto written = update(client, valid_hash);
    ASSERT_TRUE(written) << written.error().message;
    auto read = get(client, valid_hash);
    ASSERT_TRUE(read) << read.error().message;
    ASSERT_EQ(read->size(), 1U);
    auto result = liquid::reapi::decode_message<liquid::reapi::ActionResult>(read->front());
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->exit_code, 7);
    EXPECT_TRUE(std::filesystem::is_regular_file(cache / valid_hash));
}

#endif

} // namespace
