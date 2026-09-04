#include "request_queue.hpp"

#include <cerrno>

namespace liquid::vfs_detail {
namespace {

runtime::task<int> successful_request() {
    co_return 0;
}

} // namespace

auto request_queue::submit(work operation) -> int {
    auto item = std::make_shared<request>();
    item->operation = std::move(operation);
    item->submitted = std::chrono::steady_clock::now();
    auto start_drain = false;
    {
        auto lock = std::scoped_lock{mutex_};
        pending_.push_back(item);
        if (!running_) {
            running_ = true;
            start_drain = true;
        }
    }
    if (start_drain) {
        auto self = shared_from_this();
        runtime::spawn(drain(self), [self](result<void> outcome) {
            if (!outcome)
                self->fail_pending();
        });
    }
    auto lock = std::unique_lock{item->mutex};
    item->completed.wait(lock, [&] { return item->output.has_value(); });
    return *item->output;
}

runtime::task<void> request_queue::drain(std::shared_ptr<request_queue> queue) {
    for (;;) {
        auto item = std::shared_ptr<request>{};
        {
            auto lock = std::scoped_lock{queue->mutex_};
            if (queue->pending_.empty()) {
                queue->running_ = false;
                co_return result<void>{};
            }
            item = std::move(queue->pending_.front());
            queue->pending_.pop_front();
        }
        auto outcome = result<int>{};
        try {
            outcome = co_await item->operation();
        } catch (...) {
            outcome = std::unexpected(
                runtime::detail::exception_error(std::current_exception(), "VFS request queue"));
        }
        const auto value = outcome ? *outcome : -EIO;
        const auto latency = std::chrono::steady_clock::now() - item->submitted;
        {
            auto lock = std::scoped_lock{queue->mutex_};
            queue->latency_total_ += std::chrono::duration_cast<std::chrono::nanoseconds>(latency);
            ++queue->completed_;
        }
        {
            auto lock = std::scoped_lock{item->mutex};
            item->output = value;
        }
        item->completed.notify_one();
    }
}

void request_queue::fail_pending() {
    auto pending = std::deque<std::shared_ptr<request>>{};
    {
        auto lock = std::scoped_lock{mutex_};
        pending.swap(pending_);
        running_ = false;
    }
    for (const auto& item : pending) {
        {
            auto lock = std::scoped_lock{item->mutex};
            item->output = -EIO;
        }
        item->completed.notify_one();
    }
}

auto request_queue::average_latency() const -> std::chrono::nanoseconds {
    auto lock = std::scoped_lock{mutex_};
    return completed_ == 0 ? std::chrono::nanoseconds{}
                           : latency_total_ / static_cast<std::int64_t>(completed_);
}

auto measure_request_queue(std::uint64_t count) -> std::chrono::nanoseconds {
    auto queue = std::make_shared<request_queue>();
    for (auto index = std::uint64_t{}; index < count; ++index)
        static_cast<void>(queue->submit(successful_request));
    return queue->average_latency();
}

} // namespace liquid::vfs_detail
