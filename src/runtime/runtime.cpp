#include "liquid/runtime.hpp"

#include <algorithm>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/ex/thread_pool.hpp>

#include "net.hpp"

namespace liquid::runtime {
namespace {

unsigned runtime_threads() noexcept {
    return std::max(4u, std::thread::hardware_concurrency());
}

unsigned shared_runtime_threads() noexcept {
    return std::max(2u, std::thread::hardware_concurrency() / 2);
}

net::io_context_options runtime_options() noexcept {
    net::io_context_options options;
    options.thread_pool_size = 4;
    return options;
}

boost::capy::thread_pool& shared_pool() {
    static boost::capy::thread_pool pool(0, "liquid-");
    return pool;
}

boost::capy::task<> complete_spawn(task<void> coroutine,
                                   std::function<void(result<void>)> on_done) {
    result<void> output;
    try {
        output = co_await std::move(coroutine);
    } catch (...) {
        output =
            std::unexpected(detail::exception_error(std::current_exception(), "runtime::spawn"));
    }
    try {
        if (on_done)
            on_done(std::move(output));
    } catch (...) {
    }
    co_return;
}

} // namespace

struct context::impl {
    impl(unsigned count, bool keep_alive, unsigned concurrency_hint)
        : io(runtime_options(), concurrency_hint), runners(count) {
        if (!keep_alive)
            return;
        guard.emplace(boost::capy::make_work_guard(io.get_executor()));
        try {
            for (unsigned index = 0; index < runners; ++index)
                threads.emplace_back([this] { io.run(); });
        } catch (...) {
            io.stop();
            for (auto& thread : threads)
                thread.join();
            throw;
        }
    }

    ~impl() {
        io.stop();
        for (auto& thread : threads)
            thread.join();
        guard.reset();
    }

    net::io_context io;
    std::optional<boost::capy::work_guard<net::io_context::executor_type>> guard;
    std::vector<std::thread> threads;
    unsigned runners;
};

context::context() : context(runtime_threads(), false) {}
context::context(unsigned runners, bool keep_alive) : context(runners, keep_alive, runners) {}
context::context(unsigned runners, bool keep_alive, unsigned concurrency_hint)
    : impl_(std::make_unique<impl>(runners, keep_alive, concurrency_hint)) {}
context::~context() = default;
context::context(context&&) noexcept = default;
context& context::operator=(context&&) noexcept = default;

auto context::executor() const -> boost::capy::any_executor {
    return impl_->io.get_executor();
}

auto default_context() -> context& {
    static context shared{shared_runtime_threads(), true};
    return shared;
}

void context::run(boost::capy::task<> coroutine) {
    boost::capy::run_async(impl_->io.get_executor())(std::move(coroutine));

    std::vector<std::thread> runners;
    runners.reserve(impl_->runners - 1);
    try {
        for (unsigned index = 1; index < impl_->runners; ++index) {
            runners.emplace_back([this] { impl_->io.run(); });
        }
    } catch (...) {
        impl_->io.run();
        for (auto& runner : runners)
            runner.join();
        throw;
    }
    impl_->io.run();
    for (auto& runner : runners)
        runner.join();
}

namespace detail {

void run_on_pool(boost::capy::task<> coroutine) {
    boost::capy::run_async(shared_pool().get_executor())(std::move(coroutine));
}

} // namespace detail

void spawn(task<void> coroutine, std::function<void(result<void>)> on_done) {
    boost::capy::run_async(default_context().executor())(
        complete_spawn(std::move(coroutine), std::move(on_done)));
}

void spawn_serial(task<void> coroutine, std::function<void(result<void>)> on_done) {
    static context serial{1, true, 2};
    boost::capy::run_async(serial.executor())(
        complete_spawn(std::move(coroutine), std::move(on_done)));
}

} // namespace liquid::runtime
