#pragma once

#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include <boost/capy.hpp>

#include "liquid/error.hpp"
#include "liquid/export.hpp"

namespace liquid::runtime {

template <class T = void> using task = boost::capy::task<result<T>>;

using boost::capy::delay;
using boost::capy::io_result;
using boost::capy::timeout;
using boost::capy::when_all;

namespace detail {

inline error exception_error(std::exception_ptr exception, std::string context) {
    try {
        if (exception)
            std::rethrow_exception(exception);
    } catch (const std::exception& caught) {
        return {errc::internal, caught.what(), std::move(context)};
    } catch (...) {
        return {errc::internal, "unknown exception", std::move(context)};
    }
    return {errc::internal, "coroutine completed without a result", std::move(context)};
}

template <class T>
boost::capy::task<> complete_block_on(task<T> coroutine,
                                      std::shared_ptr<std::optional<result<T>>> output) {
    try {
        output->emplace(co_await std::move(coroutine));
    } catch (...) {
        output->emplace(
            std::unexpected(exception_error(std::current_exception(), "runtime::block_on")));
    }
    co_return;
}

template <class T> struct offload_state {
    std::optional<result<T>> output;
    boost::capy::continuation completion;
};

template <class T> task<void> wake_offload(std::shared_ptr<offload_state<T>> state) {
    const auto executor = co_await boost::capy::this_coro::executor;
    executor.post(state->completion);
    co_return {};
}

template <class Callable, class T, class Executor>
boost::capy::task<>
run_blocking(Callable callable, std::shared_ptr<offload_state<T>> state, Executor executor) {
    try {
        if constexpr (std::is_void_v<T>) {
            std::invoke(callable);
            state->output.emplace();
        } else {
            state->output.emplace(std::invoke(callable));
        }
    } catch (...) {
        state->output.emplace(
            std::unexpected(exception_error(std::current_exception(), "runtime::offload")));
    }
    boost::capy::run_async(executor)(wake_offload(std::move(state)));
    co_return;
}

LIQUID_EXPORT void run_on_pool(boost::capy::task<> coroutine);

template <class Callable, class T, class Executor> class offload_awaiter {
public:
    offload_awaiter(Callable callable, std::shared_ptr<offload_state<T>> state, Executor executor)
        : callable_(std::move(callable)), state_(std::move(state)), executor_(std::move(executor)) {
    }

    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> handle,
                                          const boost::capy::io_env*) {
        state_->completion.h = handle;
        run_on_pool(run_blocking<Callable, T>(std::move(callable_), state_, executor_));
        return std::noop_coroutine();
    }

    void await_resume() const noexcept {}

private:
    Callable callable_;
    std::shared_ptr<offload_state<T>> state_;
    Executor executor_;
};

} // namespace detail

class LIQUID_EXPORT context {
public:
    context();
    ~context();
    context(context&&) noexcept;
    context& operator=(context&&) noexcept;
    context(const context&) = delete;
    context& operator=(const context&) = delete;

    [[nodiscard]] auto executor() const -> boost::capy::any_executor;

private:
    struct impl;
    std::unique_ptr<impl> impl_;

    context(unsigned runners, bool keep_alive);
    void run(boost::capy::task<> coroutine);

    template <class T> friend result<T> block_on(task<T> coroutine);
    friend context& default_context();
};

[[nodiscard]] LIQUID_EXPORT auto default_context() -> context&;

template <class T> result<T> block_on(task<T> coroutine) {
    try {
        auto output = std::make_shared<std::optional<result<T>>>();
        context runtime;
        runtime.run(detail::complete_block_on(std::move(coroutine), output));
        if (!output->has_value()) {
            return std::unexpected(
                error{errc::internal, "coroutine completed without a result", "runtime::block_on"});
        }
        return std::move(**output);
    } catch (...) {
        return std::unexpected(
            detail::exception_error(std::current_exception(), "runtime::block_on"));
    }
}

LIQUID_EXPORT void spawn(task<void> coroutine, std::function<void(result<void>)> on_done);

template <class Callable> task<std::invoke_result_t<Callable>> offload(Callable callable) {
    using output_type = std::invoke_result_t<Callable>;
    try {
        auto state = std::make_shared<detail::offload_state<output_type>>();
        const auto executor = co_await boost::capy::this_coro::executor;
        co_await detail::offload_awaiter<Callable, output_type, decltype(executor)>{
            std::move(callable), state, executor};
        if (!state->output.has_value()) {
            co_return std::unexpected(error{errc::internal,
                                            "blocking operation completed without a result",
                                            "runtime::offload"});
        }
        co_return std::move(*state->output);
    } catch (...) {
        co_return std::unexpected(
            detail::exception_error(std::current_exception(), "runtime::offload"));
    }
}

template <class T> boost::capy::io_task<result<T>> as_io(task<T> coroutine) {
    try {
        co_return io_result<result<T>>{{}, co_await std::move(coroutine)};
    } catch (...) {
        co_return io_result<result<T>>{
            {},
            std::unexpected(detail::exception_error(std::current_exception(), "runtime::as_io"))};
    }
}

} // namespace liquid::runtime
