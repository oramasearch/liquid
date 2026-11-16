#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/export.hpp"
#include "liquid/object.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

class capture;
namespace capture_detail {
LIQUID_EXPORT void inject_overflow_for_test(capture& handle);
[[nodiscard]] LIQUID_EXPORT auto last_journal_append_duration_for_test(const capture& handle)
    -> std::chrono::nanoseconds;
[[nodiscard]] LIQUID_EXPORT auto last_scan_duration_for_test(const capture& handle)
    -> std::chrono::nanoseconds;
} // namespace capture_detail

struct LIQUID_EXPORT drip_id {
    digest value;
};

struct LIQUID_EXPORT path_op {
    std::string path;
};

struct LIQUID_EXPORT scan_block {
    errc code{errc::unauthorized};
    std::string path;
    std::string detector;
    std::uint64_t line{};
};

struct LIQUID_EXPORT journal_cursor {
    std::string token;
};

struct LIQUID_EXPORT write_base {
    change_id change;
    std::optional<journal_cursor> cursor;
};

enum class stale_revalidation : std::uint8_t {
    disjoint,
    forced,
};

struct LIQUID_EXPORT drip {
    drip_id id;
    change_id base;
    std::vector<path_op> ops;
    actor by;
    std::vector<std::byte> signature;
    std::chrono::sys_seconds at;
    std::optional<write_base> reasoned_from;
    std::optional<stale_revalidation> revalidated;
    std::optional<digest> delegation;
    // For a delegated drip, the principal whose key signed it; `by` is then the agent
    // (liquid#538).
    std::optional<actor> principal;
    std::optional<digest> scan_attestation;
    std::optional<scan_block> block;
    bool redacted{};
};

struct LIQUID_EXPORT scan_allow_rule {
    std::string path;
    std::vector<std::string> detectors;
};

struct LIQUID_EXPORT scan_configuration {
    std::vector<scan_allow_rule> allow;
    std::string ignore_lines{"liquid:allow-secret"};
};

struct LIQUID_EXPORT capture_options {
    std::vector<std::string> exclude;
    std::chrono::milliseconds coalesce_window{200};
    std::chrono::milliseconds reconcile_interval{2000};
    bool attribution{false};
    bool scan{true};
    scan_configuration scan_config{};
};

struct LIQUID_EXPORT capture_status {
    enum class state : std::uint8_t { running, paused } state{state::running};
    std::uint64_t pending_drips{0};
    std::uint64_t overflow_count{0};
    std::uint64_t recrawl_count{0};
    std::uint64_t blocked_drips{0};
    std::optional<scan_block> last_block;
    journal_cursor last_cursor;
    // False when the platform watcher could not be armed and capture is running
    // on periodic reconciliation alone. Changes are still observed; they are
    // observed on the reconcile interval rather than as they happen.
    bool watcher_armed{true};
    std::string watcher_diagnostic;
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
    using cursor_callback = std::function<void(const drip&, const journal_cursor&)>;

    ~capture();
    capture(capture&&) noexcept;
    capture& operator=(capture&&) noexcept;
    capture(const capture&) = delete;
    capture& operator=(const capture&) = delete;

    [[nodiscard]] static runtime::task<capture> open(std::filesystem::path workspace,
                                                     capture_options options = {});
    [[nodiscard]] runtime::task<void> close();
    [[nodiscard]] result<subscription> subscribe(std::function<void(const drip&)> callback);
    [[nodiscard]] result<subscription> subscribe_from(journal_cursor cursor,
                                                      cursor_callback callback);
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
    friend auto capture_detail::last_scan_duration_for_test(const capture& handle)
        -> std::chrono::nanoseconds;
};

} // namespace liquid
