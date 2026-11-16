// SPDX-License-Identifier: MIT

#include <algorithm>
#include <format>
#include <print>
#include <string>
#include <string_view>

#include "liquid/build.hpp"
#include "liquid/error.hpp"
#include "liquid/runtime.hpp"

#include "cli/signer.hpp"

namespace liquid_cli {
namespace {

auto fail(const liquid::error& value) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(value.code), value.message, value.context);
    return 1;
}

auto usage(std::string_view message) -> int {
    std::println(stderr, "error: usage: {} ()", message);
    return 2;
}

auto reason_name(liquid::cache_miss_reason reason) -> std::string_view {
    switch (reason) {
    case liquid::cache_miss_reason::input_changed:
        return "input changed";
    case liquid::cache_miss_reason::command_changed:
        return "command changed";
    case liquid::cache_miss_reason::environment_changed:
        return "environment changed";
    case liquid::cache_miss_reason::toolchain_changed:
        return "toolchain changed";
    case liquid::cache_miss_reason::sandbox_changed:
        return "sandbox changed";
    case liquid::cache_miss_reason::never_built:
        return "never built";
    case liquid::cache_miss_reason::divergent_result_quarantined:
        return "divergent result quarantined";
    case liquid::cache_miss_reason::unknown:
        return "unknown";
    case liquid::cache_miss_reason::policy_refused:
        return "policy refused";
    case liquid::cache_miss_reason::non_hermetic_read:
        return "non-hermetic read";
    }
    return "unknown";
}

auto decision_name(liquid::execution_decision decision) -> std::string_view {
    switch (decision) {
    case liquid::execution_decision::local:
        return "local";
    case liquid::execution_decision::remote:
        return "remote";
    case liquid::execution_decision::race:
        return "race";
    }
    return "local";
}

auto size_class_name(liquid::action_size_class value) -> std::string_view {
    switch (value) {
    case liquid::action_size_class::unknown:
        return "unknown";
    case liquid::action_size_class::tiny:
        return "tiny";
    case liquid::action_size_class::short_action:
        return "short";
    case liquid::action_size_class::medium:
        return "medium";
    case liquid::action_size_class::long_action:
        return "long";
    }
    return "unknown";
}

auto json_string(std::string_view value) -> std::string {
    auto output = std::string{"\""};
    for (const auto character : value) {
        switch (character) {
        case '\\':
            output += "\\\\";
            break;
        case '"':
            output += "\\\"";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20) {
                output += std::format("\\u{:04x}", static_cast<unsigned char>(character));
            } else {
                output.push_back(character);
            }
            break;
        }
    }
    output.push_back('"');
    return output;
}

auto json_optional(const std::optional<liquid::digest>& value) -> std::string {
    return value ? json_string(liquid::to_string(*value)) : "null";
}

auto json_optional(const std::optional<std::string>& value) -> std::string {
    return value ? json_string(*value) : "null";
}

auto json_command(const std::vector<std::string>& command) -> std::string {
    auto output = std::string{"["};
    for (auto index = std::size_t{}; index != command.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        output += json_string(command[index]);
    }
    output.push_back(']');
    return output;
}

auto history_json(const std::vector<liquid::action_timing>& history) -> std::string {
    auto output = std::string{"["};
    for (auto index = std::size_t{}; index != history.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        const auto& timing = history[index];
        output += std::to_string(
            (timing.queued + timing.sandbox + timing.exec + timing.hash + timing.store).count());
    }
    output.push_back(']');
    return output;
}

void print_schedule(const std::optional<liquid::execution_schedule>& schedule) {
    if (!schedule) {
        return;
    }
    const auto& inputs = schedule->inputs;
    std::println("decision: {}", decision_name(schedule->decision));
    std::println("model: history_ms={} size_class={} input_bytes={} remote_state_queried={} "
                 "remote_missing_bytes={} remote_queue_depth={} "
                 "remote_cache_locality_permille={} batch_supported={} batch_size={}",
                 history_json(inputs.history),
                 size_class_name(inputs.size_class),
                 inputs.input_bytes,
                 inputs.remote_state_queried,
                 inputs.remote_missing_bytes,
                 inputs.remote_queue_depth,
                 inputs.remote_cache_locality_permille,
                 inputs.batch_supported,
                 inputs.batch_size);
    std::println("model estimate: local_ms={} remote_ms={} reason={}",
                 schedule->local_estimate.count(),
                 schedule->remote_estimate.count(),
                 schedule->reason);
}

auto schedule_json(const std::optional<liquid::execution_schedule>& schedule) -> std::string {
    if (!schedule) {
        return "null";
    }
    const auto& inputs = schedule->inputs;
    return std::format(
        "{{\"decision\":{},\"history_ms\":{},\"size_class\":{},\"input_bytes\":{},"
        "\"remote_state_queried\":{},\"remote_missing_bytes\":{},\"remote_queue_depth\":{},"
        "\"remote_cache_locality_permille\":{},\"batch_supported\":{},\"batch_size\":{},"
        "\"local_estimate_ms\":{},\"remote_estimate_ms\":{},\"reason\":{}}}",
        json_string(decision_name(schedule->decision)),
        history_json(inputs.history),
        json_string(size_class_name(inputs.size_class)),
        inputs.input_bytes,
        inputs.remote_state_queried,
        inputs.remote_missing_bytes,
        inputs.remote_queue_depth,
        inputs.remote_cache_locality_permille,
        inputs.batch_supported,
        inputs.batch_size,
        schedule->local_estimate.count(),
        schedule->remote_estimate.count(),
        json_string(schedule->reason));
}

void print_command_diff(const liquid::cache_miss& cause) {
    const auto count = std::max(cause.expected_command.size(), cause.actual_command.size());
    for (auto index = std::size_t{}; index != count; ++index) {
        const auto old_value =
            index < cause.expected_command.size() ? cause.expected_command[index] : "<absent>";
        const auto new_value =
            index < cause.actual_command.size() ? cause.actual_command[index] : "<absent>";
        if (old_value != new_value) {
            std::println("argv[{}]: {} -> {}", index, old_value, new_value);
        }
    }
}

void print_cause(const liquid::cache_miss& cause) {
    std::println("reason: {}", reason_name(cause.reason));
    if (!cause.path.empty()) {
        std::println("input: {}", cause.path);
    }
    if (cause.expected) {
        std::println("old: {}", liquid::to_string(*cause.expected));
    }
    if (cause.actual) {
        std::println("new: {}", liquid::to_string(*cause.actual));
    } else if (cause.expected && cause.reason == liquid::cache_miss_reason::input_changed) {
        std::println("new: <missing>");
    }
    if (cause.lines) {
        std::println("diff: @@ -{},{} +{},{} @@",
                     cause.lines->old_line,
                     cause.lines->old_count,
                     cause.lines->new_line,
                     cause.lines->new_count);
    }
    if (cause.changed_by) {
        std::println("drip: {}", liquid::to_string(cause.changed_by->drip));
        std::println("by: {}",
                     shown_signer(cause.changed_by->by, cause.changed_by->identity_verified));
        std::println("at: {}", cause.changed_by->at.time_since_epoch().count());
    }
    if (cause.reason == liquid::cache_miss_reason::command_changed) {
        print_command_diff(cause);
    }
    if (!cause.environment_key.empty()) {
        std::println("environment: {}: {} -> {}",
                     cause.environment_key,
                     cause.expected_value.value_or("<absent>"),
                     cause.actual_value.value_or("<absent>"));
    }
    if (!cause.tool.empty()) {
        std::println("tool: {}", cause.tool);
    }
    if (!cause.detail.empty()) {
        std::println("detail: {}", cause.detail);
    }
}

void print_why_json(const liquid::why_report& report) {
    const auto& cause = report.cause;
    std::print("{{\"target\":{},\"lookup\":{},\"reason\":{},",
               json_string(report.target),
               json_string(report.lookup == liquid::cache_status::hit ? "hit" : "miss"),
               json_string(reason_name(cause.reason)));
    std::print("\"result\":{},\"previous_result\":{},",
               report.result ? json_string(liquid::to_string(report.result->value)) : "null",
               report.previous_result
                   ? json_string(liquid::to_string(report.previous_result->value))
                   : "null");
    std::print("\"action\":{},\"previous_action\":{},",
               report.action ? json_string(liquid::to_string(*report.action)) : "null",
               report.previous_action ? json_string(liquid::to_string(*report.previous_action))
                                      : "null");
    std::print("\"path\":{},\"old_digest\":{},\"new_digest\":{},",
               cause.path.empty() ? "null" : json_string(cause.path),
               json_optional(cause.expected),
               json_optional(cause.actual));
    std::print("\"old_command\":{},\"new_command\":{},",
               json_command(cause.expected_command),
               json_command(cause.actual_command));
    std::print("\"environment_key\":{},\"old_value\":{},\"new_value\":{},",
               cause.environment_key.empty() ? "null" : json_string(cause.environment_key),
               json_optional(cause.expected_value),
               json_optional(cause.actual_value));
    if (cause.lines) {
        std::print("\"line_change\":{{\"old_line\":{},\"old_count\":{},"
                   "\"new_line\":{},\"new_count\":{}}},",
                   cause.lines->old_line,
                   cause.lines->old_count,
                   cause.lines->new_line,
                   cause.lines->new_count);
    } else {
        std::print("\"line_change\":null,");
    }
    std::print(
        "\"tool\":{},\"drip\":{},\"by\":{},\"identity_verified\":{},\"at\":{},"
        "\"detail\":{},\"schedule\":{},"
        "\"cache\":{{\"disk_hits\":{},\"disk_misses\":{},"
        "\"remote_available\":{},\"remote_hits\":{},\"remote_misses\":{}}}}}\n",
        cause.tool.empty() ? "null" : json_string(cause.tool),
        cause.changed_by ? json_string(liquid::to_string(cause.changed_by->drip)) : "null",
        cause.changed_by
            ? json_string(shown_signer(cause.changed_by->by, cause.changed_by->identity_verified))
            : "null",
        cause.changed_by ? (cause.changed_by->identity_verified ? "true" : "false") : "null",
        cause.changed_by ? std::to_string(cause.changed_by->at.time_since_epoch().count()) : "null",
        cause.detail.empty() ? "null" : json_string(cause.detail),
        schedule_json(report.schedule),
        report.cache.disk.hits,
        report.cache.disk.misses,
        report.cache.remote.available,
        report.cache.remote.hits,
        report.cache.remote.misses);
}

auto rate(std::uint64_t hits, std::uint64_t misses) -> double {
    const auto total = hits + misses;
    return total == 0 ? 0.0 : static_cast<double>(hits) * 100.0 / static_cast<double>(total);
}

} // namespace

auto why(int argc, char** argv) -> int {
    auto target = std::string{};
    auto json = false;
    for (auto index = 0; index != argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--json") {
            json = true;
        } else if (target.empty()) {
            target = argument;
        } else {
            return usage("why <target> [--json]");
        }
    }
    if (target.empty()) {
        return usage("why requires a target");
    }
    auto report = liquid::runtime::block_on(
        liquid::build::why(std::filesystem::current_path(), std::move(target)));
    if (!report) {
        return fail(report.error());
    }
    if (json) {
        print_why_json(*report);
    } else {
        std::println("target: {}", report->target);
        std::println("lookup: {}", report->lookup == liquid::cache_status::hit ? "hit" : "miss");
        if (report->result) {
            std::println("result: {}", liquid::to_string(report->result->value));
        }
        if (report->previous_result) {
            std::println("previous result: {}", liquid::to_string(report->previous_result->value));
        }
        if (report->action) {
            std::println("action: {}", liquid::to_string(*report->action));
        }
        if (report->previous_action) {
            std::println("previous action: {}", liquid::to_string(*report->previous_action));
        }
        print_schedule(report->schedule);
        std::println("cache tiers: disk={}/{} remote={}/{}{}",
                     report->cache.disk.hits,
                     report->cache.disk.misses,
                     report->cache.remote.hits,
                     report->cache.remote.misses,
                     report->cache.remote.available ? "" : " (unavailable)");
        print_cause(report->cause);
    }
    return 0;
}

auto cache(int argc, char** argv) -> int {
    if (argc < 1 || std::string_view{argv[0]} != "stats") {
        return usage("cache stats [--json]");
    }
    const auto json = argc == 2 && std::string_view{argv[1]} == "--json";
    if (argc > 2 || (argc == 2 && !json)) {
        return usage("cache stats [--json]");
    }
    auto report =
        liquid::runtime::block_on(liquid::build::cache_stats(std::filesystem::current_path()));
    if (!report) {
        return fail(report.error());
    }
    if (json) {
        std::print("{{\"hits\":{},\"misses\":{},\"disk\":{{\"available\":true,"
                   "\"hits\":{},\"misses\":{},\"hit_rate\":{:.2f},"
                   "\"bytes_transferred\":{},\"bytes_avoided\":{}}},"
                   "\"memory\":{{\"available\":false}},",
                   report->hits,
                   report->misses,
                   report->disk.hits,
                   report->disk.misses,
                   rate(report->disk.hits, report->disk.misses),
                   report->disk.bytes_transferred,
                   report->disk.bytes_avoided);
        if (report->remote.available) {
            std::print("\"remote\":{{\"available\":true,\"hits\":{},\"misses\":{},"
                       "\"hit_rate\":{:.2f},\"bytes_transferred\":{},"
                       "\"bytes_avoided\":{}}},",
                       report->remote.hits,
                       report->remote.misses,
                       rate(report->remote.hits, report->remote.misses),
                       report->remote.bytes_transferred,
                       report->remote.bytes_avoided);
        } else {
            std::print("\"remote\":{{\"available\":false}},");
        }
        std::print("\"bytes_rebuilt\":{},\"misses_by_reason\":{{", report->bytes_rebuilt);
        auto first = true;
        for (const auto& [reason, count] : report->misses_by_reason) {
            if (!first)
                std::print(",");
            std::print("{}:{}", json_string(reason_name(reason)), count);
            first = false;
        }
        std::println("}}}}");
    } else {
        std::println("hits={} misses={}", report->hits, report->misses);
        std::println("disk: hit_rate={:.2f}% hits={} misses={}",
                     rate(report->disk.hits, report->disk.misses),
                     report->disk.hits,
                     report->disk.misses);
        std::println("memory: unavailable");
        if (report->remote.available) {
            std::println("remote: hit_rate={:.2f}% hits={} misses={}",
                         rate(report->remote.hits, report->remote.misses),
                         report->remote.hits,
                         report->remote.misses);
        } else {
            std::println("remote: unavailable");
        }
        std::println("bytes_transferred={} bytes_avoided={} bytes_rebuilt={}",
                     report->disk.bytes_transferred,
                     report->disk.bytes_avoided,
                     report->bytes_rebuilt);
        for (const auto& [reason, count] : report->misses_by_reason) {
            std::println("misses[{}]={}", reason_name(reason), count);
        }
    }
    return 0;
}

} // namespace liquid_cli
