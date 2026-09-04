#include <filesystem>
#include <print>
#include <string>
#include <string_view>

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
}

} // namespace

auto wrap(int argc, char** argv) -> int;

auto build(int argc, char** argv) -> int {
    if (argc >= 1 && std::string_view{argv[0]} == "--profile") {
        return wrap(argc - 1, argv + 1);
    }
    if (argc < 1 || std::string_view{argv[0]} != "trace") {
        return usage("build --profile <command> [args...] | build trace [--chrome]");
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
