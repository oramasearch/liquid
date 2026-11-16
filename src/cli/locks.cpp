// SPDX-License-Identifier: MIT

#include <filesystem>
#include <print>
#include <string_view>

#include "liquid/daemon.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace liquid_cli {

[[nodiscard]] auto ensure_daemon(const std::filesystem::path& checkout) -> liquid::result<void>;

namespace {

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

auto daemon_lock(std::string path, std::string reason) -> liquid::result<void> {
    const auto checkout = std::filesystem::current_path();
    auto ready = ensure_daemon(checkout);
    if (!ready) {
        if (ready.error().code != liquid::errc::unsupported) {
            return std::unexpected(ready.error());
        }
        auto repository = open_repository();
        if (!repository) {
            return std::unexpected(repository.error());
        }
        return liquid::runtime::block_on(repository->lock(std::move(path), std::move(reason)));
    }
    auto daemon = liquid::runtime::block_on(liquid::daemon::attach(checkout));
    if (!daemon) {
        return std::unexpected(daemon.error());
    }
    return liquid::runtime::block_on(daemon->lock(std::move(path), std::move(reason)));
}

auto daemon_unlock(std::string path) -> liquid::result<void> {
    const auto checkout = std::filesystem::current_path();
    auto ready = ensure_daemon(checkout);
    if (!ready) {
        if (ready.error().code != liquid::errc::unsupported) {
            return std::unexpected(ready.error());
        }
        auto repository = open_repository();
        if (!repository) {
            return std::unexpected(repository.error());
        }
        return liquid::runtime::block_on(repository->unlock(std::move(path)));
    }
    auto daemon = liquid::runtime::block_on(liquid::daemon::attach(checkout));
    if (!daemon) {
        return std::unexpected(daemon.error());
    }
    return liquid::runtime::block_on(daemon->unlock(std::move(path)));
}

} // namespace

auto locks_lock(int argc, char** argv) -> int {
    if (argc != 1 && argc != 3) {
        return usage("lock requires <path> [--reason <text>]");
    }
    auto reason = std::string{"exclusive checkout"};
    if (argc == 3) {
        if (std::string_view{argv[1]} != "--reason" || std::string_view{argv[2]}.empty()) {
            return usage("lock requires <path> [--reason <text>]");
        }
        reason = argv[2];
    }
    auto locked = daemon_lock(argv[0], std::move(reason));
    if (!locked) {
        return fail(locked.error());
    }
    std::println("locked {}", argv[0]);
    return 0;
}

auto locks_unlock(int argc, char** argv) -> int {
    if (argc != 1) {
        return usage("unlock requires one path");
    }
    auto unlocked = daemon_unlock(argv[0]);
    if (!unlocked) {
        return fail(unlocked.error());
    }
    std::println("unlocked {}", argv[0]);
    return 0;
}

} // namespace liquid_cli
