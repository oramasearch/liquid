// SPDX-License-Identifier: MIT

#include <filesystem>
#include <fstream>
#include <map>
#include <print>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

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
    const auto alias = sandbox / input.relative_path();
    std::filesystem::create_directories(alias.parent_path(), code);
    if (code) {
        std::println(stderr, "cannot create offline path alias: {}", code.message());
        return 1;
    }
    std::filesystem::create_directory_symlink("/liquid/work", alias, code);
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
