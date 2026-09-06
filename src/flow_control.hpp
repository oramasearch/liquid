#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "liquid/error.hpp"
#include "liquid/flow.hpp"
#include "liquid/object.hpp"

namespace liquid::flow_detail {

[[nodiscard]] auto validate(const flow_config& config) -> result<void>;
[[nodiscard]] auto node_key(const node_id& node) -> std::string;

class controller {
public:
    struct bucket {
        token_bucket_config config;
        double tokens{};
        std::chrono::steady_clock::time_point refilled_at;
        std::uint64_t rejected{};
    };

    explicit controller(flow_config config);

    [[nodiscard]] auto admit(std::string actor,
                             std::string document,
                             std::uint64_t update_bytes,
                             std::uint64_t unacknowledged = 0) -> result<void>;
    [[nodiscard]] auto status() const -> std::vector<flow_bucket_status>;
    [[nodiscard]] const flow_config& config() const noexcept;
    auto set_pause(flow_pause_reason reason) -> void;

private:
    [[nodiscard]] auto actor_bucket(const std::string& actor) -> bucket&;
    [[nodiscard]] auto document_bucket(const std::string& document) -> bucket&;
    static auto refill(bucket& value, std::chrono::steady_clock::time_point now) -> void;

    flow_config config_;
    mutable std::mutex mutex_;
    std::map<std::string, bucket> actors_;
    std::map<std::string, bucket> documents_;
    bucket repository_;
    flow_pause_reason pause_reason_{flow_pause_reason::none};
};

[[nodiscard]] auto shared_controller(std::string repository_key, flow_config config)
    -> std::shared_ptr<controller>;

} // namespace liquid::flow_detail
