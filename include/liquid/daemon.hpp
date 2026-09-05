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
#include "liquid/error.hpp"
#include "liquid/export.hpp"
#include "liquid/graph.hpp"
#include "liquid/runtime.hpp"

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
};

struct LIQUID_EXPORT daemon_target_status {
    std::string name;
    journal_cursor cursor;
    std::uint64_t behind{};
    std::uint64_t deliveries{};
    std::optional<std::string> last_error;
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
};

struct LIQUID_EXPORT daemon_options {
    std::chrono::milliseconds idle_timeout{std::chrono::minutes{5}};
    std::uint64_t graph_memory{256ULL * 1024ULL * 1024ULL};
    std::uint32_t graph_validation_rate{20};
    std::chrono::milliseconds graph_validation_interval{std::chrono::seconds{1}};
    query_registry graph_queries{};
    capture_options capture{};
    std::vector<std::shared_ptr<daemon_target>> targets;
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
    [[nodiscard]] runtime::task<build_handle> build(action_input input);
    [[nodiscard]] runtime::task<void> shutdown();
    [[nodiscard]] runtime::task<void> wait();

private:
    struct impl;
    explicit daemon(std::unique_ptr<impl> implementation) noexcept;
    std::unique_ptr<impl> impl_;
};

} // namespace liquid
