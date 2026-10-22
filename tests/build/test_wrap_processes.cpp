#include <gtest/gtest.h>

#if defined(__linux__)
#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <spawn.h>
#include <stop_token>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../temporary_directory.hpp"
#include "sandbox/process_identity.h"
#include "sandbox/sandbox.hpp"

namespace {

class wrap_process_fixture {
public:
    wrap_process_fixture() {
        std::filesystem::create_directories(root.path() / "work");
        for (const auto* name : {"hook", "trace", "plan"})
            std::ofstream{root.path() / name};
    }

    auto request(std::string command, bool network = true)
        -> liquid::sandbox_detail::wrap_launch_request {
        return {.sandbox_root = root.path() / "sandbox",
                .input_mount = root.path() / "work",
                .hook = root.path() / "hook",
                .trace = root.path() / "trace",
                .plan = root.path() / "plan",
                .stdout_path = root.path() / "stdout",
                .stderr_path = root.path() / "stderr",
                .command = {"/bin/sh", "-c", std::move(command)},
                .network = network};
    }

    auto read(std::string_view name) -> std::string {
        auto input = std::ifstream{root.path() / name};
        return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }

    auto namespace_is_gone() -> bool {
        auto identity = read("work/namespace");
        if (identity.ends_with('\n'))
            identity.pop_back();
        if (identity.empty())
            return false;
        for (const auto& entry : std::filesystem::directory_iterator{"/proc"}) {
            auto code = std::error_code{};
            const auto current = std::filesystem::read_symlink(entry.path() / "ns/pid", code);
            if (!code && current.string() == identity)
                return false;
        }
        return true;
    }

    auto wait_for_namespace_exit() -> bool {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!namespace_is_gone()) {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        return true;
    }

    liquid::test::detail::process_fixture_directory root;
};

auto namespaces_available() -> bool {
    return std::system("/usr/bin/unshare --user --map-root-user --mount --pid --fork "
                       "/bin/true >/dev/null 2>&1") == 0;
}

} // namespace

TEST(WrapProcesses, every_network_mode_requires_a_pid_namespace_and_launcher_death_cleanup) {
    for (const auto network : {false, true}) {
        const auto arguments = liquid::sandbox_detail::wrap_namespace_arguments(network);
        for (const auto* required : {"--user", "--mount", "--pid", "--fork", "--kill-child=KILL"})
            EXPECT_NE(std::ranges::find(arguments, required), arguments.end()) << required;
        EXPECT_EQ(std::ranges::find(arguments, "--net") != arguments.end(), !network);
    }
}

TEST(WrapProcesses, bootstrap_mounts_private_procfs_even_when_namespaces_are_unavailable) {
    wrap_process_fixture fixture;
    auto launched = liquid::sandbox_detail::launch_wrap(fixture.request("exit 0"));
    ASSERT_TRUE(launched) << launched.error().message;
    const auto script = fixture.read("sandbox/wrap-bootstrap.sh");
    EXPECT_NE(script.find("mount -t proc -o nosuid,nodev,noexec proc "), std::string::npos);
    EXPECT_EQ(script.find("--rbind /proc"), std::string::npos);
    EXPECT_NE(script.find(" -- /liquid/wrap-init /usr/bin/env -i"), std::string::npos);
}

TEST(WrapProcesses, bootstrap_assigns_a_new_scope_after_each_launch_finishes) {
    wrap_process_fixture fixture;
    auto scopes = std::vector<std::string>{};
    for (auto index = 0; index != 2; ++index) {
        auto launched = liquid::sandbox_detail::launch_wrap(fixture.request("exit 0"));
        ASSERT_TRUE(launched) << launched.error().message;
        const auto script = fixture.read("sandbox/wrap-bootstrap.sh");
        const auto prefix = std::string{LIQUID_WRAP_LAUNCH_SEQUENCE "="};
        const auto found = script.find(prefix);
        ASSERT_NE(found, std::string::npos);
        const auto start = found + prefix.size();
        scopes.push_back(script.substr(start, script.find('\'', start) - start));
        EXPECT_NE(script.find("namespace-pinned"), std::string::npos);
    }
    EXPECT_NE(scopes[0], scopes[1]);
}

TEST(WrapProcesses, host_and_local_observers_name_the_same_process) {
    auto direct = std::array<char, 128>{};
    auto parent = std::array<char, 128>{};
    ASSERT_EQ(liquid_process_identity(::getpid(), direct.data(), direct.size()), 0);
    const auto child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        auto observed = std::array<char, 128>{};
        const auto result = liquid_process_identity(::getppid(), observed.data(), observed.size());
        ::_exit(result == 0 && std::string{observed.data()} == direct.data() ? 0 : 1);
    }
    auto status = int{};
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    auto proc_parent = long{};
    ASSERT_EQ(liquid_process_identity_at("self", parent.data(), parent.size(), &proc_parent), 0);
    EXPECT_EQ(std::string{direct.data()}, parent.data());
    EXPECT_EQ(proc_parent, ::getppid());
}

TEST(WrapProcesses, exec_hook_and_probe_agree_on_scope_and_refuse_malformed_values) {
    wrap_process_fixture fixture;
    for (const auto& scope : std::vector<std::string>{
             "41", "42", "0", "-1", "18446744073709551616", std::string(200, '1')}) {
        auto arguments = std::array<std::string, 2>{LIQUID_WRAP_PROBE, "--identity"};
        auto pointers = std::array<char*, 3>{arguments[0].data(), arguments[1].data(), nullptr};
        std::ofstream{fixture.root.path() / "trace", std::ios::trunc};
        auto environment = std::array<std::string, 4>{
            std::string{LIQUID_WRAP_LAUNCH_SEQUENCE "="} + scope,
            std::string{"LD_PRELOAD="} + LIQUID_TEST_WRAP_HOOK,
            "LIQUID_WRAP_TRACE=" + (fixture.root.path() / "trace").string(),
            "LIQUID_WRAP_ACTIVE=1"};
        auto environment_pointers = std::array<char*, 5>{environment[0].data(),
                                                         environment[1].data(),
                                                         environment[2].data(),
                                                         environment[3].data(),
                                                         nullptr};
        const auto output = ::open((fixture.root.path() / "scope").c_str(),
                                   O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                                   0600);
        ASSERT_GE(output, 0);
        auto actions = posix_spawn_file_actions_t{};
        ::posix_spawn_file_actions_init(&actions);
        ::posix_spawn_file_actions_adddup2(&actions, output, STDOUT_FILENO);
        ::posix_spawn_file_actions_addclose(&actions, output);
        auto pid = pid_t{};
        const auto spawned = ::posix_spawn(&pid,
                                           arguments[0].c_str(),
                                           &actions,
                                           nullptr,
                                           pointers.data(),
                                           environment_pointers.data());
        ::posix_spawn_file_actions_destroy(&actions);
        ::close(output);
        ASSERT_EQ(spawned, 0);
        auto status = int{};
        ASSERT_EQ(::waitpid(pid, &status, 0), pid);
        ASSERT_TRUE(WIFEXITED(status));
        const auto valid = std::string_view{scope} == "41" || std::string_view{scope} == "42";
        EXPECT_EQ(WEXITSTATUS(status), valid ? 0 : 4);
        if (valid) {
            auto observed = fixture.read("scope");
            ASSERT_TRUE(observed.ends_with('\n'));
            observed.pop_back();
            EXPECT_TRUE(observed.starts_with(scope + ":"));
            const auto trace = fixture.read("trace");
            EXPECT_EQ(trace.substr(0, trace.find('\t')), observed);
        } else {
            EXPECT_TRUE(fixture.read("trace").empty());
        }
    }
}

TEST(WrapProcesses, private_tree_cannot_see_or_signal_a_same_user_host_sentinel) {
    if (!namespaces_available())
        GTEST_SKIP() << "runner denies unprivileged user/mount/PID namespaces";
    // This test process is a live same-user sentinel outside both wrapped process trees.
    const auto sentinel = std::to_string(::getpid());
    const auto host_namespace = std::filesystem::read_symlink("/proc/self/ns/pid").string();
    for (const auto network : {false, true}) {
        wrap_process_fixture fixture;
        auto request = fixture.request(
            "test -d /proc/1 || exit 90; "
            "test \"$(readlink /proc/self/ns/pid)\" != '" +
                host_namespace +
                "' || exit 91; "
                "while read -r name value; do if [ \"$name\" = NSpid: ]; then "
                "set -- $value; [ \"$#\" -eq 1 ] || exit 92; seen=1; fi; done < /proc/self/status; "
                "[ \"${seen:-0}\" -eq 1 ] || exit 93; "
                "! readlink /proc/1/ns/pid >/dev/null 2>&1 || exit 94; "
                // A host PID can equal an unrelated private PID; that alias belongs to this tree.
                "if [ ! -e /proc/" +
                sentinel + " ]; then ! kill -0 " + sentinel + " 2>/dev/null; fi",
            network);
        auto launched = liquid::sandbox_detail::launch_wrap(request);
        ASSERT_TRUE(launched) << launched.error().message;
        EXPECT_EQ(launched->exit, 0) << fixture.read("stderr");
    }
}

TEST(WrapProcesses, parallel_namespace_traces_match_parent_observations_without_pid_collisions) {
    if (!namespaces_available())
        GTEST_SKIP() << "runner denies unprivileged user/mount/PID namespaces";
    wrap_process_fixture first;
    wrap_process_fixture second;
    auto first_stop = std::stop_source{};
    auto second_stop = std::stop_source{};
    const auto start = [](wrap_process_fixture& fixture, std::stop_token stop) {
        auto request = fixture.request("echo ready > ready; sleep 60");
        request.hook = LIQUID_TEST_WRAP_HOOK;
        request.environment["LD_PRELOAD"] = "/liquid/wrap-hook.so";
        request.stop = stop;
        return std::async(std::launch::async, [request = std::move(request)] {
            return liquid::sandbox_detail::launch_wrap(request);
        });
    };
    auto first_run = start(first, first_stop.get_token());
    auto second_run = start(second, second_stop.get_token());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while ((!std::filesystem::exists(first.root.path() / "work/ready") ||
            !std::filesystem::exists(second.root.path() / "work/ready")) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    const auto first_trace = first.read("trace");
    const auto second_trace = second.read("trace");
    const auto first_identity = first_trace.substr(0, first_trace.find('\t'));
    const auto second_identity = second_trace.substr(0, second_trace.find('\t'));
    auto first_seen = false;
    auto second_seen = false;
    for (const auto& entry : std::filesystem::directory_iterator{"/proc"}) {
        const auto name = entry.path().filename().string();
        if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos)
            continue;
        auto identity = std::array<char, 128>{};
        if (liquid_process_identity(std::stol(name), identity.data(), identity.size()) == 0) {
            first_seen |= first_identity == identity.data();
            second_seen |= second_identity == identity.data();
        }
    }
    first_stop.request_stop();
    second_stop.request_stop();
    EXPECT_TRUE(first_run.get());
    EXPECT_TRUE(second_run.get());
    EXPECT_FALSE(first_identity.empty());
    EXPECT_FALSE(second_identity.empty());
    EXPECT_NE(first_identity, second_identity);
    EXPECT_TRUE(first_seen);
    EXPECT_TRUE(second_seen);
}

TEST(WrapProcesses, init_preserves_exit_status_and_reaps_detached_children_on_exit) {
    if (!namespaces_available())
        GTEST_SKIP() << "runner denies unprivileged user/mount/PID namespaces";
    wrap_process_fixture fixture;
    auto launched = liquid::sandbox_detail::launch_wrap(
        fixture.request("readlink /proc/self/ns/pid > namespace; "
                        "setsid /bin/sh -c 'sleep 0.01 &' ; sleep 0.2; "
                        "for status in /proc/[0-9]*/status; do "
                        "if grep -q '^State:.*Z' \"$status\"; then exit 91; fi; done; "
                        "setsid /bin/sh -c 'sleep 60' & exit 37"));
    ASSERT_TRUE(launched) << launched.error().message;
    EXPECT_EQ(launched->exit, 37) << fixture.read("stderr");
    EXPECT_TRUE(fixture.namespace_is_gone());
}

TEST(WrapProcesses, cancellation_terminates_a_detached_signal_ignoring_tree) {
    if (!namespaces_available())
        GTEST_SKIP() << "runner denies unprivileged user/mount/PID namespaces";
    wrap_process_fixture fixture;
    auto stop = std::stop_source{};
    auto request = fixture.request(
        "readlink /proc/self/ns/pid > namespace; "
        "setsid /bin/sh -c 'trap \"\" TERM INT; echo ready > ready; sleep 60' & wait");
    request.stop = stop.get_token();
    auto future = std::async(std::launch::async,
                             [&] { return liquid::sandbox_detail::launch_wrap(request); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!std::filesystem::exists(fixture.root.path() / "work/ready") &&
           future.wait_for(std::chrono::milliseconds{10}) != std::future_status::ready &&
           std::chrono::steady_clock::now() < deadline) {
    }
    stop.request_stop();
    ASSERT_EQ(future.wait_for(std::chrono::seconds{5}), std::future_status::ready);
    auto launched = future.get();
    ASSERT_TRUE(launched) << launched.error().message;
    EXPECT_TRUE(launched->cancelled);
    EXPECT_NE(launched->exit, 0);
    EXPECT_TRUE(fixture.wait_for_namespace_exit());
}

TEST(WrapProcesses, namespace_init_accepts_termination_signals) {
    if (!namespaces_available())
        GTEST_SKIP() << "runner denies unprivileged user/mount/PID namespaces";
    wrap_process_fixture fixture;
    auto stop = std::stop_source{};
    auto request = fixture.request("kill -TERM 1; sleep 60");
    request.stop = stop.get_token();
    auto future = std::async(std::launch::async,
                             [&] { return liquid::sandbox_detail::launch_wrap(request); });
    const auto terminated = future.wait_for(std::chrono::seconds{5}) == std::future_status::ready;
    if (!terminated)
        stop.request_stop();
    auto launched = future.get();
    ASSERT_TRUE(launched) << launched.error().message;
    EXPECT_TRUE(terminated);
    EXPECT_EQ(launched->exit, 128 + SIGTERM);
    EXPECT_FALSE(launched->cancelled);
}

TEST(WrapProcesses, caller_death_terminates_the_namespace_without_a_stop_callback) {
    if (!namespaces_available())
        GTEST_SKIP() << "runner denies unprivileged user/mount/PID namespaces";
    wrap_process_fixture fixture;
    const auto caller = ::fork();
    ASSERT_GE(caller, 0);
    if (caller == 0) {
        auto launched = liquid::sandbox_detail::launch_wrap(fixture.request(
            "readlink /proc/self/ns/pid > namespace; "
            "setsid /bin/sh -c 'trap \"\" TERM INT; echo ready > ready; sleep 60' & wait"));
        ::_exit(launched ? launched->exit : 125);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!std::filesystem::exists(fixture.root.path() / "work/ready") &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    const auto started = std::filesystem::exists(fixture.root.path() / "work/ready");
    ::kill(caller, SIGKILL);
    auto status = int{};
    ASSERT_EQ(::waitpid(caller, &status, 0), caller);
    ASSERT_TRUE(started) << fixture.read("stderr");
    EXPECT_TRUE(fixture.wait_for_namespace_exit());
}
#endif
