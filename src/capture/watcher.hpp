#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

#include "liquid/error.hpp"

namespace liquid {
class capture;
}

namespace liquid::capture_detail {

enum class watch_event_kind { write, remove, reconcile };

struct watch_event {
    watch_event_kind kind{watch_event_kind::write};
    std::filesystem::path path;
    std::uint64_t token{0};
};

using watch_callback = std::function<void(watch_event)>;

class watcher {
public:
    virtual ~watcher() = default;
    virtual auto start(watch_callback callback) -> result<void> = 0;
    virtual void stop() noexcept = 0;
    virtual void inject_overflow_for_test() = 0;
};

[[nodiscard]] auto make_watcher(const std::filesystem::path& workspace,
                                std::chrono::milliseconds latency) -> std::unique_ptr<watcher>;

void inject_overflow_for_test(capture& handle);
[[nodiscard]] auto last_journal_append_duration_for_test(const capture& handle)
    -> std::chrono::nanoseconds;

} // namespace liquid::capture_detail
