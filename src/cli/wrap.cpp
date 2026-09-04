#include <algorithm>
#include <charconv>
#include <filesystem>
#include <print>
#include <string_view>
#include <vector>

#include "liquid/build.hpp"
#include "liquid/error.hpp"
#include "liquid/runtime.hpp"

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
    }
    return "unknown";
}

auto hit_rate(const liquid::wrap_report& report) -> double {
    const auto total = report.hits + report.misses;
    return total == 0 ? 0.0 : static_cast<double>(report.hits) * 100.0 / static_cast<double>(total);
}

void print_miss(const liquid::cache_miss& miss) {
    std::println(
        "miss: {}{}{}", reason_name(miss.reason), miss.path.empty() ? "" : ": ", miss.path);
    if (miss.expected)
        std::println("  old: {}", liquid::to_string(*miss.expected));
    if (miss.actual)
        std::println("  new: {}", liquid::to_string(*miss.actual));
    if (miss.lines) {
        std::println("  diff: @@ -{},{} +{},{} @@",
                     miss.lines->old_line,
                     miss.lines->old_count,
                     miss.lines->new_line,
                     miss.lines->new_count);
    }
    if (miss.changed_by) {
        std::println("  drip: {} by {} at {}",
                     liquid::to_string(miss.changed_by->drip),
                     miss.changed_by->by.taap_id,
                     miss.changed_by->at.time_since_epoch().count());
    }
    if (miss.reason == liquid::cache_miss_reason::command_changed) {
        const auto count = std::max(miss.expected_command.size(), miss.actual_command.size());
        for (auto index = std::size_t{}; index != count; ++index) {
            const auto old_value =
                index < miss.expected_command.size() ? miss.expected_command[index] : "<absent>";
            const auto new_value =
                index < miss.actual_command.size() ? miss.actual_command[index] : "<absent>";
            if (old_value != new_value)
                std::println("  argv[{}]: {} -> {}", index, old_value, new_value);
        }
    }
    if (!miss.environment_key.empty()) {
        std::println("  environment {}: {} -> {}",
                     miss.environment_key,
                     miss.expected_value.value_or("<absent>"),
                     miss.actual_value.value_or("<absent>"));
    }
    if (!miss.tool.empty())
        std::println("  tool: {}", miss.tool);
    if (!miss.detail.empty())
        std::println("  detail: {}", miss.detail);
}

} // namespace

auto wrap(int argc, char** argv) -> int {
    auto options = liquid::wrap_options{};
    auto command = std::vector<std::string>{};
    for (auto index = 0; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (command.empty() && argument == "--no-cache") {
            options.cache = false;
            continue;
        }
        if (command.empty() && argument == "--no-verify") {
            options.verify = false;
            continue;
        }
        if (command.empty() && argument == "--explain") {
            options.explain = true;
            continue;
        }
        if (command.empty() && argument == "--jobs") {
            if (++index == argc)
                return usage("wrap --jobs requires a positive count");
            const auto value = std::string_view{argv[index]};
            const auto parsed =
                std::from_chars(value.data(), value.data() + value.size(), options.jobs);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                options.jobs == 0) {
                return usage("wrap --jobs requires a positive count");
            }
            continue;
        }
        if (command.empty() && argument == "--target") {
            if (++index == argc) {
                return usage("wrap --target requires a target triple");
            }
            const auto value = std::string_view{argv[index]};
            if (value.empty() || value.starts_with('-')) {
                return usage("wrap --target requires a target triple");
            }
            options.target = value;
            continue;
        }
        if (command.empty() && argument == "--")
            continue;
        command.emplace_back(argument);
    }
    if (command.empty())
        return usage("wrap requires a build command");
    auto result = liquid::runtime::block_on(
        liquid::build::wrap(std::filesystem::current_path(), std::move(command), options));
    if (!result)
        return fail(result.error());
    std::println("actions={} hits={} misses={} processes_started={} probes_started={} outer={}",
                 result->actions,
                 result->hits,
                 result->misses,
                 result->processes_started,
                 result->probes_started,
                 result->outer == liquid::cache_status::hit ? "hit" : "miss");
    std::println("disk_hit_rate={:.2f}% bytes_transferred={} bytes_avoided={} bytes_rebuilt={}",
                 hit_rate(*result),
                 result->cache.disk.bytes_transferred,
                 result->cache.disk.bytes_avoided,
                 result->cache.bytes_rebuilt);
    auto critical_path_time = std::chrono::milliseconds{};
    for (const auto& action : result->critical_path) {
        critical_path_time += action.duration;
    }
    std::println("wall_ms={} critical_path_ms={} parallelism={:.2f} queued_ms={} sandbox_ms={} "
                 "exec_ms={} hash_ms={} store_ms={} bytes_served={} bytes_rebuilt={} trace={}",
                 result->wall_time.count(),
                 critical_path_time.count(),
                 result->parallelism,
                 result->phase_totals.queued.count(),
                 result->phase_totals.sandbox.count(),
                 result->phase_totals.exec.count(),
                 result->phase_totals.hash.count(),
                 result->phase_totals.store.count(),
                 result->cache.bytes_served,
                 result->cache.bytes_rebuilt,
                 result->trace ? liquid::to_string(*result->trace) : "unavailable");
    for (const auto& [reason, count] : result->cache.misses_by_reason)
        std::println("misses[{}]={}", reason_name(reason), count);
    for (const auto& action : result->critical_path) {
        std::println("critical_action={} duration_ms={} queued_ms={} sandbox_ms={} exec_ms={} "
                     "hash_ms={} store_ms={}",
                     liquid::to_string(action.action),
                     action.duration.count(),
                     action.timing.queued.count(),
                     action.timing.sandbox.count(),
                     action.timing.exec.count(),
                     action.timing.hash.count(),
                     action.timing.store.count());
    }
    if (options.explain) {
        for (const auto& miss : result->why)
            print_miss(miss);
    }
    for (const auto& warning : result->policy_warnings) {
        std::println(stderr, "warning: {}", warning);
    }
    return result->exit;
}

} // namespace liquid_cli
