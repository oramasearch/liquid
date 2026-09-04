#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <type_traits>
#include <vector>

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
        co_return std::unexpected(liquid::error{liquid::errc::internal, wait_error.message(), "capture test"});
    co_return {};
}

class workspace {
  public:
    workspace() : path_(std::filesystem::temp_directory_path() / "liquid-capture-test") {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~workspace() { std::filesystem::remove_all(path_); }
    [[nodiscard]] auto path() const -> const std::filesystem::path & { return path_; }

  private:
    std::filesystem::path path_;
};

auto open_capture(const std::filesystem::path &path, liquid::capture_options options = {})
    -> liquid::capture {
    options.coalesce_window = std::chrono::milliseconds{25};
    auto opened = liquid::runtime::block_on(liquid::capture::open(path, std::move(options)));
    EXPECT_TRUE(opened.has_value()) << (opened ? "" : opened.error().message);
    return std::move(*opened);
}

void wait() {
    auto completed = liquid::runtime::block_on(wait_for_capture(std::chrono::milliseconds{500}));
    EXPECT_TRUE(completed.has_value()) << (completed ? "" : completed.error().message);
}

} // namespace

TEST(Drip, atomic_rename_is_one_target_drip) {
    workspace files;
    auto capture = open_capture(files.path());
    wait();
    auto drips = std::vector<liquid::drip>{};
    auto subscribed = capture.subscribe([&](const liquid::drip &drip) { drips.push_back(drip); });
    ASSERT_TRUE(subscribed);
    {
        auto output = std::ofstream{files.path() / ".notes.txt.tmp"};
        output << "saved atomically\n";
    }
    std::filesystem::rename(files.path() / ".notes.txt.tmp", files.path() / "notes.txt");
    wait();
    auto capture_status = capture.status();
    ASSERT_TRUE(capture_status);
    ASSERT_EQ(drips.size(), 1) << "pending=" << capture_status->pending_drips
                               << " cursor=" << capture_status->last_cursor.token;
    ASSERT_EQ(drips.front().ops.size(), 1);
    EXPECT_EQ(drips.front().ops.front().path, "notes.txt");
    EXPECT_TRUE(liquid::runtime::block_on(capture.close()));
}

TEST(Drip, writes_in_one_window_are_one_drip) {
    workspace files;
    auto capture = open_capture(files.path());
    wait();
    auto drips = std::vector<liquid::drip>{};
    auto subscribed = capture.subscribe([&](const liquid::drip &drip) { drips.push_back(drip); });
    ASSERT_TRUE(subscribed);
    for (auto index = 0; index < 40; ++index) {
        auto output = std::ofstream{files.path() / ("file-" + std::to_string(index) + ".txt")};
        output << index;
    }
    wait();
    ASSERT_EQ(drips.size(), 1);
    EXPECT_EQ(drips.front().ops.size(), 40);
    EXPECT_TRUE(liquid::runtime::block_on(capture.close()));
}

TEST(Drip, excluded_and_paused_paths_do_not_emit_until_resumed) {
    workspace files;
    auto capture = open_capture(files.path(), {.exclude = {"ignored/**", "ignored.txt"}});
    wait();
    auto drips = std::vector<liquid::drip>{};
    auto subscribed = capture.subscribe([&](const liquid::drip &drip) { drips.push_back(drip); });
    ASSERT_TRUE(subscribed);
    std::filesystem::create_directories(files.path() / "ignored");
    { auto output = std::ofstream{files.path() / "ignored" / "skip.txt"}; output << "skip"; }
    { auto output = std::ofstream{files.path() / "ignored.txt"}; output << "skip"; }
    wait();
    EXPECT_TRUE(drips.empty());
    ASSERT_TRUE(liquid::runtime::block_on(capture.pause()));
    { auto output = std::ofstream{files.path() / "paused.txt"}; output << "paused"; }
    wait();
    EXPECT_TRUE(drips.empty());
    auto status = capture.status();
    ASSERT_TRUE(status);
    EXPECT_EQ(status->state, liquid::capture_status::state::paused);
    ASSERT_TRUE(liquid::runtime::block_on(capture.resume()));
    wait();
    ASSERT_EQ(drips.size(), 1);
    EXPECT_EQ(drips.front().ops.front().path, "paused.txt");
    EXPECT_TRUE(liquid::runtime::block_on(capture.close()));
}

TEST(Drip, restart_replays_the_persisted_journal) {
    workspace files;
    {
        auto capture = open_capture(files.path());
        wait();
        auto subscribed = capture.subscribe([](const liquid::drip &) {});
        ASSERT_TRUE(subscribed);
        { auto output = std::ofstream{files.path() / "restart.txt"}; output << "restart"; }
        wait();
        EXPECT_TRUE(liquid::runtime::block_on(capture.close()));
    }
    auto capture = open_capture(files.path());
    auto drips = std::vector<liquid::drip>{};
    auto subscribed = capture.subscribe([&](const liquid::drip &drip) { drips.push_back(drip); });
    ASSERT_TRUE(subscribed);
    ASSERT_EQ(drips.size(), 1);
    EXPECT_EQ(drips.front().ops.front().path, "restart.txt");
    EXPECT_TRUE(liquid::runtime::block_on(capture.close()));
}
