#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include "liquid/runtime.hpp"

namespace liquid::vfs_detail {

class request_queue : public std::enable_shared_from_this<request_queue> {
public:
    using work = std::function<runtime::task<int>()>;

    [[nodiscard]] auto submit(work operation) -> int;
    [[nodiscard]] auto average_latency() const -> std::chrono::nanoseconds;

private:
    struct request {
        work operation;
        std::chrono::steady_clock::time_point submitted;
        std::mutex mutex;
        std::condition_variable completed;
        std::optional<int> output;
    };

    static auto drain(std::shared_ptr<request_queue> queue) -> runtime::task<void>;
    void fail_pending();

    mutable std::mutex mutex_;
    std::deque<std::shared_ptr<request>> pending_;
    bool running_{};
    std::chrono::nanoseconds latency_total_{};
    std::uint64_t completed_{};
};

[[nodiscard]] auto measure_request_queue(std::uint64_t count) -> std::chrono::nanoseconds;

} // namespace liquid::vfs_detail
