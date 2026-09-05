#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <vector>

#include "capture/watcher.hpp"
#include "liquid/drip.hpp"
#include "temporary_directory.hpp"

namespace {

struct sample {
    std::chrono::nanoseconds append;
    std::chrono::nanoseconds scan;
    std::chrono::nanoseconds visible;
};

auto wait_for_capture(std::chrono::milliseconds duration) -> liquid::runtime::task<void> {
    auto [wait_error] = co_await liquid::runtime::delay(duration);
    if (wait_error)
        co_return std::unexpected(
            liquid::error{liquid::errc::internal, wait_error.message(), "capture measure"});
    co_return {};
}

auto wait_for_observation(std::mutex* mutex,
                          std::optional<std::chrono::steady_clock::time_point>* observation,
                          std::chrono::milliseconds timeout) -> liquid::runtime::task<bool> {
    for (auto elapsed = std::chrono::milliseconds{}; elapsed < timeout;
         elapsed += std::chrono::milliseconds{10}) {
        {
            std::lock_guard lock(*mutex);
            if (*observation)
                co_return true;
        }
        auto [wait_error] = co_await liquid::runtime::delay(std::chrono::milliseconds{10});
        if (wait_error)
            co_return std::unexpected(
                liquid::error{liquid::errc::internal, wait_error.message(), "capture measure"});
    }
    co_return false;
}

auto percentile(std::vector<std::chrono::nanoseconds> values, unsigned percent)
    -> std::chrono::nanoseconds {
    std::ranges::sort(values);
    const auto index = (values.size() * percent + 99) / 100 - 1;
    return values[index];
}

auto run_sample(std::chrono::milliseconds window, unsigned index) -> std::optional<sample> {
    const auto workspace = liquid::test::fixture_path(
        "liquid-capture-measure-" + std::to_string(window.count()) + "-" + std::to_string(index) +
        "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace);
    auto capture =
        liquid::runtime::block_on(liquid::capture::open(workspace, {.coalesce_window = window}));
    if (!capture) {
        std::cerr << capture.error().message << '\n';
        return std::nullopt;
    }
    std::mutex mutex;
    auto observed = std::optional<std::chrono::steady_clock::time_point>{};
    const auto warmed = liquid::runtime::block_on(wait_for_capture(std::chrono::seconds{3}));
    if (!warmed) {
        std::cerr << warmed.error().message << '\n';
        return std::nullopt;
    }
    auto subscription = capture->subscribe([&](const liquid::drip&) {
        std::lock_guard lock(mutex);
        if (!observed) {
            observed = std::chrono::steady_clock::now();
        }
    });
    if (!subscription) {
        std::cerr << subscription.error().message << '\n';
        return std::nullopt;
    }
    const auto started = std::chrono::steady_clock::now();
    for (auto file = 0; file < 100; ++file) {
        auto output = std::ofstream{workspace / ("file-" + std::to_string(file) + ".txt")};
        output << "captured";
    }
    const auto observed_before_timeout = liquid::runtime::block_on(
        wait_for_observation(&mutex, &observed, std::chrono::seconds{20}));
    if (!observed_before_timeout) {
        std::cerr << observed_before_timeout.error().message << '\n';
        return std::nullopt;
    }
    if (!*observed_before_timeout) {
        std::cerr << "no capture event" << '\n';
        return std::nullopt;
    }
    auto output = std::optional<sample>{};
    {
        std::lock_guard lock(mutex);
        output.emplace(liquid::capture_detail::last_journal_append_duration_for_test(*capture),
                       liquid::capture_detail::last_scan_duration_for_test(*capture),
                       *observed - started);
    }
    auto closed = liquid::runtime::block_on(capture->close());
    std::filesystem::remove_all(workspace);
    if (!closed) {
        std::cerr << closed.error().message << '\n';
        return std::nullopt;
    }
    return output;
}

void report(std::chrono::milliseconds window, const std::vector<sample>& samples) {
    auto appends = std::vector<std::chrono::nanoseconds>{};
    auto scans = std::vector<std::chrono::nanoseconds>{};
    auto visible = std::vector<std::chrono::nanoseconds>{};
    for (const auto& value : samples) {
        appends.push_back(value.append);
        scans.push_back(value.scan);
        visible.push_back(value.visible);
    }
    const auto append_p50 = percentile(appends, 50);
    const auto append_p99 = percentile(std::move(appends), 99);
    const auto scan_p50 = percentile(scans, 50);
    const auto scan_p99 = percentile(std::move(scans), 99);
    const auto visible_p50 = percentile(visible, 50);
    const auto visible_p99 = percentile(std::move(visible), 99);
    std::cout << "window=" << window.count() << "ms append-p50=" << append_p50.count() / 1000.0
              << "us append-p99=" << append_p99.count() / 1000.0
              << "us scan-p50=" << scan_p50.count() / 1000.0
              << "us scan-p99=" << scan_p99.count() / 1000.0
              << "us visible-p50=" << visible_p50.count() / 1000000.0
              << "ms visible-p99=" << visible_p99.count() / 1000000.0 << "ms\n";
}

} // namespace

int main(int argc, char* argv[]) {
    constexpr auto runs = 1U;
    const auto window =
        std::chrono::milliseconds{argc == 2 ? std::strtol(argv[1], nullptr, 10) : 200};
    {
        auto samples = std::vector<sample>{};
        samples.reserve(runs);
        for (auto index = 0U; index < runs; ++index) {
            auto value = run_sample(window, index);
            if (!value)
                return 1;
            samples.push_back(*value);
        }
        report(window, samples);
    }
}
