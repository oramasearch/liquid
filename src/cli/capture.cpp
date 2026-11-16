// SPDX-License-Identifier: MIT

#include <chrono>
#include <csignal>
#include <filesystem>
#include <print>
#include <string_view>
#include <thread>
#include <utility>

#include "liquid/daemon.hpp"
#include "liquid/drip.hpp"
#include "liquid/runtime.hpp"

namespace liquid_cli {

[[nodiscard]] auto ensure_daemon(const std::filesystem::path& checkout) -> liquid::result<void>;

namespace {

volatile std::sig_atomic_t interrupted = 0;

auto fail(const liquid::error& error) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(error.code), error.message, error.context);
    return 1;
}

auto usage(std::string_view message) -> int {
    std::println(stderr, "error: usage: {} ()", message);
    return 2;
}

auto state_name(enum liquid::capture_status::state state) -> std::string_view {
    if (state == liquid::capture_status::state::paused) {
        return "paused";
    }
    return "running";
}

auto print_status(const liquid::capture_status& status) -> void {
    std::println("capture_state={}", state_name(status.state));
    std::println("capture_pending_drips={}", status.pending_drips);
    std::println("capture_overflow_count={}", status.overflow_count);
    std::println("capture_recrawl_count={}", status.recrawl_count);
    std::println("capture_cursor={}", status.last_cursor.token);
}

auto toggle_capture(bool pause) -> int {
    auto ready = ensure_daemon(std::filesystem::current_path());
    if (!ready) {
        return fail(ready.error());
    }
    auto attached =
        liquid::runtime::block_on(liquid::daemon::attach(std::filesystem::current_path()));
    if (!attached) {
        return fail(attached.error());
    }
    auto changed = pause ? liquid::runtime::block_on(attached->pause_capture())
                         : liquid::runtime::block_on(attached->resume_capture());
    if (!changed) {
        return fail(changed.error());
    }
    auto daemon_status = liquid::runtime::block_on(attached->status());
    if (!daemon_status) {
        return fail(daemon_status.error());
    }
    print_status(daemon_status->capture);
    return 0;
}

auto handle_interrupt(int) -> void {
    interrupted = 1;
}

} // namespace

auto capture_pause(int argc, char** argv) -> int {
    if (argc != 0) {
        return usage("pause accepts no arguments");
    }
    return toggle_capture(true);
}

auto capture_resume(int argc, char** argv) -> int {
    if (argc != 0) {
        return usage("resume accepts no arguments");
    }
    return toggle_capture(false);
}

auto capture_watch(int argc, char** argv) -> int {
    if (argc != 0) {
        return usage("watch accepts no arguments");
    }
    auto ready = ensure_daemon(std::filesystem::current_path());
    if (!ready) {
        return fail(ready.error());
    }
    auto attached =
        liquid::runtime::block_on(liquid::daemon::attach(std::filesystem::current_path()));
    if (!attached) {
        return fail(attached.error());
    }
    interrupted = 0;
    auto previous = std::signal(SIGINT, handle_interrupt);
    std::println("watching {}", std::filesystem::current_path().string());
    auto last = std::string{};
    while (interrupted == 0) {
        auto status = liquid::runtime::block_on(attached->status());
        if (!status) {
            std::signal(SIGINT, previous);
            return fail(status.error());
        }
        const auto current = status->last_drip ? liquid::to_string(status->last_drip->value) : "";
        if (!current.empty() && current != last) {
            for (const auto& path : status->invalidated_paths) {
                std::println("drip={} path={}", current, path);
            }
            last = current;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    std::signal(SIGINT, previous);
    return 0;
}

} // namespace liquid_cli
