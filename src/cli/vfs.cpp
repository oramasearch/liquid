// SPDX-License-Identifier: MIT

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <utility>

#include "liquid/credential.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "liquid/vfs.hpp"

namespace liquid_cli {
namespace {

volatile std::sig_atomic_t interrupted = 0;

auto on_interrupt(int) -> void {
    interrupted = 1;
}

auto fail(const liquid::error& error) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(error.code), error.message, error.context);
    return 1;
}

auto usage(std::string_view message) -> int {
    std::println(stderr, "error: usage: {} ()", message);
    return 2;
}

auto open_repository() -> liquid::result<liquid::repository> {
    return liquid::runtime::block_on(liquid::repository::open(std::filesystem::current_path()));
}

auto wait_for_interrupt() -> liquid::runtime::task<void> {
    while (!interrupted) {
        auto [delay_error] = co_await liquid::runtime::delay(std::chrono::milliseconds{100});
        if (delay_error) {
            co_return std::unexpected(
                liquid::error{liquid::errc::io, delay_error.message(), "VFS mount"});
        }
    }
    co_return liquid::result<void>{};
}

} // namespace

auto vfs_mount(int argc, char** argv) -> int {
    if (argc != 1 && argc != 2) {
        return usage("mount requires a mountpoint and optional --read-only");
    }
    auto options = liquid::mount_options{};
    if (const auto token = liquid::lake_token(); !token.empty()) {
        options.bearer_token = token;
    }
    if (argc == 2) {
        if (std::string_view{argv[1]} != "--read-only") {
            return usage("mount accepts only --read-only");
        }
        options.read_only = true;
    }
    auto repository = open_repository();
    if (!repository) {
        return fail(repository.error());
    }
    auto mounted = liquid::runtime::block_on(liquid::vfs::mount(*repository, argv[0], options));
    if (!mounted) {
        return fail(mounted.error());
    }
    std::println("mounted {}", argv[0]);
    interrupted = 0;
    auto previous = std::signal(SIGINT, on_interrupt);
    auto waited = liquid::runtime::block_on(wait_for_interrupt());
    std::signal(SIGINT, previous);
    if (!waited) {
        return fail(waited.error());
    }
    auto unmounted = liquid::runtime::block_on(mounted->unmount());
    if (!unmounted) {
        return fail(unmounted.error());
    }
    std::println("unmounted {}", argv[0]);
    return 0;
}

auto vfs_materialise(int argc, char** argv) -> int {
    if (argc != 1 && argc != 2) {
        return usage("materialise requires a target and optional path");
    }
    auto repository = open_repository();
    if (!repository) {
        return fail(repository.error());
    }
    auto prefix = argc == 2 ? std::optional<std::string>{argv[1]} : std::nullopt;
    auto materialised = liquid::runtime::block_on(
        liquid::vfs::materialise(*repository, argv[0], std::move(prefix)));
    if (!materialised) {
        return fail(materialised.error());
    }
    std::println("materialised {}", argv[0]);
    return 0;
}

} // namespace liquid_cli
