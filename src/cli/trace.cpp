#include <filesystem>
#include <print>
#include <string>
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

auto status_name(liquid::cache_status status) -> std::string_view {
    switch (status) {
    case liquid::cache_status::miss:
        return "miss";
    case liquid::cache_status::hit:
        return "hit";
    case liquid::cache_status::divergent:
        return "divergent";
    }
    return "miss";
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

auto history_text(const std::vector<liquid::action_timing>& history) -> std::string {
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
    std::println("  decision={} history_ms={} size_class={} input_bytes={} "
                 "remote_state_queried={} remote_missing_bytes={} remote_queue_depth={} "
                 "remote_cache_locality_permille={} batch_supported={} batch_size={} "
                 "local_estimate_ms={} remote_estimate_ms={} reason={}",
                 decision_name(schedule->decision),
                 history_text(inputs.history),
                 size_class_name(inputs.size_class),
                 inputs.input_bytes,
                 inputs.remote_state_queried,
                 inputs.remote_missing_bytes,
                 inputs.remote_queue_depth,
                 inputs.remote_cache_locality_permille,
                 inputs.batch_supported,
                 inputs.batch_size,
                 schedule->local_estimate.count(),
                 schedule->remote_estimate.count(),
                 schedule->reason);
}

void print_action(const liquid::critical_action& action) {
    std::println("action={} result={} cache={} duration_ms={} queued_ms={} sandbox_ms={} "
                 "exec_ms={} hash_ms={} store_ms={}",
                 liquid::to_string(action.action),
                 liquid::to_string(action.result.value),
                 status_name(action.cache),
                 action.duration.count(),
                 action.timing.queued.count(),
                 action.timing.sandbox.count(),
                 action.timing.exec.count(),
                 action.timing.hash.count(),
                 action.timing.store.count());
    for (const auto& output : action.outputs) {
        std::println("  output={}", output);
    }
    print_schedule(action.schedule);
}

auto print_build_events(liquid::build_handle handle) -> liquid::runtime::task<int> {
    auto subscription = handle.events();
    if (!subscription) {
        co_return std::unexpected(subscription.error());
    }
    auto exit = 0;
    while (true) {
        auto event = co_await subscription->next();
        if (!event) {
            co_return std::unexpected(event.error());
        }
        if (!*event) {
            break;
        }
        auto json = liquid::event_json(**event);
        if (!json) {
            co_return std::unexpected(json.error());
        }
        std::println("{}", *json);
        if ((*event)->summary) {
            exit = (*event)->summary->exit;
        }
    }
    co_return exit;
}

} // namespace

auto wrap(int argc, char** argv) -> int;

auto build(int argc, char** argv) -> int {
    if (argc >= 1 && std::string_view{argv[0]} == "--events") {
        if (argc == 1) {
            return usage("build --events <command> [args...]");
        }
        auto command = std::vector<std::string>{};
        command.reserve(static_cast<std::size_t>(argc - 1));
        for (auto index = 1; index != argc; ++index) {
            command.emplace_back(argv[index]);
        }
        auto handle =
            liquid::build::start_wrap(std::filesystem::current_path(), std::move(command));
        if (!handle) {
            return fail(handle.error());
        }
        auto result = liquid::runtime::block_on(print_build_events(std::move(*handle)));
        return result ? *result : fail(result.error());
    }
    if (argc >= 1 && std::string_view{argv[0]} == "--profile") {
        return wrap(argc - 1, argv + 1);
    }
    if (argc < 1 || std::string_view{argv[0]} != "trace") {
        return usage("build --events <command> [args...] | build --profile <command> [args...] | "
                     "build trace [--chrome]");
    }
    const auto chrome = argc == 2 && std::string_view{argv[1]} == "--chrome";
    if (argc > 2 || (argc == 2 && !chrome)) {
        return usage("build trace [--chrome]");
    }
    auto trace =
        liquid::runtime::block_on(liquid::build::latest_trace(std::filesystem::current_path()));
    if (!trace) {
        return fail(trace.error());
    }
    if (chrome) {
        auto json = liquid::build::chrome_trace(*trace);
        if (!json) {
            return fail(json.error());
        }
        std::println("{}", *json);
        return 0;
    }
    std::println("trace={} wall_ms={} actions={} edges={} hits={} misses={} bytes_avoided={} "
                 "bytes_rebuilt={}",
                 liquid::to_string(trace->id),
                 trace->wall_time.count(),
                 trace->actions.size(),
                 trace->edges.size(),
                 trace->cache.hits,
                 trace->cache.misses,
                 trace->cache.disk.bytes_avoided,
                 trace->cache.bytes_rebuilt);
    for (const auto& action : trace->actions) {
        std::println("action={}", liquid::to_string(action.action));
        print_schedule(action.schedule);
    }
    return 0;
}

auto why_slow(int argc, char** argv) -> int {
    if (argc != 1) {
        return usage("why-slow <target>");
    }
    auto report = liquid::runtime::block_on(
        liquid::build::why_slow(std::filesystem::current_path(), argv[0]));
    if (!report) {
        return fail(report.error());
    }
    std::println("target={} trace={} critical_path_ms={} actions={}",
                 report->target,
                 liquid::to_string(report->trace),
                 report->duration.count(),
                 report->critical_path.size());
    for (const auto& action : report->critical_path) {
        print_action(action);
    }
    return 0;
}

} // namespace liquid_cli
