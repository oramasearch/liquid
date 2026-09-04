#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <vector>

#include "liquid/drip.hpp"

namespace {

auto wait_for_drip(std::chrono::milliseconds duration) -> liquid::runtime::task<void> {
    auto [wait_error] = co_await liquid::runtime::delay(duration);
    if (wait_error)
        co_return std::unexpected(liquid::error{liquid::errc::internal, wait_error.message(), "capture measure"});
    co_return {};
}

} // namespace

int main() {
    const auto workspace = std::filesystem::temp_directory_path() / "liquid-capture-measure";
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace);
    auto capture = liquid::runtime::block_on(liquid::capture::open(workspace));
    if (!capture) {
        std::cerr << capture.error().message << '\n';
        return 1;
    }
    std::mutex mutex;
    auto visible = std::vector<std::chrono::steady_clock::time_point>{};
    auto subscription = capture->subscribe([&](const liquid::drip &) {
        std::lock_guard lock(mutex);
        visible.push_back(std::chrono::steady_clock::now());
    });
    if (!subscription) {
        std::cerr << subscription.error().message << '\n';
        return 1;
    }
    {
        std::lock_guard lock(mutex);
        visible.clear();
    }
    const auto started = std::chrono::steady_clock::now();
    for (auto index = 0; index < 100; ++index) {
        auto output = std::ofstream{workspace / ("file-" + std::to_string(index) + ".txt")};
        output << "captured";
    }
    auto waited = liquid::runtime::block_on(wait_for_drip(std::chrono::seconds{2}));
    if (!waited) {
        std::cerr << waited.error().message << '\n';
        return 1;
    }
    std::chrono::milliseconds elapsed{};
    {
        std::lock_guard lock(mutex);
        if (visible.empty()) {
            std::cerr << "no capture event" << '\n';
            return 1;
        }
        elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(visible.front() - started);
    }
    std::cout << "100-file save-to-drip p50=" << elapsed.count() << "ms p99=" << elapsed.count() << "ms\n";
    auto closed = liquid::runtime::block_on(capture->close());
    std::filesystem::remove_all(workspace);
    return closed ? 0 : 1;
}
