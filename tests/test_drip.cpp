#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <type_traits>
#include <vector>

#include "capture/watcher.hpp"
#include "liquid/drip.hpp"
TEST(Drip, PublicHandleAndStatusShapes) {
    static_assert(sizeof(liquid::capture) == sizeof(void*));
    static_assert(sizeof(liquid::subscription) == sizeof(void*));
    static_assert(std::is_aggregate_v<liquid::capture_status>);
    static_assert(std::is_aggregate_v<liquid::capture_options>);
}

namespace {

auto wait_for_capture(std::chrono::milliseconds duration) -> liquid::runtime::task<void> {
    auto [wait_error] = co_await liquid::runtime::delay(duration);
    if (wait_error)
        co_return std::unexpected(
            liquid::error{liquid::errc::internal, wait_error.message(), "capture test"});
    co_return {};
}

class workspace {
public:
    workspace() : path_(std::filesystem::temp_directory_path() / "liquid-capture-test") {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~workspace() {
        std::filesystem::remove_all(path_);
    }
    [[nodiscard]] auto path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    std::filesystem::path path_;
};

auto open_capture(const std::filesystem::path& path, liquid::capture_options options = {})
    -> liquid::capture {
    auto opened = liquid::runtime::block_on(liquid::capture::open(path, std::move(options)));
    EXPECT_TRUE(opened.has_value()) << (opened ? "" : opened.error().message);
    return std::move(*opened);
}

void wait() {
    auto completed = liquid::runtime::block_on(wait_for_capture(std::chrono::seconds{3}));
    EXPECT_TRUE(completed.has_value()) << (completed ? "" : completed.error().message);
}

class drip_collector {
public:
    void add(const liquid::drip& value) {
        std::lock_guard lock(mutex_);
        drips_.push_back(value);
    }

    [[nodiscard]] auto values() const -> std::vector<liquid::drip> {
        std::lock_guard lock(mutex_);
        return drips_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<liquid::drip> drips_;
};

} // namespace

TEST(Drip, atomic_rename_is_one_target_drip) {
    workspace files;
    auto capture = open_capture(files.path());
    wait();
    auto drips = drip_collector{};
    auto subscribed = capture.subscribe([&](const liquid::drip& drip) { drips.add(drip); });
    ASSERT_TRUE(subscribed);
    {
        auto output = std::ofstream{files.path() / ".notes.txt.tmp"};
        output << "saved atomically\n";
    }
    std::filesystem::rename(files.path() / ".notes.txt.tmp", files.path() / "notes.txt");
    wait();
    auto capture_status = capture.status();
    ASSERT_TRUE(capture_status);
    const auto values = drips.values();
    ASSERT_EQ(values.size(), 1) << "pending=" << capture_status->pending_drips
                                << " cursor=" << capture_status->last_cursor.token;
    ASSERT_EQ(values.front().ops.size(), 1);
    EXPECT_EQ(values.front().ops.front().path, "notes.txt");
    EXPECT_TRUE(liquid::runtime::block_on(capture.close()));
}

TEST(Drip, writes_in_one_window_are_one_drip) {
    workspace files;
    auto capture = open_capture(files.path());
    wait();
    auto drips = drip_collector{};
    auto subscribed = capture.subscribe([&](const liquid::drip& drip) { drips.add(drip); });
    ASSERT_TRUE(subscribed);
    for (auto index = 0; index < 40; ++index) {
        auto output = std::ofstream{files.path() / ("file-" + std::to_string(index) + ".txt")};
        output << index;
    }
    wait();
    const auto values = drips.values();
    ASSERT_EQ(values.size(), 1);
    EXPECT_EQ(values.front().ops.size(), 40);
    EXPECT_TRUE(liquid::runtime::block_on(capture.close()));
}

TEST(Drip, excluded_and_paused_paths_do_not_emit_until_resumed) {
    workspace files;
    auto capture = open_capture(files.path(), {.exclude = {"ignored/**", "ignored.txt"}});
    wait();
    auto drips = drip_collector{};
    auto subscribed = capture.subscribe([&](const liquid::drip& drip) { drips.add(drip); });
    ASSERT_TRUE(subscribed);
    std::filesystem::create_directories(files.path() / "ignored");
    {
        auto output = std::ofstream{files.path() / "ignored" / "skip.txt"};
        output << "skip";
    }
    {
        auto output = std::ofstream{files.path() / "ignored.txt"};
        output << "skip";
    }
    wait();
    EXPECT_TRUE(drips.values().empty());
    ASSERT_TRUE(liquid::runtime::block_on(capture.pause()));
    {
        auto output = std::ofstream{files.path() / "paused.txt"};
        output << "paused";
    }
    wait();
    EXPECT_TRUE(drips.values().empty());
    auto status = capture.status();
    ASSERT_TRUE(status);
    EXPECT_EQ(status->state, liquid::capture_status::state::paused);
    ASSERT_TRUE(liquid::runtime::block_on(capture.resume()));
    wait();
    const auto values = drips.values();
    ASSERT_EQ(values.size(), 1);
    EXPECT_EQ(values.front().ops.front().path, "paused.txt");
    EXPECT_TRUE(liquid::runtime::block_on(capture.close()));
}

TEST(Drip, restart_replays_the_persisted_journal) {
    workspace files;
    {
        auto capture = open_capture(files.path());
        wait();
        auto subscribed = capture.subscribe([](const liquid::drip&) {});
        ASSERT_TRUE(subscribed);
        {
            auto output = std::ofstream{files.path() / "restart.txt"};
            output << "restart";
        }
        wait();
        EXPECT_TRUE(liquid::runtime::block_on(capture.close()));
    }
    auto capture = open_capture(files.path());
    auto drips = drip_collector{};
    auto subscribed = capture.subscribe([&](const liquid::drip& drip) { drips.add(drip); });
    ASSERT_TRUE(subscribed);
    const auto values = drips.values();
    ASSERT_EQ(values.size(), 1);
    EXPECT_EQ(values.front().ops.front().path, "restart.txt");
    EXPECT_TRUE(liquid::runtime::block_on(capture.close()));
}

TEST(Drip, overflow_recrawls_changes_made_during_the_gap) {
    workspace files;
    auto capture = open_capture(files.path());
    wait();
    const auto before = capture.status();
    ASSERT_TRUE(before);
    auto drips = drip_collector{};
    auto subscribed = capture.subscribe([&](const liquid::drip& drip) { drips.add(drip); });
    ASSERT_TRUE(subscribed);
    {
        auto output = std::ofstream{files.path() / "gap-one.txt"};
        output << "one";
    }
    {
        auto output = std::ofstream{files.path() / "gap-two.txt"};
        output << "two";
    }
    liquid::capture_detail::inject_overflow_for_test(capture);
    wait();
    const auto after = capture.status();
    ASSERT_TRUE(after);
    EXPECT_EQ(after->overflow_count, before->overflow_count + 1);
    EXPECT_EQ(after->recrawl_count, before->recrawl_count + 1);
    auto paths = std::vector<std::string>{};
    for (const auto& drip : drips.values())
        for (const auto& operation : drip.ops)
            paths.push_back(operation.path);
    EXPECT_TRUE(std::ranges::find(paths, "gap-one.txt") != paths.end());
    EXPECT_TRUE(std::ranges::find(paths, "gap-two.txt") != paths.end());
    EXPECT_TRUE(liquid::runtime::block_on(capture.close()));
}
