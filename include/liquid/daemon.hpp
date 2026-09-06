#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "liquid/build.hpp"
#include "liquid/drip.hpp"
#include "liquid/edit.hpp"
#include "liquid/error.hpp"
#include "liquid/export.hpp"
#include "liquid/flow.hpp"
#include "liquid/graph.hpp"
#include "liquid/runtime.hpp"
#include "liquid/session.hpp"

namespace liquid {

struct LIQUID_EXPORT daemon_delivery {
    drip value;
    journal_cursor cursor;
};

class LIQUID_EXPORT daemon_target {
public:
    virtual ~daemon_target() = default;

    [[nodiscard]] virtual auto name() const -> std::string = 0;
    [[nodiscard]] virtual runtime::task<void> deliver(daemon_delivery delivery) = 0;
    [[nodiscard]] virtual runtime::task<void> deliver_presence(liquid::presence value) {
        static_cast<void>(value);
        co_return result<void>{};
    }
    [[nodiscard]] virtual runtime::task<void> deliver_locks(std::vector<exclusive_lock> values) {
        static_cast<void>(values);
        co_return result<void>{};
    }
    [[nodiscard]] virtual auto detail() const -> std::optional<std::string> {
        return std::nullopt;
    }
};

struct LIQUID_EXPORT daemon_target_status {
    std::string name;
    journal_cursor cursor;
    std::uint64_t behind{};
    std::uint64_t deliveries{};
    std::optional<std::string> last_error;
    std::optional<std::string> detail;
};

struct LIQUID_EXPORT daemon_status {
    bool running{};
    std::uint64_t process_id{};
    std::string endpoint;
    std::optional<drip_id> last_drip;
    std::vector<std::string> invalidated_paths;
    std::vector<daemon_target_status> targets;
    capture_status capture;
    std::uint64_t graph_keys{};
    std::uint64_t graph_values{};
    std::uint64_t graph_memory_bytes{};
    std::uint64_t graph_memory_ceiling{};
    std::uint64_t graph_validation_backlog{};
    std::optional<graph_validation_record> last_graph_divergence;
    flow_status flow;
};

struct LIQUID_EXPORT daemon_options {
    std::chrono::milliseconds idle_timeout{std::chrono::minutes{5}};
    std::uint64_t graph_memory{256ULL * 1024ULL * 1024ULL};
    std::uint32_t graph_validation_rate{20};
    std::chrono::milliseconds graph_validation_interval{std::chrono::seconds{1}};
    query_registry graph_queries{};
    capture_options capture{};
    std::vector<std::shared_ptr<daemon_target>> targets;
    std::string drive_bearer_token;
    lock_authorizer authorize_lock;
};

class LIQUID_EXPORT daemon {
public:
    daemon() noexcept;
    ~daemon();
    daemon(daemon&&) noexcept;
    daemon& operator=(daemon&&) noexcept;
    daemon(const daemon&) = delete;
    daemon& operator=(const daemon&) = delete;

    [[nodiscard]] static runtime::task<daemon> start(std::filesystem::path checkout,
                                                     daemon_options options = {});
    [[nodiscard]] static runtime::task<daemon> attach(std::filesystem::path checkout);
    [[nodiscard]] runtime::task<daemon_status> status();
    [[nodiscard]] runtime::task<void> pause_capture();
    [[nodiscard]] runtime::task<void> resume_capture();
    [[nodiscard]] runtime::task<std::vector<build_plan_action>>
    build_plan(build_target target = {});
    [[nodiscard]] runtime::task<build_handle> build(build_target target);
    [[nodiscard]] runtime::task<build_handle> build(action_input input);
    [[nodiscard]] runtime::task<edit_preview> preview_edit(edit_request request,
                                                           edit_attribution attribution);
    [[nodiscard]] runtime::task<edit_result> apply_edit(edit_preview preview);
    [[nodiscard]] runtime::task<void> report_consumer_lag(std::string name, std::uint64_t behind);
    [[nodiscard]] runtime::task<void> report_presence(liquid::presence value);
    [[nodiscard]] runtime::task<void> lock(std::string path,
                                           std::string reason = "exclusive checkout");
    [[nodiscard]] runtime::task<void> unlock(std::string path);
    [[nodiscard]] runtime::task<void> shutdown();
    [[nodiscard]] runtime::task<void> wait();

private:
    struct impl;
    explicit daemon(std::unique_ptr<impl> implementation) noexcept;
    std::unique_ptr<impl> impl_;
};

} // namespace liquid
