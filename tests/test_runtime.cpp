#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <thread>
#include <utility>

#include "liquid/runtime.hpp"

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

}  // namespace

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
