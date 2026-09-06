#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "liquid/daemon.hpp"

namespace liquid::daemon_detail {

class fanout {
public:
    explicit fanout(std::filesystem::path state_path,
                    std::vector<std::shared_ptr<daemon_target>> targets);

    [[nodiscard]] auto load() -> result<void>;
    [[nodiscard]] auto observe(const journal_cursor& cursor) -> result<void>;
    [[nodiscard]] runtime::task<void> deliver(daemon_delivery delivery);
    [[nodiscard]] runtime::task<void> deliver_presence(liquid::presence value);
    [[nodiscard]] runtime::task<void> deliver_locks(std::vector<exclusive_lock> values);
    [[nodiscard]] auto target_status() const -> std::vector<daemon_target_status>;
    [[nodiscard]] auto replay_cursor() const -> journal_cursor;
    [[nodiscard]] auto last_drip() const -> std::optional<drip_id>;
    [[nodiscard]] auto invalidated_paths() const -> std::vector<std::string>;

private:
    struct target_state {
        journal_cursor cursor;
        std::uint64_t deliveries{};
        std::optional<std::string> last_error;
    };

    [[nodiscard]] auto persist() const -> result<void>;

    std::filesystem::path state_path_;
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<daemon_target>> targets_;
    std::map<std::string, target_state> state_;
    journal_cursor observed_cursor_;
    std::uint64_t observed_deliveries_{};
    journal_cursor last_cursor_;
    std::optional<drip_id> last_drip_;
    std::vector<std::string> invalidated_paths_;
};

[[nodiscard]] auto cursor_offset(const journal_cursor& cursor) -> result<std::uint64_t>;

} // namespace liquid::daemon_detail
