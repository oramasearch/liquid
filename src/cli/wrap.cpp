// SPDX-License-Identifier: MIT

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <limits>
#include <print>
#include <string_view>
#include <vector>

#include "liquid/build.hpp"
#include "liquid/error.hpp"
#include "liquid/runtime.hpp"

#include "cli/signer.hpp"

namespace liquid_cli {

auto print_tier(unsigned level, std::string_view mechanism) -> void;
auto wrap_tier_mechanism() -> std::string_view;

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

auto change_name(liquid::abi_change_kind kind) -> std::string_view {
    switch (kind) {
    case liquid::abi_change_kind::added:
        return "added";
    case liquid::abi_change_kind::removed:
        return "removed";
    case liquid::abi_change_kind::type_changed:
        return "type_changed";
    case liquid::abi_change_kind::size_changed:
        return "size_changed";
    case liquid::abi_change_kind::version_changed:
        return "version_changed";
    case liquid::abi_change_kind::inspection_truncated:
        return "inspection_truncated";
    }
    return "type_changed";
}

auto hit_rate(const liquid::wrap_report& report) -> double {
    const auto total = report.hits + report.misses;
    return total == 0 ? 0.0 : static_cast<double>(report.hits) * 100.0 / static_cast<double>(total);
}

auto remote_options(liquid::wrap_options& options) -> liquid::remote_config& {
    if (!options.remote) {
        options.remote = liquid::remote_config{};
    }
    return *options.remote;
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
                     shown_signer(miss.changed_by->by, miss.changed_by->identity_verified),
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
    auto fail_on_abi_break = false;
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
        if (command.empty() && argument == "--fail-on-abi-break") {
            fail_on_abi_break = true;
            continue;
        }
        if (command.empty() && argument == "--install-prefix") {
            if (++index == argc) {
                return usage("wrap --install-prefix requires an absolute path");
            }
            options.install_prefix = argv[index];
            continue;
        }
        if (command.empty() && argument == "--allow-embedded-prefix") {
            options.allow_embedded_prefix = true;
            continue;
        }
        if (command.empty() && argument == "--remote") {
            if (++index == argc) {
                return usage("wrap --remote requires an endpoint");
            }
            remote_options(options).endpoint = argv[index];
            remote_options(options).backend = liquid::remote_backend::reapi;
            continue;
        }
        if (command.empty() && argument == "--scheduler") {
            if (++index == argc) {
                return usage("wrap --scheduler requires an endpoint");
            }
            remote_options(options).endpoint = argv[index];
            remote_options(options).backend = liquid::remote_backend::liquid_scheduler;
            continue;
        }
        if (command.empty() && argument == "--remote-instance") {
            if (++index == argc) {
                return usage("wrap --remote-instance requires a name");
            }
            remote_options(options).instance_name = argv[index];
            continue;
        }
        if (command.empty() && argument == "--remote-header") {
            if (++index == argc) {
                return usage("wrap --remote-header requires name=value");
            }
            const auto value = std::string_view{argv[index]};
            const auto separator = value.find('=');
            if (separator == std::string_view::npos || separator == 0) {
                return usage("wrap --remote-header requires name=value");
            }
            remote_options(options).headers.insert_or_assign(
                std::string{value.substr(0, separator)}, std::string{value.substr(separator + 1)});
            continue;
        }
        if (command.empty() && argument == "--remote-tls") {
            remote_options(options).tls.enabled = true;
            continue;
        }
        if (command.empty() && argument == "--remote-no-verify") {
            remote_options(options).tls.enabled = true;
            remote_options(options).tls.verify_peer = false;
            continue;
        }
        if (command.empty() && argument == "--remote-ca") {
            if (++index == argc) {
                return usage("wrap --remote-ca requires a path");
            }
            remote_options(options).tls.enabled = true;
            remote_options(options).tls.certificate_authority = argv[index];
            continue;
        }
        if (command.empty() && argument == "--remote-client-cert") {
            if (++index == argc) {
                return usage("wrap --remote-client-cert requires a path");
            }
            remote_options(options).tls.enabled = true;
            remote_options(options).tls.client_certificate = argv[index];
            continue;
        }
        if (command.empty() && argument == "--remote-client-key") {
            if (++index == argc) {
                return usage("wrap --remote-client-key requires a path");
            }
            remote_options(options).tls.enabled = true;
            remote_options(options).tls.client_key = argv[index];
            continue;
        }
        if (command.empty() && argument == "--remote-publish") {
            remote_options(options).publish = true;
            continue;
        }
        if (command.empty() && argument == "--remote-deadline-ms") {
            if (++index == argc) {
                return usage("wrap --remote-deadline-ms requires a positive count");
            }
            auto count = std::uint64_t{};
            const auto value = std::string_view{argv[index]};
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), count);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                count == 0 ||
                count > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                return usage("wrap --remote-deadline-ms requires a positive count");
            }
            remote_options(options).deadline =
                std::chrono::milliseconds{static_cast<std::int64_t>(count)};
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
        if (command.empty() && argument == "--session") {
            if (++index == argc) {
                return usage("wrap --session requires an id");
            }
            auto session = liquid::parse_session_id(argv[index]);
            if (!session) {
                return fail(session.error());
            }
            options.session = *session;
            continue;
        }
        if (command.empty() && argument == "--input") {
            if (++index == argc) {
                return usage("wrap --input requires name=result-or-tree-digest");
            }
            const auto value = std::string_view{argv[index]};
            const auto separator = value.find('=');
            if (separator == std::string_view::npos || separator == 0 ||
                separator + 1 == value.size()) {
                return usage("wrap --input requires name=result-or-tree-digest");
            }
            auto source = liquid::parse_digest(value.substr(separator + 1));
            if (!source) {
                return usage("wrap --input requires name=result-or-tree-digest");
            }
            options.inputs.push_back({std::string{value.substr(0, separator)}, std::move(*source)});
            continue;
        }
        if (command.empty() && argument == "--")
            continue;
        command.emplace_back(argument);
    }
    if (command.empty())
        return usage("wrap requires a build command");
    if (options.remote && options.remote->endpoint.empty()) {
        return usage("wrap remote options require --remote or --scheduler <endpoint>");
    }
    if (options.allow_embedded_prefix && !options.install_prefix) {
        return usage("wrap --allow-embedded-prefix requires --install-prefix");
    }
    print_tier(1, wrap_tier_mechanism());
    auto result = liquid::runtime::block_on(
        liquid::build::wrap(std::filesystem::current_path(), std::move(command), options));
    if (!result)
        return fail(result.error());
    std::println("actions={} hits={} misses={} processes_started={} probes_started={} outer={} "
                 "output_tree={}",
                 result->actions,
                 result->hits,
                 result->misses,
                 result->processes_started,
                 result->probes_started,
                 result->outer == liquid::cache_status::hit ? "hit" : "miss",
                 result->output_tree ? liquid::to_string(*result->output_tree) : "unavailable");
    std::println("disk_hit_rate={:.2f}% bytes_transferred={} bytes_avoided={} bytes_rebuilt={}",
                 hit_rate(*result),
                 result->cache.disk.bytes_transferred,
                 result->cache.disk.bytes_avoided,
                 result->cache.bytes_rebuilt);
    std::println(
        "graph_validation_backlog={} last_graph_divergence={} last_graph_query={}",
        result->graph_validation_backlog,
        result->last_graph_divergence ? liquid::to_string(result->last_graph_divergence->id)
                                      : "none",
        result->last_graph_divergence ? result->last_graph_divergence->query.kind : "none");
    if (result->cache.remote.available) {
        std::println("remote_hits={} remote_misses={} remote_bytes_transferred={} "
                     "remote_bytes_avoided={}",
                     result->cache.remote.hits,
                     result->cache.remote.misses,
                     result->cache.remote.bytes_transferred,
                     result->cache.remote.bytes_avoided);
    }
    auto critical_path_time = std::chrono::milliseconds{};
    for (const auto& action : result->critical_path) {
        critical_path_time += action.duration;
    }
    std::println("wall_ms={} critical_path_ms={} peak_rss_bytes={} parallelism={:.2f} queued_ms={} "
                 "sandbox_ms={} exec_ms={} hash_ms={} store_ms={} bytes_served={} bytes_rebuilt={} "
                 "trace={}",
                 result->wall_time.count(),
                 critical_path_time.count(),
                 result->peak_rss_bytes,
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
    for (const auto& abi_break : result->abi_breaks) {
        auto summary = std::string{};
        const auto count = std::min<std::size_t>(abi_break.report.changes.size(), 3);
        for (auto index = std::size_t{}; index < count; ++index) {
            if (!summary.empty()) {
                summary += "; ";
            }
            summary += abi_break.report.changes[index].symbol + " " +
                       std::string{change_name(abi_break.report.changes[index].kind)};
        }
        std::println("abi-break: {} previous={} {}",
                     abi_break.output,
                     liquid::to_string(abi_break.previous),
                     summary);
    }
    return result->exit != 0                                  ? result->exit
           : fail_on_abi_break && !result->abi_breaks.empty() ? 1
                                                              : 0;
}

} // namespace liquid_cli
