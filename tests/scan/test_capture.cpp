#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "liquid/drip.hpp"
#include "liquid/runtime.hpp"
#include "liquid/store.hpp"
#include "objects/records.hpp"

namespace {

class scan_workspace {
public:
    scan_workspace()
        : path_(std::filesystem::temp_directory_path() /
                ("liquid-scan-test-" + std::to_string(next_.fetch_add(1)))) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }

    ~scan_workspace() {
        std::filesystem::remove_all(path_);
    }

    [[nodiscard]] auto path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    inline static std::atomic<std::uint64_t> next_{};
    std::filesystem::path path_;
};

class capture_events {
public:
    void add(const liquid::drip& value) {
        std::lock_guard lock(mutex_);
        values_.push_back(value);
    }

    [[nodiscard]] auto values() const -> std::vector<liquid::drip> {
        std::lock_guard lock(mutex_);
        return values_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<liquid::drip> values_;
};

auto wait_for_count(const capture_events& events, std::size_t count)
    -> liquid::runtime::task<void> {
    for (auto elapsed = std::chrono::milliseconds{}; elapsed < std::chrono::seconds{4};
         elapsed += std::chrono::milliseconds{10}) {
        if (events.values().size() >= count) {
            co_return liquid::result<void>{};
        }
        auto [delay_error] = co_await liquid::runtime::delay(std::chrono::milliseconds{10});
        if (delay_error) {
            co_return std::unexpected(
                liquid::error{liquid::errc::internal, delay_error.message(), "scan test"});
        }
    }
    co_return std::unexpected(
        liquid::error{liquid::errc::unavailable, "capture event was not observed", "scan test"});
}

void write_text(const std::filesystem::path& path, std::string_view value) {
    auto output = std::ofstream{path, std::ios::trunc};
    ASSERT_TRUE(output.good());
    output << value;
    ASSERT_TRUE(output.good());
}

auto open_capture(const scan_workspace& workspace, liquid::capture_options options = {})
    -> liquid::capture {
    options.coalesce_window = std::chrono::milliseconds{20};
    options.reconcile_interval = std::chrono::milliseconds{100};
    auto opened = liquid::runtime::block_on(liquid::capture::open(workspace.path(), options));
    EXPECT_TRUE(opened.has_value()) << (opened ? "" : opened.error().message);
    if (!opened) {
        throw std::runtime_error("capture did not open");
    }
    return std::move(*opened);
}

TEST(InlineScanning, blocked_event_discloses_only_safe_coordinates) {
    scan_workspace workspace;
    auto capture = open_capture(workspace);
    capture_events events;
    auto subscription = capture.subscribe([&](const liquid::drip& value) { events.add(value); });
    ASSERT_TRUE(subscription);
    const auto credential = std::string{"AKIA"} + "0123456789ABCDEF";
    write_text(workspace.path() / "settings.env", "key=" + credential + "\n");
    auto waited = liquid::runtime::block_on(wait_for_count(events, 1));
    ASSERT_TRUE(waited) << waited.error().message;
    const auto values = events.values();
    ASSERT_EQ(values.size(), 1U);
    ASSERT_TRUE(values.front().block);
    EXPECT_EQ(values.front().block->code, liquid::errc::unauthorized);
    EXPECT_EQ(values.front().block->path, "settings.env");
    EXPECT_EQ(values.front().block->detector, "aws-access-key");
    EXPECT_EQ(values.front().block->line, 1U);
    const auto safe_message = values.front().block->path + values.front().block->detector;
    EXPECT_FALSE(safe_message.contains(credential));
    const auto status = capture.status();
    ASSERT_TRUE(status);
    EXPECT_GE(status->blocked_drips, 1U);
    ASSERT_TRUE(status->last_block);
    EXPECT_EQ(status->last_block->detector, "aws-access-key");
    EXPECT_TRUE(liquid::runtime::block_on(capture.close()));
    EXPECT_EQ(std::filesystem::file_size(workspace.path() / ".liquid" / "capture" / "drips.cbor"),
              0U);
}

TEST(InlineScanning, configured_exceptions_are_attested) {
    scan_workspace workspace;
    liquid::capture_options options;
    options.scan_config.allow.push_back({"fixtures/**", {"aws-access-key"}});
    auto capture = open_capture(workspace, options);
    capture_events events;
    auto subscription = capture.subscribe([&](const liquid::drip& value) { events.add(value); });
    ASSERT_TRUE(subscription);
    std::filesystem::create_directories(workspace.path() / "fixtures");
    const auto first = std::string{"AKIA"} + "0123456789ABCDEF";
    const auto second = std::string{"AIza"} + "0123456789abcdefghijABCDEFGHIJ-_xyz";
    write_text(workspace.path() / "fixtures" / "allowed.env",
               "key=" + first + "\nkey=" + second + " liquid:allow-secret\n");
    auto waited = liquid::runtime::block_on(wait_for_count(events, 1));
    ASSERT_TRUE(waited) << waited.error().message;
    const auto values = events.values();
    ASSERT_FALSE(values.empty());
    const auto accepted = std::ranges::find_if(
        values, [](const auto& value) { return value.scan_attestation.has_value(); });
    ASSERT_NE(accepted, values.end());
    EXPECT_FALSE(accepted->block);
    EXPECT_TRUE(liquid::runtime::block_on(capture.close()));

    auto store = liquid::runtime::block_on(
        liquid::store::open(workspace.path() / ".liquid" / "capture" / "store"));
    ASSERT_TRUE(store) << store.error().message;
    auto encoded = liquid::runtime::block_on(store->get(*accepted->scan_attestation));
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto attestation = liquid::objects::decode_attestation(*encoded);
    ASSERT_TRUE(attestation) << attestation.error().message;
    EXPECT_EQ(attestation->profile, "scan_result/1");
    EXPECT_GE(attestation->findings_count, 2U);
    EXPECT_FALSE(attestation->detectors.empty());
    EXPECT_FALSE(attestation->allowances.empty());
    EXPECT_TRUE(std::ranges::any_of(
        attestation->allowances, [](const auto& value) { return value.reason == "allow-list"; }));
    EXPECT_TRUE(std::ranges::any_of(
        attestation->allowances, [](const auto& value) { return value.reason == "line-marker"; }));
    auto verified = liquid::verify(*attestation);
    ASSERT_TRUE(verified) << verified.error().message;
    EXPECT_TRUE(*verified);
}

TEST(InlineScanning, accepted_drip_always_references_scan_result) {
    scan_workspace workspace;
    auto capture = open_capture(workspace);
    capture_events events;
    auto subscription = capture.subscribe([&](const liquid::drip& value) { events.add(value); });
    ASSERT_TRUE(subscription);
    write_text(workspace.path() / "notes.txt", "ordinary content\n");
    auto waited = liquid::runtime::block_on(wait_for_count(events, 1));
    ASSERT_TRUE(waited) << waited.error().message;
    const auto values = events.values();
    ASSERT_EQ(values.size(), 1U);
    EXPECT_TRUE(values.front().scan_attestation.has_value());
    EXPECT_FALSE(values.front().block.has_value());
    EXPECT_TRUE(liquid::runtime::block_on(capture.close()));
}

} // namespace
