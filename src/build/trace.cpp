#include "build/trace.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <set>

#include "build/repository_access.hpp"
#include "build/wrap.hpp"
#include "liquid/repository.hpp"
#include "objects/records.hpp"

namespace liquid {
namespace {

auto same_digest(const digest& left, const digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

auto duration(const action_timing& timing) -> std::chrono::milliseconds {
    return timing.queued + timing.sandbox + timing.exec + timing.hash + timing.store;
}

auto action_index(const build_trace& trace, const digest& action) -> std::optional<std::size_t> {
    for (auto index = std::size_t{}; index != trace.actions.size(); ++index) {
        if (same_digest(trace.actions[index].action, action)) {
            return index;
        }
    }
    return std::nullopt;
}

auto critical_path_to(const build_trace& trace, std::size_t target)
    -> std::vector<critical_action> {
    auto totals = std::vector<std::chrono::milliseconds>(trace.actions.size());
    auto previous = std::vector<std::optional<std::size_t>>(trace.actions.size());
    auto state = std::vector<std::uint8_t>(trace.actions.size());
    const auto visit = [&](const auto& self, std::size_t index) -> std::chrono::milliseconds {
        if (state[index] == 2) {
            return totals[index];
        }
        if (state[index] == 1) {
            return duration(trace.actions[index].timing);
        }
        state[index] = 1;
        auto best = std::chrono::milliseconds{};
        for (const auto& edge : trace.edges) {
            if (!same_digest(edge.dependent, trace.actions[index].action)) {
                continue;
            }
            const auto dependency = action_index(trace, edge.dependency);
            if (!dependency) {
                continue;
            }
            const auto candidate = self(self, *dependency);
            if (candidate > best) {
                best = candidate;
                previous[index] = *dependency;
            }
        }
        totals[index] = best + duration(trace.actions[index].timing);
        state[index] = 2;
        return totals[index];
    };
    static_cast<void>(visit(visit, target));
    auto indexes = std::vector<std::size_t>{};
    for (auto current = std::optional<std::size_t>{target}; current; current = previous[*current]) {
        indexes.push_back(*current);
    }
    std::ranges::reverse(indexes);
    auto output = std::vector<critical_action>{};
    output.reserve(indexes.size());
    for (const auto index : indexes) {
        const auto& action = trace.actions[index];
        output.push_back({action.result,
                          action.action,
                          duration(action.timing),
                          action.timing,
                          action.cache,
                          action.outputs});
    }
    return output;
}

auto global_critical_path(const build_trace& trace) -> std::vector<critical_action> {
    auto best = std::vector<critical_action>{};
    auto best_duration = std::chrono::milliseconds{};
    for (auto index = std::size_t{}; index != trace.actions.size(); ++index) {
        auto candidate = critical_path_to(trace, index);
        auto total = std::chrono::milliseconds{};
        for (const auto& action : candidate) {
            total += action.duration;
        }
        if (total > best_duration || best.empty()) {
            best = std::move(candidate);
            best_duration = total;
        }
    }
    return best;
}

auto json_string(std::string_view input) -> std::string {
    auto output = std::string{"\""};
    for (const auto character : input) {
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
            output.push_back(character);
            break;
        }
    }
    output.push_back('"');
    return output;
}

auto open_trace(std::filesystem::path working_tree,
                std::optional<std::filesystem::path> requested_cache)
    -> runtime::task<build_trace> {
    auto cache_directory = requested_cache ? result<std::filesystem::path>{*requested_cache}
                                           : build::wrap_cache_directory(working_tree);
    if (!cache_directory) {
        co_return std::unexpected(cache_directory.error());
    }
    const auto repository_path = *cache_directory / "repository";
    if (!std::filesystem::exists(repository_path / "LIQUID_REPO")) {
        co_return std::unexpected(
            error{errc::not_found, "no recorded build trace", working_tree.string()});
    }
    auto source = co_await repository::open(repository_path);
    if (!source) {
        co_return std::unexpected(source.error());
    }
    auto cache = build_detail::disk_cache{*source};
    auto opened = co_await cache.open();
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    if (cache.traces().empty()) {
        co_return std::unexpected(
            error{errc::not_found, "no recorded build trace", working_tree.string()});
    }
    const auto id = cache.traces().back();
    auto bytes = co_await repository_detail::build_access::get(*source, id);
    if (!bytes) {
        co_return std::unexpected(bytes.error());
    }
    auto trace = objects::decode_build_trace(*bytes);
    if (!trace) {
        co_return std::unexpected(trace.error());
    }
    trace->id = id;
    co_return *trace;
}

} // namespace

namespace build_detail {

runtime::task<void> record_trace(repository& source, disk_cache& cache, wrap_report& report) {
    auto trace = build_trace{
        .wall_time = report.wall_time, .actions = report.trace_actions, .cache = report.cache};
    auto producers = std::map<std::string, std::size_t>{};
    for (auto index = std::size_t{}; index != trace.actions.size(); ++index) {
        for (const auto& output : trace.actions[index].outputs) {
            const auto found = producers.find(output);
            if (found == producers.end() ||
                trace.actions[index].outputs.size() < trace.actions[found->second].outputs.size()) {
                producers.insert_or_assign(output, index);
            }
        }
    }
    auto unique_edges = std::set<std::pair<std::string, std::string>>{};
    for (const auto& action : trace.actions) {
        auto result_bytes =
            co_await repository_detail::build_access::get(source, action.result.value);
        if (!result_bytes) {
            co_return std::unexpected(result_bytes.error());
        }
        auto result = objects::decode_result(*result_bytes);
        if (!result) {
            co_return std::unexpected(result.error());
        }
        for (const auto& input : result->manifest) {
            const auto found = producers.find(input.path);
            if (found == producers.end()) {
                continue;
            }
            const auto& dependency = trace.actions[found->second];
            if (same_digest(dependency.action, action.action)) {
                continue;
            }
            const auto key = std::pair{to_string(dependency.action), to_string(action.action)};
            if (unique_edges.insert(key).second) {
                trace.edges.push_back({dependency.action, action.action});
            }
        }
    }
    auto encoded = objects::encode(trace);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    auto id = co_await repository_detail::build_access::put_record(source, std::move(*encoded));
    if (!id) {
        co_return std::unexpected(id.error());
    }
    auto retained = co_await cache.record_trace(*id);
    if (!retained) {
        co_return std::unexpected(retained.error());
    }
    trace.id = *id;
    report.trace = *id;
    report.critical_path = global_critical_path(trace);
    auto execution = std::chrono::milliseconds{};
    for (const auto& action : trace.actions) {
        report.phase_totals.queued += action.timing.queued;
        report.phase_totals.sandbox += action.timing.sandbox;
        report.phase_totals.exec += action.timing.exec;
        report.phase_totals.hash += action.timing.hash;
        report.phase_totals.store += action.timing.store;
        execution += action.timing.exec;
    }
    if (report.wall_time.count() > 0) {
        report.parallelism =
            static_cast<double>(execution.count()) / static_cast<double>(report.wall_time.count());
    }
    co_return result<void>{};
}

} // namespace build_detail

namespace build {

runtime::task<build_trace> latest_trace(std::filesystem::path working_tree,
                                        std::optional<std::filesystem::path> cache_directory) {
    co_return co_await open_trace(std::move(working_tree), std::move(cache_directory));
}

runtime::task<why_slow_report> why_slow(std::filesystem::path working_tree,
                                        std::string target,
                                        std::optional<std::filesystem::path> cache_directory) {
    auto trace = co_await open_trace(std::move(working_tree), std::move(cache_directory));
    if (!trace) {
        co_return std::unexpected(trace.error());
    }
    auto selected = std::optional<std::size_t>{};
    auto parsed = parse_digest(target);
    for (auto index = std::size_t{}; index != trace->actions.size(); ++index) {
        const auto& action = trace->actions[index];
        const auto digest_matches = parsed && (same_digest(action.action, *parsed) ||
                                               same_digest(action.result.value, *parsed));
        const auto output_matches =
            std::ranges::find(action.outputs, target) != action.outputs.end();
        if (!digest_matches && !output_matches) {
            continue;
        }
        if (!selected || action.outputs.size() < trace->actions[*selected].outputs.size()) {
            selected = index;
        }
    }
    if (!selected) {
        co_return std::unexpected(
            error{errc::not_found, "target is absent from build trace", target});
    }
    auto path = critical_path_to(*trace, *selected);
    auto total = std::chrono::milliseconds{};
    for (const auto& action : path) {
        total += action.duration;
    }
    co_return why_slow_report{std::move(target), trace->id, total, std::move(path)};
}

liquid::result<std::string> chrome_trace(const build_trace& trace) {
    auto output = std::string{"{\"traceEvents\":["};
    auto first = true;
    const auto append_event = [&](std::string_view name,
                                  std::size_t thread,
                                  std::chrono::milliseconds start,
                                  std::chrono::milliseconds elapsed,
                                  const digest& action) {
        if (!first) {
            output.push_back(',');
        }
        first = false;
        output += std::format("{{\"name\":{},\"cat\":\"build\",\"ph\":\"X\",\"ts\":{},\"dur\":{},"
                              "\"pid\":1,\"tid\":{},\"args\":{{\"action\":{}}}}}",
                              json_string(name),
                              start.count() * 1000,
                              elapsed.count() * 1000,
                              thread,
                              json_string(to_string(action)));
    };
    for (auto index = std::size_t{}; index != trace.actions.size(); ++index) {
        const auto& action = trace.actions[index];
        auto offset = action.start_offset;
        const auto phase = [&](std::string_view name, std::chrono::milliseconds elapsed) {
            append_event(name, index + 1, offset, elapsed, action.action);
            offset += elapsed;
        };
        phase("queued", action.timing.queued);
        phase("sandbox", action.timing.sandbox);
        phase("exec", action.timing.exec);
        phase("hash", action.timing.hash);
        phase("store", action.timing.store);
    }
    output += "]}";
    return output;
}

} // namespace build
} // namespace liquid
