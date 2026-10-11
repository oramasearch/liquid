#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "liquid/runtime.hpp"
#include "liquid/store.hpp"
#include "temporary_directory.hpp"

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
    auto observation = offload_observation{true, true};
    for (auto attempt = 0; attempt < 256; ++attempt) {
        const auto before_thread = std::this_thread::get_id();
        auto offloaded = co_await liquid::runtime::offload(
            [before_thread] { return std::this_thread::get_id() != before_thread; });
        if (!offloaded)
            co_return std::unexpected(offloaded.error());
        const auto after_executor = co_await boost::capy::this_coro::executor;
        observation.ran_elsewhere = observation.ran_elsewhere && *offloaded;
        observation.resumed_on_executor =
            observation.resumed_on_executor && before_executor == after_executor;
    }
    co_return observation;
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

// Observes an offload's completion post from the pool thread, as liquid#350 needs. After the post
// has published its operation, corosio's scheduler still unlocks its mutex and wakes a runner; a
// block_on context destroyed in that window is touched after its end. The hook keeps the pool
// thread inside its post until block_on returns or a deadline passes, and records whether
// block_on returned while it was there.
struct poster_hook {
    std::mutex mutex;
    std::condition_variable changed;
    std::thread::id pool_thread;
    bool block_on_returned = false;
    bool returned_during_post = false;
    bool poster_left = false;
};

class hook_executor {
public:
    hook_executor(boost::capy::executor_ref inner, std::shared_ptr<poster_hook> hook) noexcept
        : inner_(inner), hook_(std::move(hook)) {}

    bool operator==(const hook_executor& other) const noexcept {
        return inner_ == other.inner_;
    }
    auto context() const noexcept -> boost::capy::execution_context& {
        return inner_.context();
    }
    void on_work_started() const noexcept {
        inner_.on_work_started();
    }
    void on_work_finished() const noexcept {
        inner_.on_work_finished();
    }
    std::coroutine_handle<> dispatch(boost::capy::continuation& continuation) const {
        if (!from_pool())
            return inner_.dispatch(continuation);
        post(continuation);
        return std::noop_coroutine();
    }
    void post(boost::capy::continuation& continuation) const {
        const bool foreign = from_pool();
        inner_.post(continuation);
        if (!foreign)
            return;
        const auto hook = hook_;
        std::unique_lock lock(hook->mutex);
        hook->changed.wait_for(lock, std::chrono::milliseconds(200),
                               [&] { return hook->block_on_returned; });
        hook->returned_during_post = hook->block_on_returned;
        hook->poster_left = true;
        hook->changed.notify_all();
    }

private:
    bool from_pool() const {
        std::lock_guard lock(hook_->mutex);
        return std::this_thread::get_id() == hook_->pool_thread;
    }

    boost::capy::executor_ref inner_;
    std::shared_ptr<poster_hook> hook_;
};

liquid::runtime::task<int> offload_through_hook(std::shared_ptr<poster_hook> hook) {
    const auto inner = co_await boost::capy::this_coro::executor;
    auto state = std::make_shared<liquid::runtime::detail::offload_state<int>>();
    auto work = [hook] {
        std::lock_guard lock(hook->mutex);
        hook->pool_thread = std::this_thread::get_id();
        return 7;
    };
    co_await liquid::runtime::detail::offload_awaiter<decltype(work), int, hook_executor>{
        std::move(work), state, hook_executor{inner, hook}};
    if (!state->output.has_value())
        co_return std::unexpected(
            liquid::error{liquid::errc::internal, "no offload result", "runtime test"});
    co_return std::move(*state->output);
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

TEST(Runtime, BlockOnWaitsForACoroutineSuspendedOutsideItsContext) {
    // While the coroutine waits on the shared pool, block_on's own context has nothing queued.
    // It must keep running until the coroutine finishes, not report "completed without a result"
    // (seen on Linux in lake's sync tests).
    for (int attempt = 0; attempt < 20; ++attempt) {
        const auto value = liquid::runtime::block_on([]() -> liquid::runtime::task<int> {
            auto slept = co_await liquid::runtime::offload([] {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                return 42;
            });
            if (!slept)
                co_return std::unexpected(slept.error());
            co_return *slept;
        }());
        ASSERT_TRUE(value.has_value()) << value.error().message;
        EXPECT_EQ(*value, 42);
    }
}

TEST(Runtime, AnOffloadsCompletionPostReturnsBeforeBlockOnEnds) {
    // liquid#350: the pool thread posts the offload's completion into block_on's context. The
    // awaiting coroutine must not resume, and block_on must not end and destroy that context,
    // until the pool thread has left the post.
    for (int attempt = 0; attempt < 3; ++attempt) {
        auto hook = std::make_shared<poster_hook>();
        const auto value = liquid::runtime::block_on(offload_through_hook(hook));
        {
            std::lock_guard lock(hook->mutex);
            hook->block_on_returned = true;
        }
        hook->changed.notify_all();
        ASSERT_TRUE(value.has_value()) << value.error().message;
        EXPECT_EQ(*value, 7);

        std::unique_lock lock(hook->mutex);
        ASSERT_TRUE(hook->changed.wait_for(lock, std::chrono::seconds(10),
                                           [&] { return hook->poster_left; }));
        EXPECT_FALSE(hook->returned_during_post)
            << "block_on ended while the pool thread was still inside the completion post";
    }
}

TEST(Runtime, ManyContextsOffloadAndEndConcurrently) {
    // The shape of the lake crash: short-lived block_on contexts that end right after an offload
    // completes, many at once.
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int thread = 0; thread < 8; ++thread) {
        threads.emplace_back([&failures] {
            for (int round = 0; round < 150; ++round) {
                const auto value = liquid::runtime::block_on([]() -> liquid::runtime::task<int> {
                    auto offloaded = co_await liquid::runtime::offload([] { return 3; });
                    if (!offloaded)
                        co_return std::unexpected(offloaded.error());
                    co_return *offloaded;
                }());
                if (!value || *value != 3)
                    failures.fetch_add(1);
            }
        });
    }
    for (auto& thread : threads)
        thread.join();
    EXPECT_EQ(failures.load(), 0);
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
    const auto root = liquid::test::fixture_path("liquid-runtime-spawn-store");
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
