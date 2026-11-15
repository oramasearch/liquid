// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "reapi/conversion.hpp"
#include "reapi/messages.hpp"
#include "workers/protocol.hpp"

namespace liquid::workers {

class scheduler {
public:
    explicit scheduler(std::chrono::milliseconds lease_duration);

    [[nodiscard]] auto submit(reapi::Digest action_digest,
                              reapi::DigestFunctionValue digest_function,
                              std::vector<std::byte> action,
                              std::vector<std::byte> command,
                              reapi::DecodedAction decoded,
                              digest native_action,
                              execution_schedule schedule) -> result<reapi::Operation>;
    [[nodiscard]] auto lease(const worker_request& request) -> result<lease_response>;
    [[nodiscard]] auto heartbeat(const heartbeat_request& request) -> acknowledgement;
    [[nodiscard]] auto complete(const complete_request& request, reapi::ActionResult result)
        -> acknowledgement;
    [[nodiscard]] auto fail(const complete_request& request) -> acknowledgement;
    [[nodiscard]] auto operation(std::string_view name) -> result<reapi::Operation>;
    [[nodiscard]] auto action_digest(std::string_view name) -> result<reapi::Digest>;
    [[nodiscard]] auto cancel(std::string_view name) -> result<void>;
    [[nodiscard]] auto status() -> scheduler_status;
    [[nodiscard]] auto ready() -> bool;

private:
    enum class stage : std::uint8_t { queued, leased, completed, failed, cancelled };

    struct operation_entry {
        std::string name;
        reapi::Digest action_digest;
        reapi::DigestFunctionValue digest_function{reapi::DigestFunctionValue::blake3};
        std::vector<std::byte> action;
        std::vector<std::byte> command;
        reapi::DecodedAction decoded;
        digest native_action;
        execution_schedule schedule;
        stage state{stage::queued};
        std::string worker;
        std::uint64_t token{};
        std::uint64_t retry{};
        std::chrono::steady_clock::time_point lease_expires{};
        std::optional<reapi::ActionResult> result;
        std::string failure;
    };

    [[nodiscard]] auto operation_message(const operation_entry& entry) const
        -> result<reapi::Operation>;
    void expire_locked(std::chrono::steady_clock::time_point now);
    [[nodiscard]] static auto fits(action_size_class worker, action_size_class action) -> bool;

    std::chrono::milliseconds lease_duration_;
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<operation_entry>> by_action_;
    std::map<std::string, std::shared_ptr<operation_entry>> by_name_;
    std::map<std::string, std::chrono::steady_clock::time_point> workers_;
    std::uint64_t next_operation_{1};
    std::uint64_t next_token_{1};
    std::uint64_t requeued_{};
};

} // namespace liquid::workers
