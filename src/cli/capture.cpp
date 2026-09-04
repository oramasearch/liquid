#include <chrono>
#include <csignal>
#include <filesystem>
#include <print>
#include <string_view>
#include <thread>
#include <utility>

#include "liquid/drip.hpp"
#include "liquid/runtime.hpp"

namespace liquid_cli {
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

auto close(liquid::capture& capture) -> int {
    auto closed = liquid::runtime::block_on(capture.close());
    if (!closed) {
        return fail(closed.error());
    }
    return 0;
}

auto toggle_capture(bool pause) -> int {
    auto opened = liquid::runtime::block_on(liquid::capture::open(std::filesystem::current_path()));
    if (!opened) {
        return fail(opened.error());
    }
    auto capture = std::move(*opened);
    auto changed = pause ? liquid::runtime::block_on(capture.pause())
                         : liquid::runtime::block_on(capture.resume());
    if (!changed) {
        return fail(changed.error());
    }
    auto status = capture.status();
    if (!status) {
        return fail(status.error());
    }
    print_status(*status);
    return close(capture);
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
    auto opened = liquid::runtime::block_on(liquid::capture::open(std::filesystem::current_path()));
    if (!opened) {
        return fail(opened.error());
    }
    auto capture = std::move(*opened);
    auto subscribed = capture.subscribe([](const liquid::drip& drip) {
        for (const auto& operation : drip.ops) {
            std::println("drip={} path={}", liquid::to_string(drip.id.value), operation.path);
        }
    });
    if (!subscribed) {
        return fail(subscribed.error());
    }
    interrupted = 0;
    auto previous = std::signal(SIGINT, handle_interrupt);
    std::println("watching {}", std::filesystem::current_path().string());
    while (interrupted == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    std::signal(SIGINT, previous);
    return close(capture);
}

} // namespace liquid_cli
