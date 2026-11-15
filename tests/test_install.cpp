// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "liquid/install.hpp"
#include "temporary_directory.hpp"

namespace {

auto write_text(const std::filesystem::path& path, std::string_view value) -> void {
    auto code = std::error_code{};
    std::filesystem::create_directories(path.parent_path(), code);
    ASSERT_FALSE(code) << code.message();
    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
    output << value;
    ASSERT_TRUE(output);
}

auto write_bytes(const std::filesystem::path& path, std::span<const std::byte> value) -> void {
    auto code = std::error_code{};
    std::filesystem::create_directories(path.parent_path(), code);
    ASSERT_FALSE(code) << code.message();
    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
    output.write(reinterpret_cast<const char*>(value.data()),
                 static_cast<std::streamsize>(value.size()));
    ASSERT_TRUE(output);
}

auto read_text(const std::filesystem::path& path) -> std::string {
    auto input = std::ifstream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

auto make_tree(const std::filesystem::path& path, std::string_view version) -> void {
    write_text(path / "bin/tool", version);
    write_text(path / "share/data", std::string{"data-"} + std::string{version});
}

TEST(Install, generations_switch_atomically_and_rollback) {
    const auto workspace = liquid::test::fixture_path("liquid-install-generations");
    const auto first = workspace / "first";
    const auto second = workspace / "second";
    const auto root = workspace / "root";
    make_tree(first, "one");
    make_tree(second, "two");

    auto installed_first = liquid::runtime::block_on(liquid::installation::install(first, root));
    ASSERT_TRUE(installed_first) << installed_first.error().message;
    EXPECT_EQ(installed_first->generation.number, 1U);
    EXPECT_EQ(read_text(root / "current/bin/tool"), "one");

    auto installed_second = liquid::runtime::block_on(liquid::installation::install(second, root));
    ASSERT_TRUE(installed_second) << installed_second.error().message;
    EXPECT_EQ(installed_second->generation.number, 2U);
    EXPECT_EQ(read_text(root / "current/bin/tool"), "two");

    auto generations = liquid::runtime::block_on(liquid::installation::list(root));
    ASSERT_TRUE(generations) << generations.error().message;
    ASSERT_EQ(generations->size(), 2U);
    EXPECT_FALSE(generations->front().current);
    EXPECT_TRUE(generations->back().current);

    auto rolled_back = liquid::runtime::block_on(liquid::installation::rollback(root));
    ASSERT_TRUE(rolled_back) << rolled_back.error().message;
    EXPECT_EQ(rolled_back->number, 1U);
    EXPECT_EQ(read_text(root / "current/bin/tool"), "one");
}

TEST(Install, embedded_intent_is_applied_inside_the_generation) {
    const auto workspace = liquid::test::fixture_path("liquid-install-intent");
    const auto tree = workspace / "tree";
    const auto root = workspace / "root";
    write_text(tree / "usr/bin/agent", "agent");
    const auto intent =
        liquid::install_intent{.services = {{"liquid_agent",
                                             "/usr/bin/agent",
                                             "liquid_agent",
                                             {"network_online"},
                                             liquid::service_restart::always}},
                               .users = {{"liquid_agent", true, "/var/lib/liquid-agent"}},
                               .groups = {{"liquid_agent", true}},
                               .directories = {{"/var/lib/liquid-agent", 0750, "liquid_agent"}},
                               .symlinks = {{"/usr/bin/agent-current", "/usr/bin/agent"}},
                               .alternatives = {{"liquid-tool", "/usr/bin/agent", 40}}};
    auto encoded = liquid::encode_install_intent(intent);
    ASSERT_TRUE(encoded) << encoded.error().message;
    write_bytes(tree / "usr/share/liquid/intents/agent.cbor", *encoded);

    auto installed = liquid::runtime::block_on(liquid::installation::install(tree, root));
    ASSERT_TRUE(installed) << installed.error().message;
    EXPECT_NE(read_text(root / "current/etc/passwd").find("liquid_agent:x:"), std::string::npos);
    EXPECT_NE(read_text(root / "current/etc/group").find("liquid_agent:x:"), std::string::npos);
    EXPECT_EQ(std::filesystem::read_symlink(root / "current/usr/bin/agent-current"),
              "/usr/bin/agent");
    EXPECT_EQ(std::filesystem::read_symlink(root / "current/usr/bin/liquid-tool"),
              "/usr/bin/agent");
    EXPECT_EQ(std::filesystem::read_symlink(
                  root / "current/etc/systemd/system/multi-user.target.wants/liquid_agent.service"),
              "/usr/lib/systemd/system/liquid_agent.service");
}

#ifndef _WIN32
auto run_crashing_install(const std::filesystem::path& source,
                          const std::filesystem::path& root,
                          std::uint64_t step) -> int {
    const auto child = ::fork();
    if (child == 0) {
        const auto step_text = std::to_string(step);
        ::execl(LIQUID_INSTALL_CRASH_FIXTURE,
                LIQUID_INSTALL_CRASH_FIXTURE,
                source.c_str(),
                root.c_str(),
                step_text.c_str(),
                static_cast<char*>(nullptr));
        std::_Exit(127);
    }
    if (child < 0) {
        return -1;
    }
    auto status = 0;
    if (::waitpid(child, &status, 0) != child) {
        return -1;
    }
    return status;
}

TEST(Install, sigkill_at_each_fault_keeps_the_previous_generation_current) {
    const auto workspace = liquid::test::fixture_path("liquid-install-kill");
    const auto old_tree = workspace / "old";
    const auto new_tree = workspace / "new";
    make_tree(old_tree, "old");
    make_tree(new_tree, "new");

    constexpr auto fault_count = std::uint64_t{3};
    for (auto step = std::uint64_t{1}; step <= fault_count; ++step) {
        const auto root = workspace / ("root-" + std::to_string(step));
        auto baseline = liquid::runtime::block_on(liquid::installation::install(old_tree, root));
        ASSERT_TRUE(baseline) << baseline.error().message;

        const auto status = run_crashing_install(new_tree, root, step);
        ASSERT_NE(status, -1);
        ASSERT_TRUE(WIFSIGNALED(status));
        EXPECT_EQ(WTERMSIG(status), SIGKILL);
        EXPECT_EQ(read_text(root / "current/bin/tool"), "old");

        auto recovered = liquid::runtime::block_on(liquid::installation::install(new_tree, root));
        ASSERT_TRUE(recovered) << recovered.error().message;
        EXPECT_EQ(read_text(root / "current/bin/tool"), "new");
    }

    const auto complete_root = workspace / "complete";
    const auto status = run_crashing_install(new_tree, complete_root, fault_count + 1);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    EXPECT_EQ(read_text(complete_root / "current/bin/tool"), "new");
}
#endif

} // namespace
