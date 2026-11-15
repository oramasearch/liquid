// SPDX-License-Identifier: MIT

#include "workers/scheduler.hpp"

#include <algorithm>
#include <utility>

#include "identity/taap.hpp"
#include "objects/records.hpp"

namespace liquid::workers {
namespace {

auto scheduler_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto digest_key(const reapi::Digest& value, reapi::DigestFunctionValue function) -> std::string {
    return std::to_string(static_cast<std::int32_t>(function)) + "/" + value.hash + "/" +
           std::to_string(value.size_bytes);
}

auto execute_response(const reapi::ActionResult& value) -> result<reapi::Any> {
    auto response = reapi::encode(reapi::ExecuteResponse{.result = value});
    if (!response) {
        return std::unexpected(response.error());
    }
    return reapi::Any{.type_url =
                          "type.googleapis.com/build.bazel.remote.execution.v2.ExecuteResponse",
                      .value = std::move(*response)};
}

} // namespace

scheduler::scheduler(std::chrono::milliseconds lease_duration) : lease_duration_(lease_duration) {}

auto scheduler::fits(action_size_class worker, action_size_class action) -> bool {
    const auto normalized =
        action == action_size_class::unknown ? action_size_class::medium : action;
    return static_cast<std::uint8_t>(worker) >= static_cast<std::uint8_t>(normalized);
}

void scheduler::expire_locked(std::chrono::steady_clock::time_point now) {
    for (const auto& [name, entry] : by_name_) {
        if (entry->state != stage::leased || entry->lease_expires > now) {
            continue;
        }
        entry->worker.clear();
        entry->token = 0;
        if (entry->retry == 0) {
            entry->retry = 1;
            entry->state = stage::queued;
            ++requeued_;
            continue;
        }
        entry->state = stage::failed;
        entry->failure = "worker lease expired after the single requeue";
    }
    const auto stale_before = now - lease_duration_ * 2;
    std::erase_if(workers_, [&](const auto& item) { return item.second < stale_before; });
}

auto scheduler::operation_message(const operation_entry& entry) const -> result<reapi::Operation> {
    auto output = reapi::Operation{.name = entry.name};
    if (entry.state == stage::completed) {
        auto response = execute_response(*entry.result);
        if (!response) {
            return std::unexpected(response.error());
        }
        output.done = true;
        output.result = std::move(*response);
        return output;
    }
    if (entry.state == stage::failed || entry.state == stage::cancelled) {
        output.done = true;
        output.result = reapi::Status{.code = entry.state == stage::cancelled ? 1 : 13,
                                      .message = entry.failure};
        return output;
    }
    auto metadata = reapi::ExecuteOperationMetadata{.stage = entry.state == stage::queued
                                                                 ? reapi::ExecutionStage::queued
                                                                 : reapi::ExecutionStage::executing,
                                                    .action_digest = entry.action_digest,
                                                    .digest_function = entry.digest_function};
    auto encoded = reapi::encode(metadata);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    output.metadata = reapi::Any{
        .type_url = "type.googleapis.com/build.bazel.remote.execution.v2.ExecuteOperationMetadata",
        .value = std::move(*encoded)};
    return output;
}

auto scheduler::submit(reapi::Digest action_digest,
                       reapi::DigestFunctionValue digest_function,
                       std::vector<std::byte> action,
                       std::vector<std::byte> command,
                       reapi::DecodedAction decoded,
                       digest native_action,
                       execution_schedule schedule) -> result<reapi::Operation> {
    auto lock = std::scoped_lock{mutex_};
    expire_locked(std::chrono::steady_clock::now());
    const auto key = digest_key(action_digest, digest_function);
    if (const auto found = by_action_.find(key); found != by_action_.end()) {
        return operation_message(*found->second);
    }
    auto entry = std::make_shared<operation_entry>();
    entry->name = "operations/liquid-" + std::to_string(next_operation_++);
    entry->action_digest = std::move(action_digest);
    entry->digest_function = digest_function;
    entry->action = std::move(action);
    entry->command = std::move(command);
    entry->decoded = std::move(decoded);
    entry->native_action = native_action;
    entry->schedule = std::move(schedule);
    by_action_.emplace(key, entry);
    by_name_.emplace(entry->name, entry);
    return operation_message(*entry);
}

auto scheduler::lease(const worker_request& request) -> result<lease_response> {
    auto lock = std::scoped_lock{mutex_};
    const auto now = std::chrono::steady_clock::now();
    expire_locked(now);
    workers_.insert_or_assign(request.worker, now);
    auto selected = std::shared_ptr<operation_entry>{};
    for (const auto& [name, entry] : by_name_) {
        if (entry->state != stage::queued ||
            entry->decoded.input.platform.os != request.platform.os ||
            entry->decoded.input.platform.arch != request.platform.arch ||
            !fits(request.size_class, entry->schedule.inputs.size_class)) {
            continue;
        }
        if (!selected || static_cast<std::uint8_t>(entry->schedule.inputs.size_class) >
                             static_cast<std::uint8_t>(selected->schedule.inputs.size_class)) {
            selected = entry;
        }
    }
    if (!selected) {
        return lease_response{};
    }
    selected->state = stage::leased;
    selected->worker = request.worker;
    selected->token = next_token_++;
    selected->lease_expires = now + lease_duration_;
    return lease_response{.assigned = true,
                          .operation = selected->name,
                          .token = selected->token,
                          .retry = selected->retry,
                          .action_digest = selected->action_digest,
                          .digest_function = selected->digest_function,
                          .action = selected->action,
                          .command = selected->command,
                          .schedule = selected->schedule,
                          .native_action = selected->native_action,
                          .remote_eligible = selected->decoded.input.remote_eligible};
}

auto scheduler::heartbeat(const heartbeat_request& request) -> acknowledgement {
    auto lock = std::scoped_lock{mutex_};
    const auto now = std::chrono::steady_clock::now();
    expire_locked(now);
    workers_.insert_or_assign(request.worker, now);
    const auto found = by_name_.find(request.operation);
    if (found == by_name_.end() || found->second->state != stage::leased ||
        found->second->token != request.token || found->second->worker != request.worker) {
        return {.message = "lease is no longer active"};
    }
    found->second->lease_expires = now + lease_duration_;
    return {.accepted = true};
}

auto scheduler::complete(const complete_request& request, reapi::ActionResult wire_result)
    -> acknowledgement {
    auto lock = std::scoped_lock{mutex_};
    expire_locked(std::chrono::steady_clock::now());
    const auto found = by_name_.find(request.operation);
    if (found == by_name_.end()) {
        return {.message = "operation is unknown"};
    }
    auto& entry = *found->second;
    if (entry.state == stage::completed) {
        return {.accepted = true, .message = "result was already accepted"};
    }
    if (entry.state != stage::leased || entry.token != request.token ||
        entry.worker != request.worker) {
        return {.message = "lease is no longer active"};
    }
    auto action_bytes =
        objects::encode(objects::action_record{entry.decoded.input, entry.decoded.sandbox});
    auto action_id = action_bytes
                         ? objects::digest_of(entry.decoded.input.inputs.alg, *action_bytes)
                         : result<digest>{std::unexpected(action_bytes.error())};
    auto native =
        action_id
            ? reapi::from_reapi_result(wire_result, *action_id, entry.decoded.input.inputs.alg)
            : result<action_result>{std::unexpected(action_id.error())};
    auto expected_schedule = objects::encode_execution_schedule(entry.schedule);
    auto actual_schedule =
        native && native->schedule
            ? objects::encode_execution_schedule(*native->schedule)
            : result<std::vector<std::byte>>{
                  std::unexpected(scheduler_error(errc::corrupt, "worker result has no schedule"))};
    if (!native || native->worker != request.worker || native->producer.taap_id != request.worker ||
        !expected_schedule || !actual_schedule || *actual_schedule != *expected_schedule) {
        return {.message =
                    native ? "worker result does not match its lease" : native.error().message};
    }
    auto signature = identity_detail::parse_signature(native->sig);
    auto body = objects::encode_result_body(*native);
    auto verified = signature && body ? verify(native->producer, "result", *body, *signature)
                                      : result<bool>{false};
    if (!verified || !*verified) {
        return {.message =
                    verified ? "worker result signature is invalid" : verified.error().message};
    }
    entry.result = std::move(wire_result);
    entry.state = stage::completed;
    entry.lease_expires = {};
    return {.accepted = true};
}

auto scheduler::fail(const complete_request& request) -> acknowledgement {
    auto lock = std::scoped_lock{mutex_};
    expire_locked(std::chrono::steady_clock::now());
    const auto found = by_name_.find(request.operation);
    if (found == by_name_.end()) {
        return {.message = "operation is unknown"};
    }
    auto& entry = *found->second;
    if (entry.state != stage::leased || entry.token != request.token ||
        entry.worker != request.worker) {
        return {.message = "lease is no longer active"};
    }
    entry.worker.clear();
    entry.token = 0;
    if (entry.retry == 0) {
        entry.retry = 1;
        entry.state = stage::queued;
        ++requeued_;
        return {.accepted = true, .message = "action requeued"};
    }
    entry.state = stage::failed;
    entry.failure = request.failure.empty() ? "worker execution failed" : request.failure;
    return {.accepted = true, .message = "action failed after the single requeue"};
}

auto scheduler::operation(std::string_view name) -> result<reapi::Operation> {
    auto lock = std::scoped_lock{mutex_};
    expire_locked(std::chrono::steady_clock::now());
    const auto found = by_name_.find(std::string{name});
    if (found == by_name_.end()) {
        return std::unexpected(
            scheduler_error(errc::not_found, "execution operation is unknown", std::string{name}));
    }
    return operation_message(*found->second);
}

auto scheduler::action_digest(std::string_view name) -> result<reapi::Digest> {
    auto lock = std::scoped_lock{mutex_};
    const auto found = by_name_.find(std::string{name});
    if (found == by_name_.end()) {
        return std::unexpected(
            scheduler_error(errc::not_found, "execution operation is unknown", std::string{name}));
    }
    return found->second->action_digest;
}

auto scheduler::cancel(std::string_view name) -> result<void> {
    auto lock = std::scoped_lock{mutex_};
    const auto found = by_name_.find(std::string{name});
    if (found == by_name_.end()) {
        return std::unexpected(
            scheduler_error(errc::not_found, "execution operation is unknown", std::string{name}));
    }
    if (found->second->state == stage::completed) {
        return {};
    }
    found->second->state = stage::cancelled;
    found->second->failure = "execution operation was cancelled";
    return {};
}

auto scheduler::status() -> scheduler_status {
    auto lock = std::scoped_lock{mutex_};
    expire_locked(std::chrono::steady_clock::now());
    auto output = scheduler_status{.requeued = requeued_, .workers = workers_.size()};
    for (const auto& [name, entry] : by_name_) {
        if (entry->state == stage::queued) {
            ++output.queued;
        } else if (entry->state == stage::leased) {
            ++output.in_flight;
        } else if (entry->state == stage::completed) {
            ++output.completed;
        } else if (entry->state == stage::failed) {
            ++output.failed;
        }
    }
    return output;
}

auto scheduler::ready() -> bool {
    return status().workers != 0;
}

} // namespace liquid::workers
