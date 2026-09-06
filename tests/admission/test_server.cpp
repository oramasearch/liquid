#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "../temporary_directory.hpp"
#include "http1/client.hpp"
#include "liquid/admission.hpp"
#include "liquid/attestation.hpp"
#include "liquid/runtime.hpp"
#include "liquid/store.hpp"

namespace {

constexpr auto image = "registry.example/liquid@sha256:"
                       "1111111111111111111111111111111111111111111111111111111111111111";

auto shell_quote(const std::filesystem::path& value) -> std::string {
    return "'" + value.string() + "'";
}

auto read_file(const std::filesystem::path& path) -> std::string {
    auto input = std::ifstream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

auto write_file(const std::filesystem::path& path, std::string_view value) -> void {
    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
    output << value;
    ASSERT_TRUE(output);
}

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

class child_process {
public:
    explicit child_process(std::vector<std::string> arguments) {
        pid_ = ::fork();
        if (pid_ == 0) {
            auto pointers = std::vector<char*>{};
            pointers.reserve(arguments.size() + 1);
            for (auto& argument : arguments) {
                pointers.push_back(argument.data());
            }
            pointers.push_back(nullptr);
            ::execv(pointers.front(), pointers.data());
            std::_Exit(127);
        }
        EXPECT_GT(pid_, 0);
    }

    ~child_process() {
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            auto status = 0;
            ::waitpid(pid_, &status, 0);
        }
    }

    child_process(const child_process&) = delete;
    auto operator=(const child_process&) -> child_process& = delete;

private:
    pid_t pid_{};
};

auto bytes(std::string_view value) -> std::vector<std::byte> {
    const auto* first = reinterpret_cast<const std::byte*>(value.data());
    return {first, first + value.size()};
}

auto text(std::span<const std::byte> value) -> std::string {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

auto request(std::string url,
             std::string method = "GET",
             std::string suffix = {},
             std::string body = {},
             bool verify_peer = true) -> liquid::http1::response {
    auto response = liquid::runtime::block_on(
        liquid::http1::request({.url = std::move(url),
                                .method = std::move(method),
                                .suffix = std::move(suffix),
                                .headers = {{"Content-Type", "application/json"}},
                                .body = bytes(body),
                                .verify_peer = verify_peer}));
    EXPECT_TRUE(response) << response.error().message << " " << response.error().context;
    if (!response) {
        return {};
    }
    return *response;
}

auto wait_until_ready(const std::string& control_url) -> void {
    for (auto attempt = 0; attempt < 100; ++attempt) {
        auto response = liquid::runtime::block_on(
            liquid::http1::request({.url = control_url, .suffix = "/readyz"}));
        if (response && response->status == 200) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    FAIL() << "admission listener did not become ready";
}

auto wait_until_healthy(const std::string& control_url) -> void {
    for (auto attempt = 0; attempt < 100; ++attempt) {
        auto response = liquid::runtime::block_on(
            liquid::http1::request({.url = control_url, .suffix = "/healthz"}));
        if (response && response->status == 200) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    FAIL() << "admission listener did not become healthy";
}

auto policy_for(std::string issuer,
                const std::filesystem::path& store,
                liquid::admission_mode mode = liquid::admission_mode::attestation)
    -> liquid::admission_policy {
    return {.mode = mode,
            .issuers = {std::move(issuer)},
            .profiles = {"image_provenance/1"},
            .exempt = {"registry.example/exempt/*"},
            .exempt_namespaces = {"kube-system"},
            .attestation_source = store.string(),
            .log = {.lake = "http://127.0.0.1:1", .max_checkpoint_age_seconds = 60}};
}

auto write_policy(const std::filesystem::path& path, const liquid::admission_policy& policy)
    -> void {
    auto encoded = liquid::encode_admission_policy(policy);
    ASSERT_TRUE(encoded) << encoded.error().message;
    write_file(path, *encoded);
}

auto mint_with_cli(const std::filesystem::path& root) -> liquid::attestation {
    std::filesystem::create_directories(root / ".liquid-store");
    const auto output = root / "attest.out";
    const auto command =
        "cd " + shell_quote(root) + " && LIQUID_HOME=" + shell_quote(root / "identity") + " " +
        shell_quote(LIQUID_CLI) + " attest image " + image +
        " --tree blake3:2222222222222222222222222222222222222222222222222222222222222222" +
        " --result blake3:3333333333333333333333333333333333333333333333333333333333333333" +
        " > " + shell_quote(output);
    EXPECT_EQ(std::system(command.c_str()), 0);
    auto emitted = read_file(output);
    EXPECT_TRUE(emitted.starts_with("attestation="));
    if (!emitted.starts_with("attestation=")) {
        return {};
    }
    while (!emitted.empty() && (emitted.back() == '\n' || emitted.back() == '\r')) {
        emitted.pop_back();
    }
    auto id = liquid::parse_digest(std::string_view{emitted}.substr(12));
    EXPECT_TRUE(id) << id.error().message;
    if (!id) {
        return {};
    }
    const auto verify = "cd " + shell_quote(root) +
                        " && LIQUID_HOME=" + shell_quote(root / "identity") + " " +
                        shell_quote(LIQUID_CLI) + " attest verify " + liquid::to_string(*id);
    EXPECT_EQ(std::system(verify.c_str()), 0);
    auto opened = liquid::runtime::block_on(liquid::store::open(root / ".liquid-store"));
    EXPECT_TRUE(opened) << opened.error().message;
    if (!opened) {
        return {};
    }
    auto stored = liquid::runtime::block_on(opened->get(*id));
    EXPECT_TRUE(stored) << stored.error().message;
    if (!stored) {
        return {};
    }
    auto statement = liquid::decode_attestation(*stored);
    EXPECT_TRUE(statement) << statement.error().message;
    if (!statement) {
        return {};
    }
    return *statement;
}

auto server_arguments(const std::filesystem::path& root,
                      std::uint16_t port,
                      std::uint16_t control_port,
                      bool tls) -> std::vector<std::string> {
    auto output = std::vector<std::string>{LIQUID_CLI,
                                           "serve",
                                           "admission",
                                           "--policy",
                                           (root / "policy.json").string(),
                                           "--store",
                                           (root / ".liquid-store").string(),
                                           "--listen",
                                           "127.0.0.1:" + std::to_string(port),
                                           "--control-listen",
                                           "127.0.0.1:" + std::to_string(control_port)};
    if (tls) {
        output.insert(output.end(), {"--tls-cert", LIQUID_ADMISSION_TLS_CERT});
        output.insert(output.end(), {"--tls-key", LIQUID_ADMISSION_TLS_KEY});
    } else {
        output.push_back("--insecure-http");
    }
    return output;
}

TEST(AdmissionServer, serves_tls_control_and_cli_contracts) {
    const auto root = liquid::test::fixture_path("admission-server");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto statement = mint_with_cli(root);
    auto policy = policy_for(statement.issuer.taap_id, root / ".liquid-store");
    write_policy(root / "policy.json", policy);
    const auto port = free_port();
    const auto control_port = free_port();
    auto server = child_process{server_arguments(root, port, control_port, true)};
    const auto control = "http://127.0.0.1:" + std::to_string(control_port);
    const auto webhook = "https://127.0.0.1:" + std::to_string(port);
    wait_until_ready(control);

    const auto review = read_file(std::filesystem::path{LIQUID_ADMISSION_FIXTURES} / "review.json");
    auto accepted = request(webhook, "POST", "/admission/review", review, false);
    EXPECT_EQ(accepted.status, 200U);
    EXPECT_NE(text(accepted.body).find(R"("uid": "fixture-review")"), std::string::npos)
        << text(accepted.body);
    EXPECT_NE(text(accepted.body).find(R"("allowed": true)"), std::string::npos);

    policy.issuers = {"taap:1:ed25519:not-accepted"};
    write_policy(root / "policy.json", policy);
    EXPECT_EQ(request(control, "POST", "/v1/policy/reload", "{}").status, 200U);
    auto wrong_issuer = request(webhook, "POST", "/admission/review", review, false);
    EXPECT_NE(text(wrong_issuer.body).find("issuer not accepted"), std::string::npos);

    policy.issuers = {statement.issuer.taap_id};
    policy.profiles = {"slsa_provenance/1"};
    write_policy(root / "policy.json", policy);
    EXPECT_EQ(request(control, "POST", "/v1/policy/reload", "{}").status, 200U);
    auto wrong_profile = request(webhook, "POST", "/admission/review", review, false);
    EXPECT_NE(text(wrong_profile.body).find("profile not accepted"), std::string::npos);

    policy.profiles = {"image_provenance/1"};
    write_policy(root / "policy.json", policy);
    EXPECT_EQ(request(control, "POST", "/v1/policy/reload", "{}").status, 200U);
    auto reloaded_issuer = request(webhook, "POST", "/admission/review", review, false);
    EXPECT_NE(text(reloaded_issuer.body).find(R"("allowed": true)"), std::string::npos);

    auto missing_review = review;
    const auto digest = std::string(64, '1');
    missing_review.replace(missing_review.find(digest), digest.size(), std::string(64, '4'));
    auto absent = request(webhook, "POST", "/admission/review", missing_review, false);
    EXPECT_NE(text(absent.body).find("no attestation"), std::string::npos);

    const auto tagged = read_file(std::filesystem::path{LIQUID_ADMISSION_FIXTURES} / "tagged.json");
    auto unpinned = request(webhook, "POST", "/admission/review", tagged, false);
    EXPECT_NE(text(unpinned.body).find("image not pinned by digest"), std::string::npos);

    policy.exempt = {"registry.example/*"};
    write_policy(root / "policy.json", policy);
    EXPECT_EQ(request(control, "POST", "/v1/policy/reload", "{}").status, 200U);
    auto glob_allowed = request(webhook, "POST", "/admission/review", tagged, false);
    EXPECT_NE(text(glob_allowed.body).find(R"("allowed": true)"), std::string::npos);
    policy.exempt = {"registry.example/exempt/*"};
    write_policy(root / "policy.json", policy);
    EXPECT_EQ(request(control, "POST", "/v1/policy/reload", "{}").status, 200U);

    const auto namespace_review =
        read_file(std::filesystem::path{LIQUID_ADMISSION_FIXTURES} / "exempt-namespace.json");
    auto namespace_allowed = request(webhook, "POST", "/admission/review", namespace_review, false);
    EXPECT_NE(text(namespace_allowed.body).find(R"("allowed": true)"), std::string::npos);

    auto verify =
        request(control, "GET", "/v1/verify?image=registry.example%2Fliquid%40sha256%3A" + digest);
    EXPECT_NE(text(verify.body).find(R"("allowed": true)"), std::string::npos);
    EXPECT_NE(text(verify.body).find("image_provenance/1"), std::string::npos);
    EXPECT_NE(text(request(control, "GET", "/v1").body).find("admission"), std::string::npos);
    EXPECT_NE(text(request(control, "GET", "/v1/policy").body).find("image_provenance/1"),
              std::string::npos);
    EXPECT_NE(text(request(control, "GET", "/v1/openapi.json").body).find("/admission/review"),
              std::string::npos);

    policy.mode = liquid::admission_mode::attestation_and_log;
    write_policy(root / "policy.json", policy);
    EXPECT_EQ(request(control, "POST", "/v1/policy/reload", "{}").status, 200U);
    auto unavailable_log = request(webhook, "POST", "/admission/review", review, false);
    EXPECT_NE(text(unavailable_log.body).find("log verifier unavailable"), std::string::npos);

    policy.mode = liquid::admission_mode::off;
    write_policy(root / "policy.json", policy);
    EXPECT_EQ(request(control, "POST", "/v1/policy/reload", "{}").status, 200U);
    auto off = request(webhook, "POST", "/admission/review", tagged, false);
    EXPECT_NE(text(off.body).find(R"("allowed": true)"), std::string::npos);

    auto metrics = request(control, "GET", "/metrics");
    EXPECT_NE(text(metrics.body).find("liquid_admission_reviews_total"), std::string::npos);
    EXPECT_NE(text(metrics.body).find("liquid_admission_images_total"), std::string::npos);
    EXPECT_NE(text(metrics.body).find("liquid_admission_review_seconds"), std::string::npos);
    EXPECT_NE(text(metrics.body).find("reason=\"exempt\""), std::string::npos);
    auto tools = request(
        control, "POST", "/mcp", R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}})");
    EXPECT_NE(text(tools.body).find("admission_policy"), std::string::npos);
    EXPECT_NE(text(tools.body).find("admission_verify"), std::string::npos);
    auto called = request(
        control,
        "POST",
        "/mcp",
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"admission_policy","arguments":{}}})");
    EXPECT_NE(text(called.body).find("attestation_source"), std::string::npos);
}

TEST(AdmissionServer, starts_insecure_http_through_the_cli) {
    const auto root = liquid::test::fixture_path("admission-server-insecure");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto statement = mint_with_cli(root);
    write_policy(root / "policy.json",
                 policy_for(statement.issuer.taap_id, root / ".liquid-store"));
    const auto port = free_port();
    const auto control_port = free_port();
    auto server = child_process{server_arguments(root, port, control_port, false)};
    const auto control = "http://127.0.0.1:" + std::to_string(control_port);
    wait_until_ready(control);
    const auto review = read_file(std::filesystem::path{LIQUID_ADMISSION_FIXTURES} / "review.json");
    auto accepted =
        request("http://127.0.0.1:" + std::to_string(port), "POST", "/admission/review", review);
    EXPECT_NE(text(accepted.body).find(R"("allowed": true)"), std::string::npos)
        << text(accepted.body);
}

TEST(AdmissionServer, refuses_when_the_network_source_is_down) {
    const auto root = liquid::test::fixture_path("admission-server-down-source");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / ".liquid-store");
    const auto source = "http://127.0.0.1:1";
    auto policy = liquid::admission_policy{
        .mode = liquid::admission_mode::attestation,
        .issuers = {"taap:1:ed25519:issuer"},
        .profiles = {"image_provenance/1"},
        .attestation_source = source,
        .log = {.lake = "", .max_checkpoint_age_seconds = 60},
    };
    write_policy(root / "policy.json", policy);
    const auto port = free_port();
    const auto control_port = free_port();
    auto arguments = server_arguments(root, port, control_port, false);
    const auto store_option = std::ranges::find(arguments, "--store");
    ASSERT_NE(store_option, arguments.end());
    *store_option = "--attestation-source";
    *std::next(store_option) = source;
    auto server = child_process{std::move(arguments)};
    const auto control = "http://127.0.0.1:" + std::to_string(control_port);
    wait_until_healthy(control);
    EXPECT_EQ(request(control, "GET", "/readyz").status, 503U);
    const auto review = read_file(std::filesystem::path{LIQUID_ADMISSION_FIXTURES} / "review.json");
    auto refused =
        request("http://127.0.0.1:" + std::to_string(port), "POST", "/admission/review", review);
    EXPECT_NE(text(refused.body).find("attestation source unavailable"), std::string::npos);
}

} // namespace
