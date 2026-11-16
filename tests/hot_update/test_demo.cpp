// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "../fuse_probe.hpp"
#include "../socket_fixture.hpp"
#include "../temporary_directory.hpp"
#include "build/cache.hpp"
#include "build/provenance.hpp"
#include "build/repository_access.hpp"
#include "liquid/daemon.hpp"
#include "liquid/hot_abi.h"
#include "liquid/hot_update.hpp"
#include "liquid/repository.hpp"
#include "objects/records.hpp"

namespace {

auto test_digest(unsigned char value) -> liquid::digest {
    auto output = liquid::digest{liquid::algorithm::blake3, {}};
    output.bytes.front() = std::byte{value};
    return output;
}

auto read_file(const std::filesystem::path& path) -> std::vector<std::byte> {
    auto stream = std::ifstream{path, std::ios::binary | std::ios::ate};
    const auto size = stream.tellg();
    auto output = std::vector<std::byte>(static_cast<std::size_t>(size));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(output.data()), size);
    return output;
}

auto bytes(std::string_view value) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

auto host_platform() -> liquid::platform {
#if defined(__linux__)
    return {"linux", "x86_64"};
#else
    return {"macos", "arm64"};
#endif
}

auto store_blob(liquid::repository& source, std::vector<std::byte> contents)
    -> liquid::result<liquid::digest> {
    const auto size = contents.size();
    auto chunk = liquid::runtime::block_on(
        liquid::repository_detail::build_access::put_record(source, std::move(contents)));
    if (!chunk) {
        return std::unexpected(chunk.error());
    }
    auto encoded = liquid::objects::encode(liquid::blob{{*chunk}, size, {size}});
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return liquid::runtime::block_on(
        liquid::repository_detail::build_access::put_record(source, std::move(*encoded)));
}

auto cached_result(liquid::repository& source,
                   liquid::action_input input,
                   std::vector<std::byte> output) -> liquid::result<liquid::result_id> {
    auto output_blob = store_blob(source, std::move(output));
    auto empty_blob = store_blob(source, {});
    if (!output_blob || !empty_blob) {
        return std::unexpected(output_blob ? empty_blob.error() : output_blob.error());
    }
    const auto sandbox = test_digest(79);
    auto value = liquid::action_result{
        .outputs = {{"out/demo-target", 0100644, *output_blob}},
        .stdout_output = *empty_blob,
        .stderr_output = *empty_blob,
        .worker = "prepared-demo",
        .observed_at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()),
    };
    auto stored =
        liquid::runtime::block_on(liquid::build_detail::testing::store_result_with_provenance(
            source, liquid::objects::action_record{input, sandbox}, value));
    if (!stored) {
        return std::unexpected(stored.error());
    }
    auto cache = liquid::build_detail::disk_cache{source};
    auto opened = liquid::runtime::block_on(cache.open());
    if (!opened) {
        return std::unexpected(opened.error());
    }
    auto candidate = cache.candidate_key(input, sandbox);
    if (!candidate) {
        return std::unexpected(candidate.error());
    }
    auto published = liquid::runtime::block_on(cache.publish(*candidate, *stored, value));
    if (!published) {
        return std::unexpected(published.error());
    }
    return *stored;
}

auto wait_for_hot_update(liquid::daemon& daemon, std::string_view expected)
    -> liquid::result<liquid::daemon_status> {
    auto observed = std::string{};
    for (auto attempt = 0; attempt != 1000; ++attempt) {
        auto status = liquid::runtime::block_on(daemon.status());
        if (!status) {
            return std::unexpected(status.error());
        }
        const auto target = std::ranges::find(
            status->targets, std::string{"hot-update"}, &liquid::daemon_target_status::name);
        if (target != status->targets.end()) {
            observed = "behind=" + std::to_string(target->behind) +
                       " deliveries=" + std::to_string(target->deliveries) +
                       " error=" + target->last_error.value_or("") +
                       " detail=" + target->detail.value_or("");
        }
        if (target != status->targets.end() && target->behind == 0 && target->detail &&
            target->detail->contains(expected)) {
            return status;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return std::unexpected(liquid::error{liquid::errc::unavailable,
                                         "daemon did not complete the hot-update drip: " + observed,
                                         "hot-update demo"});
}

TEST(HotUpdateDemo, TierOnePatchChangesARunningCapyLoop) {
    const auto root = liquid::test::fixture_path("liquid-hot-update-demo");
    std::filesystem::remove_all(root);
    auto socket = liquid::test::socket_fixture{};
    const auto endpoint = socket.path();
    auto options = liquid::repository_options{};
    options.cache.publish = true;
    options.verify.rate = std::numeric_limits<std::uint32_t>::max();
    options.hot_update.restart_hook = {LIQUID_HOT_DEMO_RESTART_HOOK,
                                       (root / "restart.txt").string()};
    options.hot_update.targets = {{"out/demo-target", endpoint.string()}};
    auto opened = liquid::runtime::block_on(liquid::repository::create(root, options));
    ASSERT_TRUE(opened) << opened.error().message;
    auto source = std::move(*opened);
    ASSERT_TRUE(liquid::runtime::block_on(source.write("input.txt", bytes("old"))));
    ASSERT_TRUE(liquid::runtime::block_on(source.write("out/demo-target", {})));
    ASSERT_TRUE(liquid::runtime::block_on(source.cut("prepare old demo input")));
    auto old_root = liquid::runtime::block_on(source.root_digest());
    ASSERT_TRUE(old_root) << old_root.error().message;
    auto input = liquid::action_input{.command = {"prepared-hot-update"},
                                      .outputs = {"out/demo-target"},
                                      .inputs = *old_root,
                                      .toolchain = *old_root,
                                      .platform = host_platform(),
                                      .timeout = std::chrono::seconds{3}};
    auto old_result = cached_result(source, input, bytes("old"));
    ASSERT_TRUE(old_result) << old_result.error().message;

    auto patch_object =
        liquid::runtime::block_on(liquid::repository_detail::build_access::put_record(
            source, read_file(LIQUID_HOT_DEMO_PATCH)));
    ASSERT_TRUE(patch_object) << patch_object.error().message;
    auto old_evidence = liquid::hot_update_build_evidence{
        .result = *old_result,
        .artifact = *patch_object,
        .changed_input = "input.txt",
        .units = {{liquid::frontend_unit_kind::definition,
                   "demo_value",
                   "int()",
                   test_digest(81),
                   test_digest(82)}},
    };
    ASSERT_TRUE(liquid::runtime::block_on(
        liquid::hot_update::record_evidence(source, std::move(old_evidence))));

    ASSERT_TRUE(liquid::runtime::block_on(source.write("input.txt", bytes("new"))));
    ASSERT_TRUE(liquid::runtime::block_on(source.cut("prepare new demo input")));
    auto new_root = liquid::runtime::block_on(source.root_digest());
    ASSERT_TRUE(new_root) << new_root.error().message;
    input.inputs = *new_root;
    auto new_result = cached_result(source, input, bytes("new"));
    ASSERT_TRUE(new_result) << new_result.error().message;
    auto new_evidence = liquid::hot_update_build_evidence{
        .result = *new_result,
        .artifact = *patch_object,
        .changed_input = "input.txt",
        .units = {{liquid::frontend_unit_kind::definition,
                   "demo_value",
                   "int()",
                   test_digest(83),
                   test_digest(84)}},
    };
    ASSERT_TRUE(liquid::runtime::block_on(
        liquid::hot_update::record_evidence(source, std::move(new_evidence))));

    ASSERT_TRUE(liquid::runtime::block_on(source.write("input.txt", bytes("old"))));
    ASSERT_TRUE(liquid::runtime::block_on(source.cut("restore old demo input")));
    ASSERT_TRUE(liquid::runtime::block_on(source.set_metadata(
        "hot_update/current/out/demo-target", bytes(liquid::to_string(old_result->value)))));
    std::filesystem::create_directories(root / ".liquid-build");
    {
        auto configuration = std::ofstream{root / ".liquid-build" / "configuration"};
        ASSERT_TRUE(configuration);
        configuration << "A\t1\t" << liquid::to_string(old_result->value) << '\n';
    }
    std::filesystem::create_directories(root / "out");
    {
        auto old_input = std::ofstream{root / "input.txt"};
        old_input << "old";
    }
    {
        auto target = std::ofstream{root / "out" / "demo-target"};
    }

    int output_pipe[2];
    ASSERT_EQ(::pipe(output_pipe), 0);
    const auto child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        ::close(output_pipe[0]);
        ::dup2(output_pipe[1], STDOUT_FILENO);
        ::close(output_pipe[1]);
        ::execl(
            LIQUID_HOT_DEMO, LIQUID_HOT_DEMO, endpoint.c_str(), "300", static_cast<char*>(nullptr));
        _exit(127);
    }
    ::close(output_pipe[1]);
    auto* output = ::fdopen(output_pipe[0], "r");
    ASSERT_NE(output, nullptr);
    char line[64];
    ASSERT_NE(std::fgets(line, sizeof(line), output), nullptr);
    EXPECT_STREQ(line, "value=1\n");

    auto daemon_options = liquid::daemon_options{};
    daemon_options.capture.scan = false;
    daemon_options.capture.coalesce_window = std::chrono::milliseconds{5};
    daemon_options.capture.reconcile_interval = std::chrono::milliseconds{0};
    auto daemon = liquid::runtime::block_on(liquid::daemon::start(root, daemon_options));
    ASSERT_TRUE(daemon) << daemon.error().message;
    {
        auto changed = std::ofstream{root / "input.txt", std::ios::trunc};
        changed << "new";
    }
    auto applied = wait_for_hot_update(*daemon, "out/demo-target outcome=patched tier=1");
    if (!applied) {
        static_cast<void>(liquid::runtime::block_on(daemon->shutdown()));
        static_cast<void>(liquid::runtime::block_on(daemon->wait()));
    }
    ASSERT_TRUE(applied) << applied.error().message;
    auto saw_replacement = false;
    while (std::fgets(line, sizeof(line), output) != nullptr) {
        if (std::string_view{line} == "value=2\n") {
            saw_replacement = true;
            break;
        }
    }
    EXPECT_TRUE(saw_replacement);
    EXPECT_TRUE(liquid::runtime::block_on(daemon->shutdown()));
    EXPECT_TRUE(liquid::runtime::block_on(daemon->wait()));
    std::fclose(output);

    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    std::printf("drip=input.txt tier=1 outcome=patched pid=%d before=value=1 after=value=2\n",
                static_cast<int>(child));
    std::filesystem::remove_all(root);
}

TEST(HotUpdateDemo, SocketPathLimitNamesTheEndpointAndNativeCapacity) {
    auto socket = liquid::test::socket_fixture{};
    const auto capacity = sizeof(sockaddr_un::sun_path);
    auto endpoint = socket.path().string();
    endpoint.append(capacity - 1 - endpoint.size(), 'x');

    ASSERT_EQ(liquid_hot_listen(endpoint.c_str()), LIQUID_HOT_OK) << liquid_hot_last_error();
    EXPECT_TRUE(std::filesystem::exists(endpoint));
    const auto overlong = endpoint + 'x';
    EXPECT_EQ(liquid_hot_listen(overlong.c_str()), LIQUID_HOT_INVALID);
    EXPECT_EQ(std::string{liquid_hot_last_error()},
              "socket path exceeds the platform limit (" + std::to_string(capacity + 1) + " > " +
                  std::to_string(capacity) + " bytes including NUL): '" + overlong + "'");
    EXPECT_TRUE(std::filesystem::exists(endpoint));
    liquid_hot_close();
    EXPECT_FALSE(std::filesystem::exists(endpoint));
}

TEST(HotUpdateDemo, TierTwoLoadsAnAdditiveObject) {
    const auto payload =
        std::string{"liquid-hot/1\n"} + LIQUID_HOT_DEMO_PATCH + "\ntier=2\ndemo_added=demo_added\n";

    ASSERT_EQ(liquid_hot_apply(payload.data(), payload.size()), LIQUID_HOT_OK)
        << liquid_hot_last_error();
    EXPECT_EQ(liquid_hot_quiescent(), LIQUID_HOT_OK) << liquid_hot_last_error();
    liquid_hot_close();
}

TEST(HotUpdateDemo, TierThreeInvokesRestartHook) {
    if (!LIQUID_BUILD_HAS_FUSE) {
        std::printf("tier=3 outcome=restart hook=skipped reason=sandbox-backend-unavailable\n");
        GTEST_SKIP() << "restart hooks require the configured sandbox backend";
    }
    if (const auto reason = liquid::test::fuse_unavailable_reason()) {
        std::printf("tier=3 outcome=restart hook=skipped reason=sandbox-backend-unavailable\n");
        GTEST_SKIP() << "restart hooks require the configured sandbox backend: " << *reason;
    }
    const auto root = liquid::test::fixture_path("liquid-hot-update-restart-demo");
    std::filesystem::remove_all(root);
    const auto transcript = root / "restart.txt";
    auto options = liquid::repository_options{};
    options.cache.publish = true;
    options.verify.rate = std::numeric_limits<std::uint32_t>::max();
    options.hot_update.restart_hook = {"./restart-hook", "restart.txt"};
    options.hot_update.targets = {{"out/demo-target", (root / "unused.sock").string()}};
    auto opened = liquid::runtime::block_on(liquid::repository::create(root, options));
    ASSERT_TRUE(opened) << opened.error().message;
    auto source = std::move(*opened);
    ASSERT_TRUE(liquid::runtime::block_on(source.write("input.txt", bytes("old-layout"))));
    ASSERT_TRUE(liquid::runtime::block_on(source.write("out/demo-target", {})));
    ASSERT_TRUE(liquid::runtime::block_on(source.cut("prepare old layout")));
    auto old_root = liquid::runtime::block_on(source.root_digest());
    ASSERT_TRUE(old_root) << old_root.error().message;
    auto action = liquid::action_input{.command = {"prepared-hot-update"},
                                       .outputs = {"out/demo-target"},
                                       .inputs = *old_root,
                                       .toolchain = *old_root,
                                       .platform = host_platform(),
                                       .timeout = std::chrono::seconds{3}};
    auto old_result = cached_result(source, action, bytes("old-layout"));
    ASSERT_TRUE(old_result) << old_result.error().message;
    auto old_evidence = liquid::hot_update_build_evidence{
        .result = *old_result,
        .artifact = test_digest(120),
        .changed_input = "input.txt",
        .units = {{liquid::frontend_unit_kind::declaration,
                   "demo_record",
                   "struct demo_record { int value; }",
                   test_digest(121),
                   test_digest(122)}},
    };

    ASSERT_TRUE(liquid::runtime::block_on(source.write("input.txt", bytes("new-layout"))));
    ASSERT_TRUE(liquid::runtime::block_on(source.cut("prepare new layout")));
    auto new_root = liquid::runtime::block_on(source.root_digest());
    ASSERT_TRUE(new_root) << new_root.error().message;
    action.inputs = *new_root;
    auto new_result = cached_result(source, action, bytes("new-layout"));
    ASSERT_TRUE(new_result) << new_result.error().message;
    auto new_evidence = liquid::hot_update_build_evidence{
        .result = *new_result,
        .artifact = test_digest(123),
        .changed_input = "input.txt",
        .units = {{liquid::frontend_unit_kind::declaration,
                   "demo_record",
                   "struct demo_record { long value; }",
                   test_digest(124),
                   test_digest(125)}},
    };
    ASSERT_TRUE(liquid::runtime::block_on(
        liquid::hot_update::record_evidence(source, std::move(old_evidence))));
    ASSERT_TRUE(liquid::runtime::block_on(
        liquid::hot_update::record_evidence(source, std::move(new_evidence))));

    ASSERT_TRUE(liquid::runtime::block_on(source.write("input.txt", bytes("old-layout"))));
    ASSERT_TRUE(liquid::runtime::block_on(source.cut("restore old layout")));
    ASSERT_TRUE(liquid::runtime::block_on(source.set_metadata(
        "hot_update/current/out/demo-target", bytes(liquid::to_string(old_result->value)))));
    std::filesystem::create_directories(root / ".liquid-build");
    {
        auto configuration = std::ofstream{root / ".liquid-build" / "configuration"};
        ASSERT_TRUE(configuration);
        configuration << "A\t1\t" << liquid::to_string(old_result->value) << '\n';
    }
    std::filesystem::create_directories(root / "out");
    std::filesystem::copy_file(LIQUID_HOT_DEMO_RESTART_HOOK,
                               root / "restart-hook",
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::permissions(root / "restart-hook",
                                 std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::add);
    {
        auto old_input = std::ofstream{root / "input.txt"};
        old_input << "old-layout";
    }
    {
        auto target = std::ofstream{root / "out" / "demo-target"};
    }

    auto daemon_options = liquid::daemon_options{};
    daemon_options.capture.scan = false;
    daemon_options.capture.exclude = {"restart.txt"};
    daemon_options.capture.coalesce_window = std::chrono::milliseconds{5};
    daemon_options.capture.reconcile_interval = std::chrono::milliseconds{0};
    auto daemon = liquid::runtime::block_on(liquid::daemon::start(root, daemon_options));
    ASSERT_TRUE(daemon) << daemon.error().message;
    {
        auto changed = std::ofstream{root / "input.txt", std::ios::trunc};
        changed << "new-layout";
    }
    auto restarted = wait_for_hot_update(*daemon, "out/demo-target outcome=restart tier=3");
    if (!restarted) {
        static_cast<void>(liquid::runtime::block_on(daemon->shutdown()));
        static_cast<void>(liquid::runtime::block_on(daemon->wait()));
    }
    ASSERT_TRUE(restarted) << restarted.error().message;
    EXPECT_TRUE(liquid::runtime::block_on(daemon->shutdown()));
    EXPECT_TRUE(liquid::runtime::block_on(daemon->wait()));

    auto input = std::ifstream{transcript};
    auto line = std::string{};
    ASSERT_TRUE(std::getline(input, line));
    EXPECT_EQ(line,
              "outcome=restart old=" + liquid::to_string(old_result->value) +
                  " new=" + liquid::to_string(new_result->value) + " tier=3");
    const auto hot_target = std::ranges::find(
        restarted->targets, std::string{"hot-update"}, &liquid::daemon_target_status::name);
    ASSERT_NE(hot_target, restarted->targets.end());
    std::printf("drip=input.txt tier=3 %s status=%s\n",
                line.c_str(),
                hot_target->detail.value_or("").c_str());
    std::filesystem::remove_all(root);
}

} // namespace
