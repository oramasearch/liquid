#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/export.hpp"
#include "liquid/object.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

class capture;
namespace capture_detail {
void inject_overflow_for_test(capture& handle);
[[nodiscard]] auto last_journal_append_duration_for_test(const capture& handle)
    -> std::chrono::nanoseconds;
} // namespace capture_detail

struct LIQUID_EXPORT drip_id {
    digest value;
};

struct LIQUID_EXPORT path_op {
    std::string path;
};

struct LIQUID_EXPORT drip {
    drip_id id;
    change_id base;
    std::vector<path_op> ops;
    actor by;
    std::vector<std::byte> signature;
    std::chrono::sys_seconds at;
};

struct LIQUID_EXPORT capture_options {
    std::vector<std::string> exclude;
    std::chrono::milliseconds coalesce_window{200};
    bool attribution{false};
};

struct LIQUID_EXPORT journal_cursor {
    std::string token;
};

struct LIQUID_EXPORT capture_status {
    enum class state : std::uint8_t { running, paused } state{state::running};
    std::uint64_t pending_drips{0};
    std::uint64_t overflow_count{0};
    std::uint64_t recrawl_count{0};
    journal_cursor last_cursor;
};

class LIQUID_EXPORT subscription {
public:
    subscription() noexcept;
    ~subscription();
    subscription(subscription&&) noexcept;
    subscription& operator=(subscription&&) noexcept;
    subscription(const subscription&) = delete;
    subscription& operator=(const subscription&) = delete;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
    friend class capture;
};

class LIQUID_EXPORT capture {
public:
    ~capture();
    capture(capture&&) noexcept;
    capture& operator=(capture&&) noexcept;
    capture(const capture&) = delete;
    capture& operator=(const capture&) = delete;

    [[nodiscard]] static runtime::task<capture> open(std::filesystem::path workspace,
                                                     capture_options options = {});
    [[nodiscard]] runtime::task<void> close();
    [[nodiscard]] result<subscription> subscribe(std::function<void(const drip&)> callback);
    [[nodiscard]] runtime::task<void> pause();
    [[nodiscard]] runtime::task<void> resume();
    [[nodiscard]] result<capture_status> status();

private:
    struct impl;
    explicit capture(std::unique_ptr<impl> implementation) noexcept;
    std::unique_ptr<impl> impl_;
    friend class subscription;
    friend void capture_detail::inject_overflow_for_test(capture& handle);
    friend auto capture_detail::last_journal_append_duration_for_test(const capture& handle)
        -> std::chrono::nanoseconds;
};

} // namespace liquid
