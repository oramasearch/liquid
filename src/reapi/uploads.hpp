// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/reapi.hpp"

namespace liquid::reapi {

[[nodiscard]] auto validate_upload_limits(const reapi_upload_limits& limits) -> result<void>;

// Shared by both listeners. A Write owns its entry until its response is delivered. Dropping
// that ownership on an error or coroutine cancellation releases the entry and its byte charge.
class partial_uploads {
public:
    using clock = std::chrono::steady_clock;
    explicit partial_uploads(reapi_upload_limits limits) : limits_(limits) {}

    class write {
    public:
        ~write();
        write(const write&) = delete;
        auto operator=(const write&) -> write& = delete;
        [[nodiscard]] auto append(std::int64_t offset,
                                  std::span<const std::byte> bytes,
                                  clock::time_point now = clock::now()) -> result<std::int64_t>;
        [[nodiscard]] auto finish() -> result<std::vector<std::byte>>;
        void retain(clock::time_point now = clock::now());

    private:
        friend class partial_uploads;
        write(partial_uploads& owner, std::string name, std::uint64_t generation)
            : owner_(owner), name_(std::move(name)), generation_(generation) {}
        partial_uploads& owner_;
        std::string name_;
        std::uint64_t generation_;
        bool retained_{};
    };

    [[nodiscard]] auto
    begin(std::string_view name, std::int64_t expected_size, clock::time_point now = clock::now())
        -> result<std::unique_ptr<write>>;
    [[nodiscard]] auto committed(std::string_view name, clock::time_point now = clock::now())
        -> result<std::int64_t>;
    void expire(clock::time_point now = clock::now());

private:
    struct upload {
        std::vector<std::byte> data;
        std::size_t expected_size{};
        std::size_t capacity{};
        std::size_t committed{};
        clock::time_point touched;
        std::uint64_t generation{};
        bool active{};
        bool finished{};
    };
    using uploads = std::map<std::string, upload, std::less<>>;
    void expire_locked(clock::time_point now);
    void erase(uploads::iterator entry);
    reapi_upload_limits limits_;
    std::mutex mutex_;
    uploads uploads_;
    std::size_t buffered_{};
    std::uint64_t generation_{};
};

} // namespace liquid::reapi
