#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <future>
#include <thread>
#include <utility>
#include <vector>

#include "liquid/runtime.hpp"
#include "liquid/store.hpp"

namespace {

liquid::runtime::task<int> value_task(int value) {
    co_return value;
}

liquid::runtime::task<int> error_task() {
    co_return std::unexpected(liquid::error{liquid::errc::busy, "not ready", "runtime test"});
}

liquid::runtime::task<void> lazy_task(std::atomic<bool>* started) {
    started->store(true);
    co_return {};
}

liquid::runtime::task<void> void_task() {
    co_return {};
}

struct offload_observation {
    bool ran_elsewhere;
    bool resumed_on_executor;
};

liquid::runtime::task<offload_observation> observe_offload() {
    const auto before_executor = co_await boost::capy::this_coro::executor;
    const auto before_thread = std::this_thread::get_id();
    auto offloaded = co_await liquid::runtime::offload(
        [before_thread] { return std::this_thread::get_id() != before_thread; });
    if (!offloaded)
        co_return std::unexpected(offloaded.error());
    const auto after_executor = co_await boost::capy::this_coro::executor;
    co_return offload_observation{*offloaded, before_executor == after_executor};
}

liquid::runtime::task<int> combined_task() {
    auto [error, first, second] = co_await liquid::runtime::when_all(
        liquid::runtime::as_io(value_task(20)), liquid::runtime::as_io(value_task(22)));
    if (error) {
        co_return std::unexpected(
            liquid::error{liquid::errc::internal, error.message(), "runtime::when_all"});
    }
    if (!first)
        co_return std::unexpected(first.error());
    if (!second)
        co_return std::unexpected(second.error());
    co_return *first + *second;
}

liquid::runtime::task<void> store_round_trip(std::filesystem::path root) {
    auto opened = co_await liquid::store::open(std::move(root));
    if (!opened)
        co_return std::unexpected(opened.error());
    const auto input = std::vector<std::byte>{std::byte{'l'},
                                              std::byte{'i'},
                                              std::byte{'q'},
                                              std::byte{'u'},
                                              std::byte{'i'},
                                              std::byte{'d'}};
    auto stored = co_await opened->put(input);
    if (!stored)
        co_return std::unexpected(stored.error());
    auto read = co_await opened->get(*stored);
    if (!read)
        co_return std::unexpected(read.error());
    if (*read != input)
        co_return std::unexpected(
            liquid::error{liquid::errc::corrupt, "store round trip differs", "runtime test"});
    co_return {};
}

liquid::runtime::task<void> concurrent_task(std::atomic<unsigned>* active,
                                            std::atomic<unsigned>* peak) {
    const auto now = active->fetch_add(1) + 1;
    auto observed = peak->load();
    while (observed < now && !peak->compare_exchange_weak(observed, now)) {
    }
    auto [wait_error] = co_await liquid::runtime::delay(std::chrono::milliseconds{25});
    active->fetch_sub(1);
    if (wait_error)
        co_return std::unexpected(
            liquid::error{liquid::errc::internal, wait_error.message(), "runtime test"});
    co_return {};
}

} // namespace

TEST(Runtime, BlockOnReturnsValueAndError) {
    const auto value = liquid::runtime::block_on(value_task(42));
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 42);

    const auto failure = liquid::runtime::block_on(error_task());
    ASSERT_FALSE(failure.has_value());
    EXPECT_EQ(failure.error().code, liquid::errc::busy);
    EXPECT_EQ(failure.error().context, "runtime test");
}

TEST(Runtime, TasksAreLazy) {
    std::atomic<bool> started{false};
    auto pending = lazy_task(&started);
    EXPECT_FALSE(started.load());
    const auto completed = liquid::runtime::block_on(std::move(pending));
    EXPECT_TRUE(completed.has_value());
    EXPECT_TRUE(started.load());
}

TEST(Runtime, OffloadUsesPoolAndReturnsToCallerExecutor) {
    const auto observation = liquid::runtime::block_on(observe_offload());
    ASSERT_TRUE(observation.has_value());
    EXPECT_TRUE(observation->ran_elsewhere);
    EXPECT_TRUE(observation->resumed_on_executor);
}

TEST(Runtime, WhenAllComposesLiquidTasksThroughAdapter) {
    const auto sum = liquid::runtime::block_on(combined_task());
    ASSERT_TRUE(sum.has_value());
    EXPECT_EQ(*sum, 42);
}

TEST(Runtime, SpawnReportsCompletion) {
    std::promise<liquid::result<void>> completion;
    auto completed = completion.get_future();
    liquid::runtime::spawn(void_task(), [&completion](liquid::result<void> value) {
        completion.set_value(std::move(value));
    });
    EXPECT_TRUE(completed.get().has_value());
}

TEST(Runtime, SpawnUsesSharedContextForStoreIo) {
    const auto root = std::filesystem::temp_directory_path() / "liquid-runtime-spawn-store";
    std::filesystem::remove_all(root);
    std::promise<liquid::result<void>> completion;
    auto completed = completion.get_future();
    liquid::runtime::spawn(store_round_trip(root), [&completion](liquid::result<void> value) {
        completion.set_value(std::move(value));
    });
    const auto result = completed.get();
    std::filesystem::remove_all(root);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().message);
}

TEST(Runtime, SpawnedTasksRunConcurrently) {
    std::atomic<unsigned> active{0};
    std::atomic<unsigned> peak{0};
    std::promise<liquid::result<void>> first_completion;
    std::promise<liquid::result<void>> second_completion;
    auto first = first_completion.get_future();
    auto second = second_completion.get_future();
    liquid::runtime::spawn(concurrent_task(&active, &peak),
                           [&first_completion](liquid::result<void> value) {
                               first_completion.set_value(std::move(value));
                           });
    liquid::runtime::spawn(concurrent_task(&active, &peak),
                           [&second_completion](liquid::result<void> value) {
                               second_completion.set_value(std::move(value));
                           });
    EXPECT_TRUE(first.get().has_value());
    EXPECT_TRUE(second.get().has_value());
    EXPECT_EQ(peak.load(), 2);
}

TEST(Runtime, DefaultContextIsProcessWide) {
    EXPECT_EQ(&liquid::runtime::default_context(), &liquid::runtime::default_context());
}
