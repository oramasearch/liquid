#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "liquid/build.hpp"
#include "reapi/messages.hpp"

namespace liquid::workers {

inline constexpr auto submit_method = "/liquid.scheduler.v1.Scheduler/Submit";
inline constexpr auto lease_method = "/liquid.scheduler.v1.Scheduler/Lease";
inline constexpr auto heartbeat_method = "/liquid.scheduler.v1.Scheduler/Heartbeat";
inline constexpr auto complete_method = "/liquid.scheduler.v1.Scheduler/Complete";
inline constexpr auto read_object_method = "/liquid.scheduler.v1.Scheduler/ReadObject";
inline constexpr auto status_method = "/liquid.scheduler.v1.Scheduler/Status";

struct submit_request {
    std::string instance;
    reapi::Digest action;
    execution_schedule schedule;
    digest native_action;
    bool remote_eligible{};
};

struct worker_request {
    std::string instance;
    std::string worker;
    platform platform;
    action_size_class size_class{action_size_class::long_action};
};

struct lease_response {
    bool assigned{};
    std::string operation;
    std::uint64_t token{};
    std::uint64_t retry{};
    reapi::Digest action_digest;
    reapi::DigestFunctionValue digest_function{reapi::DigestFunctionValue::blake3};
    std::vector<std::byte> action;
    std::vector<std::byte> command;
    execution_schedule schedule;
    digest native_action;
    bool remote_eligible{};
};

struct heartbeat_request {
    std::string operation;
    std::string worker;
    std::uint64_t token{};
};

struct complete_request {
    std::string operation;
    std::string worker;
    std::uint64_t token{};
    std::vector<std::byte> result;
    std::string failure;
};

struct acknowledgement {
    bool accepted{};
    std::string message;
};

struct read_object_request {
    std::string digest;
};

struct read_object_response {
    std::vector<std::byte> contents;
};

struct scheduler_status {
    std::uint64_t queued{};
    std::uint64_t in_flight{};
    std::uint64_t completed{};
    std::uint64_t failed{};
    std::uint64_t requeued{};
    std::uint64_t workers{};
};

[[nodiscard]] auto encode(const submit_request& value) -> result<std::vector<std::byte>>;
[[nodiscard]] auto encode(const worker_request& value) -> result<std::vector<std::byte>>;
[[nodiscard]] auto encode(const lease_response& value) -> result<std::vector<std::byte>>;
[[nodiscard]] auto encode(const heartbeat_request& value) -> result<std::vector<std::byte>>;
[[nodiscard]] auto encode(const complete_request& value) -> result<std::vector<std::byte>>;
[[nodiscard]] auto encode(const acknowledgement& value) -> result<std::vector<std::byte>>;
[[nodiscard]] auto encode(const read_object_request& value) -> result<std::vector<std::byte>>;
[[nodiscard]] auto encode(const read_object_response& value) -> result<std::vector<std::byte>>;
[[nodiscard]] auto encode(const scheduler_status& value) -> result<std::vector<std::byte>>;

template <typename Message>
[[nodiscard]] auto decode(std::span<const std::byte> input) -> result<Message>;

extern template auto decode<submit_request>(std::span<const std::byte>) -> result<submit_request>;
extern template auto decode<worker_request>(std::span<const std::byte>) -> result<worker_request>;
extern template auto decode<lease_response>(std::span<const std::byte>) -> result<lease_response>;
extern template auto decode<heartbeat_request>(std::span<const std::byte>)
    -> result<heartbeat_request>;
extern template auto decode<complete_request>(std::span<const std::byte>)
    -> result<complete_request>;
extern template auto decode<acknowledgement>(std::span<const std::byte>) -> result<acknowledgement>;
extern template auto decode<read_object_request>(std::span<const std::byte>)
    -> result<read_object_request>;
extern template auto decode<read_object_response>(std::span<const std::byte>)
    -> result<read_object_response>;
extern template auto decode<scheduler_status>(std::span<const std::byte>)
    -> result<scheduler_status>;

} // namespace liquid::workers
