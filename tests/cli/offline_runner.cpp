// SPDX-License-Identifier: MIT

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#endif

#include "sandbox/sandbox.hpp"

namespace {

auto usage() -> int {
    std::println(stderr, "usage: liquid-offline-runner <sandbox> <input> -- <command> [args...]");
    return 2;
}

auto touch(const std::filesystem::path& path) -> bool {
    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
    return static_cast<bool>(output);
}

#if defined(__linux__)
// Why this host cannot give the runner the namespaces its sandbox needs, or nothing when it
// can. The sandbox runs `unshare --user --map-root-user --mount --net`; an unprivileged
// container commonly refuses that. The probe runs the same unshare with /bin/true, so a host
// that allows it runs the suite in full (liquid#363).
auto namespaces_unavailable() -> std::optional<std::string> {
    char unshare[] = "/usr/bin/unshare";
    char user[] = "--user";
    char root[] = "--map-root-user";
    char mount[] = "--mount";
    char net[] = "--net";
    char command[] = "/bin/true";
    char* arguments[] = {unshare, user, root, mount, net, command, nullptr};
    char* environment[] = {nullptr};
    auto actions = posix_spawn_file_actions_t{};
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    auto pid = pid_t{};
    const auto spawned = posix_spawn(&pid, unshare, &actions, nullptr, arguments, environment);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) {
        return std::string{"cannot run /usr/bin/unshare: "} + std::strerror(spawned);
    }
    auto status = int{};
    if (::waitpid(pid, &status, 0) != pid) {
        return std::string{"cannot wait for /usr/bin/unshare: "} + std::strerror(errno);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return "this host does not permit user, mount and network namespaces "
               "(unshare --user --map-root-user --mount --net exited " +
               std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status)) +
               ")";
    }
    return std::nullopt;
}

// The alias is made by an earlier run of the same sandbox directory too; a link that already
// names the input is the one this run would make.
auto link_input(const std::filesystem::path& alias, std::error_code& code) -> void {
    const auto target = std::filesystem::path{"/liquid/work"};
    auto existing = std::error_code{};
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(alias, existing)) &&
        std::filesystem::read_symlink(alias, existing) == target && !existing) {
        code.clear();
        return;
    }
    std::filesystem::create_directory_symlink(target, alias, code);
}
#endif

} // namespace

int main(int argc, char** argv) {
    if (argc < 5 || std::string_view{argv[3]} != "--") {
        return usage();
    }
    auto code = std::error_code{};
    const auto sandbox = std::filesystem::absolute(argv[1]);
    const auto input = std::filesystem::canonical(argv[2], code);
    if (code) {
        std::println(stderr, "cannot resolve offline input: {}", code.message());
        return 1;
    }
    std::filesystem::create_directories(sandbox, code);
    if (code) {
        std::println(stderr, "cannot create offline sandbox: {}", code.message());
        return 1;
    }
#if defined(__linux__)
    if (const auto reason = namespaces_unavailable()) {
        // Exit 77 is the runner's skip: the calling script reports it as a skipped test.
        std::println("liquid-offline-runner: skipped: {}", *reason);
        return 77;
    }
    const auto alias = sandbox / input.relative_path();
    std::filesystem::create_directories(alias.parent_path(), code);
    if (code) {
        std::println(stderr, "cannot create offline path alias: {}", code.message());
        return 1;
    }
    link_input(alias, code);
    if (code) {
        std::println(stderr, "cannot link offline input: {}", code.message());
        return 1;
    }
#endif
    const auto trace = sandbox / "trace";
    const auto plan = sandbox / "plan";
    if (!touch(trace) || !touch(plan)) {
        std::println(stderr, "cannot create offline sandbox inputs");
        return 1;
    }
    auto command = std::vector<std::string>{};
    command.reserve(static_cast<std::size_t>(argc - 4));
    for (auto index = 4; index < argc; ++index) {
        command.emplace_back(argv[index]);
    }
    auto home = std::filesystem::path{"/tmp/home"};
#if defined(__APPLE__)
    home = std::filesystem::path{"/tmp"} / ("liquid-offline-" + std::to_string(::getpid()));
    std::filesystem::create_directories(home, code);
    if (code) {
        std::println(stderr, "cannot create offline temporary directory: {}", code.message());
        return 1;
    }
#endif
    const auto environment = std::map<std::string, std::string>{
        {"HOME", home.string()},
        {"LIQUID_OFFLINE", "1"},
        {"PATH", "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"},
        {"TMPDIR", home.parent_path().string()},
    };
    auto launched = liquid::sandbox_detail::launch_wrap({.sandbox_root = sandbox,
                                                         .input_mount = input,
                                                         .hook = std::filesystem::absolute(argv[0]),
                                                         .trace = trace,
                                                         .plan = plan,
                                                         .command = std::move(command),
                                                         .environment = environment,
                                                         .network = false,
                                                         .network_only = true});
    if (!launched) {
#if defined(__APPLE__)
        std::filesystem::remove_all(home, code);
#endif
        std::println(stderr,
                     "offline sandbox failed: {} ({})",
                     launched.error().message,
                     launched.error().context);
        return 1;
    }
    const auto exit = launched->exit;
#if defined(__APPLE__)
    std::filesystem::remove_all(home, code);
#endif
    return exit;
}
