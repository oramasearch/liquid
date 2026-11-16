// SPDX-License-Identifier: MIT

#include "workers/protocol.hpp"

#include <limits>
#include <type_traits>
#include <utility>

#include "objects/records.hpp"
#include "protobuf/wire.hpp"

namespace liquid::workers {
namespace {

auto protocol_error(std::string message) -> error {
    return {errc::corrupt, std::move(message), "worker protocol"};
}

auto encoded_digest(const reapi::Digest& value) -> result<std::vector<std::byte>> {
    return reapi::encode(value);
}

auto decoded_digest(std::span<const std::byte> value) -> result<reapi::Digest> {
    return reapi::decode_message<reapi::Digest>(value);
}

auto required_string(const protobuf::field& field, std::string_view name) -> result<std::string> {
    return protobuf::require_string(field, name);
}

auto required_bytes(const protobuf::field& field, std::string_view name)
    -> result<std::vector<std::byte>> {
    auto value = protobuf::require_bytes(field, name);
    if (!value) {
        return std::unexpected(value.error());
    }
    return std::vector<std::byte>{value->begin(), value->end()};
}

auto required_integer(const protobuf::field& field, std::string_view name)
    -> result<std::uint64_t> {
    return protobuf::require_varint(field, name);
}

template <typename Message, typename Visitor>
auto decode_fields(std::span<const std::byte> input, Visitor visitor) -> result<Message> {
    auto output = Message{};
    auto reader = protobuf::reader{input};
    while (!reader.finished()) {
        auto field = reader.next();
        if (!field) {
            return std::unexpected(field.error());
        }
        if (!*field) {
            break;
        }
        auto visited = visitor(output, **field);
        if (!visited) {
            return std::unexpected(visited.error());
        }
    }
    return output;
}

auto schedule_bytes(const execution_schedule& value) -> result<std::vector<std::byte>> {
    return objects::encode_execution_schedule(value);
}

auto schedule_value(const protobuf::field& field) -> result<execution_schedule> {
    auto bytes = protobuf::require_bytes(field, "schedule");
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    return objects::decode_execution_schedule(*bytes);
}

} // namespace

auto encode(const submit_request& value) -> result<std::vector<std::byte>> {
    auto action = encoded_digest(value.action);
    auto schedule = schedule_bytes(value.schedule);
    if (!action || !schedule) {
        return std::unexpected(!action ? action.error() : schedule.error());
    }
    auto output = protobuf::writer{};
    output.string(1, value.instance);
    output.message(2, *action);
    output.bytes(3, *schedule);
    output.string(4, to_string(value.native_action));
    output.boolean(5, value.remote_eligible);
    return std::move(output).take();
}

auto encode(const worker_request& value) -> result<std::vector<std::byte>> {
    auto output = protobuf::writer{};
    output.string(1, value.instance);
    output.string(2, value.worker);
    output.string(3, value.platform.os);
    output.string(4, value.platform.arch);
    output.varint(5, static_cast<std::uint64_t>(value.size_class));
    return std::move(output).take();
}

auto encode(const lease_response& value) -> result<std::vector<std::byte>> {
    auto output = protobuf::writer{};
    output.boolean(1, value.assigned);
    if (!value.assigned) {
        return std::move(output).take();
    }
    auto digest = encoded_digest(value.action_digest);
    auto schedule = schedule_bytes(value.schedule);
    if (!digest || !schedule) {
        return std::unexpected(!digest ? digest.error() : schedule.error());
    }
    output.string(2, value.operation);
    output.varint(3, value.token);
    output.varint(4, value.retry);
    output.message(5, *digest);
    output.varint(6, static_cast<std::uint64_t>(value.digest_function));
    output.bytes(7, value.action);
    output.bytes(8, value.command);
    output.bytes(9, *schedule);
    output.string(10, to_string(value.native_action));
    output.boolean(11, value.remote_eligible);
    return std::move(output).take();
}

auto encode(const heartbeat_request& value) -> result<std::vector<std::byte>> {
    auto output = protobuf::writer{};
    output.string(1, value.operation);
    output.string(2, value.worker);
    output.varint(3, value.token);
    return std::move(output).take();
}

auto encode(const complete_request& value) -> result<std::vector<std::byte>> {
    auto output = protobuf::writer{};
    output.string(1, value.operation);
    output.string(2, value.worker);
    output.varint(3, value.token);
    output.bytes(4, value.result);
    output.string(5, value.failure);
    return std::move(output).take();
}

auto encode(const acknowledgement& value) -> result<std::vector<std::byte>> {
    auto output = protobuf::writer{};
    output.boolean(1, value.accepted);
    output.string(2, value.message);
    return std::move(output).take();
}

auto encode(const read_object_request& value) -> result<std::vector<std::byte>> {
    auto output = protobuf::writer{};
    output.string(1, value.digest);
    return std::move(output).take();
}

auto encode(const read_object_response& value) -> result<std::vector<std::byte>> {
    auto output = protobuf::writer{};
    output.bytes(1, value.contents);
    return std::move(output).take();
}

auto encode(const scheduler_status& value) -> result<std::vector<std::byte>> {
    auto output = protobuf::writer{};
    output.varint(1, value.queued);
    output.varint(2, value.in_flight);
    output.varint(3, value.completed);
    output.varint(4, value.failed);
    output.varint(5, value.requeued);
    output.varint(6, value.workers);
    return std::move(output).take();
}

template <typename Message> auto decode(std::span<const std::byte> input) -> result<Message> {
    return decode_fields<Message>(
        input, [](Message& output, const protobuf::field& field) -> result<void> {
            if constexpr (std::is_same_v<Message, submit_request>) {
                if (field.number == 1) {
                    auto value = required_string(field, "Submit.instance");
                    if (!value) {
                        return std::unexpected(value.error());
                    }
                    output.instance = std::move(*value);
                } else if (field.number == 2) {
                    auto bytes = protobuf::require_bytes(field, "Submit.action");
                    auto value = bytes ? decoded_digest(*bytes)
                                       : result<reapi::Digest>{std::unexpected(bytes.error())};
                    if (!value) {
                        return std::unexpected(value.error());
                    }
                    output.action = std::move(*value);
                } else if (field.number == 3) {
                    auto value = schedule_value(field);
                    if (!value) {
                        return std::unexpected(value.error());
                    }
                    output.schedule = std::move(*value);
                } else if (field.number == 4) {
                    auto text = required_string(field, "Submit.native_action");
                    auto value =
                        text ? parse_digest(*text) : result<digest>{std::unexpected(text.error())};
                    if (!value) {
                        return std::unexpected(value.error());
                    }
                    output.native_action = *value;
                } else if (field.number == 5) {
                    auto value = required_integer(field, "Submit.remote_eligible");
                    if (!value) {
                        return std::unexpected(value.error());
                    }
                    output.remote_eligible = *value != 0;
                }
            } else if constexpr (std::is_same_v<Message, worker_request>) {
                if (field.number >= 1 && field.number <= 4) {
                    auto value = required_string(field, "Lease.worker");
                    if (!value) {
                        return std::unexpected(value.error());
                    }
                    auto* target = field.number == 1   ? &output.instance
                                   : field.number == 2 ? &output.worker
                                   : field.number == 3 ? &output.platform.os
                                                       : &output.platform.arch;
                    *target = std::move(*value);
                } else if (field.number == 5) {
                    auto value = required_integer(field, "Lease.size_class");
                    if (!value ||
                        *value > static_cast<std::uint64_t>(action_size_class::long_action)) {
                        return std::unexpected(value ? protocol_error("invalid worker size class")
                                                     : value.error());
                    }
                    output.size_class = static_cast<action_size_class>(*value);
                }
            } else if constexpr (std::is_same_v<Message, lease_response>) {
                if (field.number == 1) {
                    auto value = required_integer(field, "Lease.assigned");
                    if (!value) {
                        return std::unexpected(value.error());
                    }
                    output.assigned = *value != 0;
                } else if (field.number == 2) {
                    auto value = required_string(field, "Lease.operation");
                    if (!value) {
                        return std::unexpected(value.error());
                    }
                    output.operation = std::move(*value);
                } else if (field.number == 3 || field.number == 4) {
                    auto value = required_integer(field, "Lease.integer");
                    if (!value) {
                        return std::unexpected(value.error());
                    }
                    (field.number == 3 ? output.token : output.retry) = *value;
                } else if (field.number == 5) {
                    auto bytes = protobuf::require_bytes(field, "Lease.action_digest");
                    auto value = bytes ? decoded_digest(*bytes)
                                       : result<reapi::Digest>{std::unexpected(bytes.error())};
                    if (!value) {
                        return std::unexpected(value.error());
                    }
                    output.action_digest = std::move(*value);
                } else if (field.number == 6) {
                    auto value = required_integer(field, "Lease.digest_function");
                    if (!value || *value > std::numeric_limits<std::int32_t>::max()) {
                        return std::unexpected(value ? protocol_error("invalid digest function")
                                                     : value.error());
                    }
                    output.digest_function = static_cast<reapi::DigestFunctionValue>(*value);
                } else if (field.number == 7 || field.number == 8) {
                    auto value = required_bytes(field, "Lease.message");
                    if (!value) {
                        return std::unexpected(value.error());
                    }
                    (field.number == 7 ? output.action : output.command) = std::move(*value);
                } else if (field.number == 9) {
                    auto value = schedule_value(field);
                    if (!value) {
                        return std::unexpected(value.error());
                    }
                    output.schedule = std::move(*value);
                } else if (field.number == 10) {
                    auto text = required_string(field, "Lease.native_action");
                    auto value =
                        text ? parse_digest(*text) : result<digest>{std::unexpected(text.error())};
                    if (!value) {
                        return std::unexpected(value.error());
                    }
                    output.native_action = *value;
                } else if (field.number == 11) {
                    auto value = required_integer(field, "Lease.remote_eligible");
                    if (!value) {
                        return std::unexpected(value.error());
                    }
                    output.remote_eligible = *value != 0;
                }
            } else if constexpr (std::is_same_v<Message, heartbeat_request>) {
                if (field.number == 1 || field.number == 2) {
                    auto value = required_string(field, "Heartbeat.string");
                    if (!value) {
                        return std::unexpected(value.error());
                    }
                    (field.number == 1 ? output.operation : output.worker) = std::move(*value);
                } else if (field.number == 3) {
                    auto value = required_integer(field, "Heartbeat.token");
                    if (!value) {
                        return std::unexpected(value.error());
                    }
                    output.token = *value;
                }
            } else if constexpr (std::is_same_v<Message, complete_request>) {
                if (field.number == 1 || field.number == 2 || field.number == 5) {
                    auto value = required_string(field, "Complete.string");
                    if (!value) {
                        return std::unexpected(value.error());
                    }
                    auto* target = field.number == 1   ? &output.operation
                                   : field.number == 2 ? &output.worker
                                                       : &output.failure;
                    *target = std::move(*value);
                } else if (field.number == 3) {
                    auto value = required_integer(field, "Complete.token");
                    if (!value) {
                        return std::unexpected(value.error());
                    }
                    output.token = *value;
                } else if (field.number == 4) {
                    auto value = required_bytes(field, "Complete.result");
                    if (!value) {
                        return std::unexpected(value.error());
                    }
                    output.result = std::move(*value);
                }
            } else if constexpr (std::is_same_v<Message, acknowledgement>) {
                if (field.number == 1) {
                    auto value = required_integer(field, "Acknowledgement.accepted");
                    if (!value) {
                        return std::unexpected(value.error());
                    }
                    output.accepted = *value != 0;
                } else if (field.number == 2) {
                    auto value = required_string(field, "Acknowledgement.message");
                    if (!value) {
                        return std::unexpected(value.error());
                    }
                    output.message = std::move(*value);
                }
            } else if constexpr (std::is_same_v<Message, read_object_request>) {
                if (field.number == 1) {
                    auto value = required_string(field, "ReadObject.digest");
                    if (!value) {
                        return std::unexpected(value.error());
                    }
                    output.digest = std::move(*value);
                }
            } else if constexpr (std::is_same_v<Message, read_object_response>) {
                if (field.number == 1) {
                    auto value = required_bytes(field, "ReadObject.contents");
                    if (!value) {
                        return std::unexpected(value.error());
                    }
                    output.contents = std::move(*value);
                }
            } else if constexpr (std::is_same_v<Message, scheduler_status>) {
                if (field.number >= 1 && field.number <= 6) {
                    auto value = required_integer(field, "Status.value");
                    if (!value) {
                        return std::unexpected(value.error());
                    }
                    auto* target = field.number == 1   ? &output.queued
                                   : field.number == 2 ? &output.in_flight
                                   : field.number == 3 ? &output.completed
                                   : field.number == 4 ? &output.failed
                                   : field.number == 5 ? &output.requeued
                                                       : &output.workers;
                    *target = *value;
                }
            }
            return {};
        });
}

template auto decode<submit_request>(std::span<const std::byte>) -> result<submit_request>;
template auto decode<worker_request>(std::span<const std::byte>) -> result<worker_request>;
template auto decode<lease_response>(std::span<const std::byte>) -> result<lease_response>;
template auto decode<heartbeat_request>(std::span<const std::byte>) -> result<heartbeat_request>;
template auto decode<complete_request>(std::span<const std::byte>) -> result<complete_request>;
template auto decode<acknowledgement>(std::span<const std::byte>) -> result<acknowledgement>;
template auto decode<read_object_request>(std::span<const std::byte>)
    -> result<read_object_request>;
template auto decode<read_object_response>(std::span<const std::byte>)
    -> result<read_object_response>;
template auto decode<scheduler_status>(std::span<const std::byte>) -> result<scheduler_status>;

} // namespace liquid::workers
