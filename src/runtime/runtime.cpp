#include "liquid/runtime.hpp"

#include <algorithm>
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

}  // namespace

struct context::impl {
    impl() : io(runtime_options(), runtime_threads()) {}

    net::io_context io;
};

context::context() : impl_(std::make_unique<impl>()) {}
context::~context() = default;
context::context(context&&) noexcept = default;
context& context::operator=(context&&) noexcept = default;

void context::run(boost::capy::task<> coroutine) {
    const auto count = runtime_threads();
    boost::capy::run_async(impl_->io.get_executor())(std::move(coroutine));

    std::vector<std::thread> runners;
    runners.reserve(count - 1);
    try {
        for (unsigned index = 1; index < count; ++index) {
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

}  // namespace detail

void spawn(task<void> coroutine, std::function<void(result<void>)> on_done) {
    detail::run_on_pool(complete_spawn(std::move(coroutine), std::move(on_done)));
}

}  // namespace liquid::runtime
