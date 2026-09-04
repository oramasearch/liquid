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
    boost::capy::async_event done;
};

template <class Callable, class T>
boost::capy::task<> run_blocking(Callable callable, std::shared_ptr<offload_state<T>> state) {
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
    state->done.set();
    co_return;
}

LIQUID_EXPORT void run_on_pool(boost::capy::task<> coroutine);

}  // namespace detail

class LIQUID_EXPORT context {
public:
    context();
    ~context();
    context(context&&) noexcept;
    context& operator=(context&&) noexcept;
    context(const context&) = delete;
    context& operator=(const context&) = delete;

private:
    struct impl;
    std::unique_ptr<impl> impl_;

    void run(boost::capy::task<> coroutine);

    template <class T> friend result<T> block_on(task<T> coroutine);
};

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
        detail::run_on_pool(
            detail::run_blocking<Callable, output_type>(std::move(callable), state));
        auto [event_error] = co_await state->done.wait();
        if (event_error) {
            co_return std::unexpected(
                error{errc::internal, event_error.message(), "runtime::offload"});
        }
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

}  // namespace liquid::runtime
