// SPDX-License-Identifier: MIT

#include "build/plan.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <ranges>
#include <set>
#include <sstream>
#include <string_view>

#include "build/cache.hpp"
#include "build/content.hpp"
#include "build/events.hpp"
#include "build/repository_access.hpp"
#include "liquid/repository.hpp"
#include "objects/records.hpp"

namespace liquid {
namespace {

struct configured_action {
    std::uint64_t key{};
    result_id result;
};

struct configured_build {
    std::vector<configured_action> actions;
};

auto plan_error(errc code, std::string message, const std::filesystem::path& path = {}) -> error {
    return {code, std::move(message), path.string()};
}

auto configuration_paths(repository& source) -> result<std::vector<std::filesystem::path>> {
    auto root = repository_detail::build_access::root_path(source);
    if (!root) {
        return std::unexpected(root.error());
    }
    auto output = std::vector<std::filesystem::path>{*root / ".liquid-build" / "configuration"};
    if (root->filename() == "repository") {
        output.push_back(root->parent_path() / "state");
    }
    return output;
}

auto read_configuration(repository& source) -> result<configured_build> {
    auto paths = configuration_paths(source);
    if (!paths) {
        return std::unexpected(paths.error());
    }
    const auto found = std::ranges::find_if(*paths, [](const std::filesystem::path& path) {
        return std::filesystem::is_regular_file(path);
    });
    if (found == paths->end()) {
        return std::unexpected(plan_error(
            errc::not_configured,
            "repository has no recorded build configuration; run liquid wrap <command> first",
            paths->front()));
    }
    auto input = std::ifstream{*found, std::ios::binary};
    if (!input) {
        return std::unexpected(plan_error(errc::io, "cannot read build configuration", *found));
    }
    auto output = configured_build{};
    for (auto line = std::string{}; std::getline(input, line);) {
        if (!line.starts_with("A\t")) {
            continue;
        }
        const auto key_end = line.find('\t', 2);
        if (key_end == std::string::npos) {
            return std::unexpected(
                plan_error(errc::corrupt, "invalid configured build action", *found));
        }
        auto key = std::uint64_t{};
        auto key_input = std::istringstream{line.substr(2, key_end - 2)};
        key_input >> std::hex >> key;
        auto id = parse_digest(std::string_view{line}.substr(key_end + 1));
        if (!key_input || !id) {
            return std::unexpected(
                plan_error(errc::corrupt, "invalid configured build action", *found));
        }
        output.actions.push_back({key, result_id{*id}});
    }
    if (!input.eof()) {
        return std::unexpected(plan_error(errc::io, "cannot read build configuration", *found));
    }
    if (output.actions.empty()) {
        return std::unexpected(
            plan_error(errc::not_configured,
                       "repository has no recorded build actions; run liquid wrap <command> first",
                       *found));
    }
    return output;
}

struct loaded_action {
    configured_action configured;
    action_result previous;
    objects::action_record record;
    std::vector<std::size_t> dependencies;
};

auto load_actions(repository& source) -> runtime::task<std::vector<loaded_action>> {
    auto configured = read_configuration(source);
    if (!configured) {
        co_return std::unexpected(configured.error());
    }
    auto output = std::vector<loaded_action>{};
    output.reserve(configured->actions.size());
    for (const auto& action : configured->actions) {
        auto result_bytes =
            co_await repository_detail::build_access::get(source, action.result.value);
        if (!result_bytes) {
            co_return std::unexpected(result_bytes.error());
        }
        auto previous = objects::decode_result(*result_bytes);
        if (!previous) {
            co_return std::unexpected(previous.error());
        }
        auto action_bytes = co_await repository_detail::build_access::get(source, previous->action);
        if (!action_bytes) {
            co_return std::unexpected(action_bytes.error());
        }
        auto record = objects::decode_action(*action_bytes);
        if (!record) {
            co_return std::unexpected(record.error());
        }
        output.push_back({action, std::move(*previous), std::move(*record), {}});
    }
    auto producers = std::map<std::string, std::size_t>{};
    for (auto index = std::size_t{}; index != output.size(); ++index) {
        for (const auto& item : output[index].previous.outputs) {
            const auto found = producers.find(item.path);
            if (found == producers.end() || output[index].previous.outputs.size() <
                                                output[found->second].previous.outputs.size()) {
                producers.insert_or_assign(item.path, index);
            }
        }
    }
    for (auto index = std::size_t{}; index != output.size(); ++index) {
        for (const auto& input : output[index].previous.manifest) {
            const auto producer = producers.find(input.path);
            if (producer == producers.end() || producer->second == index ||
                std::ranges::find(output[index].dependencies, producer->second) !=
                    output[index].dependencies.end()) {
                continue;
            }
            output[index].dependencies.push_back(producer->second);
        }
    }
    co_return output;
}

auto selected_actions(const std::vector<loaded_action>& actions, const build_target& target)
    -> result<std::vector<bool>> {
    auto selected = std::vector<bool>(actions.size(), target.name.empty());
    if (target.name.empty()) {
        return selected;
    }
    for (auto index = std::size_t{}; index != actions.size(); ++index) {
        if (std::ranges::any_of(actions[index].previous.outputs, [&](const action_output& output) {
                return output.path == target.name;
            })) {
            selected[index] = true;
        }
    }
    if (std::ranges::none_of(selected, [](bool value) { return value; })) {
        return std::unexpected(
            plan_error(errc::not_found, "build target is not recorded", target.name));
    }
    const auto include_dependencies = [&](const auto& self, std::size_t index) -> void {
        for (const auto dependency : actions[index].dependencies) {
            if (!selected[dependency]) {
                selected[dependency] = true;
                self(self, dependency);
            }
        }
    };
    for (auto index = std::size_t{}; index != actions.size(); ++index) {
        if (selected[index]) {
            include_dependencies(include_dependencies, index);
        }
    }
    return selected;
}

auto create_plan(repository& source, build_target target)
    -> runtime::task<std::vector<build_plan_action>> {
    auto actions = co_await load_actions(source);
    if (!actions) {
        co_return std::unexpected(actions.error());
    }
    auto selected = selected_actions(*actions, target);
    if (!selected) {
        co_return std::unexpected(selected.error());
    }
    auto root = co_await source.root_digest();
    if (!root) {
        co_return std::unexpected(root.error());
    }
    auto cache = build_detail::disk_cache{source};
    auto opened = co_await cache.open();
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    auto output = std::vector<build_plan_action>{};
    auto action_digests = std::vector<std::optional<digest>>(actions->size());
    for (auto index = std::size_t{}; index != actions->size(); ++index) {
        if (!(*selected)[index]) {
            continue;
        }
        auto& action = (*actions)[index];
        auto input = action.record.input;
        auto refreshed =
            co_await build_detail::refresh_dependency_input_tree(source, *root, input.inputs);
        if (!refreshed) {
            co_return std::unexpected(refreshed.error());
        }
        input.inputs = *refreshed;
        auto lookup = co_await cache.lookup(input, action.record.sandbox);
        if (!lookup) {
            co_return std::unexpected(lookup.error());
        }
        auto encoded = objects::encode(objects::action_record{input, action.record.sandbox});
        if (!encoded) {
            co_return std::unexpected(encoded.error());
        }
        auto action_id =
            co_await repository_detail::build_access::put_record(source, std::move(*encoded));
        if (!action_id) {
            co_return std::unexpected(action_id.error());
        }
        action_digests[index] = *action_id;
        output.push_back({.action = *action_id,
                          .input = std::move(input),
                          .state = lookup->hit ? cache_status::hit : cache_status::miss,
                          .result = lookup->hit});
    }
    auto output_index = std::size_t{};
    for (auto index = std::size_t{}; index != actions->size(); ++index) {
        if (!(*selected)[index]) {
            continue;
        }
        auto dependencies = std::vector<digest>{};
        for (const auto dependency : (*actions)[index].dependencies) {
            if ((*selected)[dependency] && action_digests[dependency]) {
                dependencies.push_back(*action_digests[dependency]);
            }
        }
        output[output_index].dependencies = std::move(dependencies);
        ++output_index;
    }
    co_return output;
}

auto json_string(std::string_view value) -> std::string {
    auto output = std::string{"\""};
    for (const auto character : value) {
        switch (character) {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
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
            if (static_cast<unsigned char>(character) < 0x20U) {
                output += std::format("\\u{:04x}", static_cast<unsigned char>(character));
            } else {
                output.push_back(character);
            }
        }
    }
    output.push_back('"');
    return output;
}

auto strings_json(const std::vector<std::string>& values) -> std::string {
    auto output = std::string{"["};
    for (auto index = std::size_t{}; index != values.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        output += json_string(values[index]);
    }
    output.push_back(']');
    return output;
}

auto environment_json(const std::map<std::string, std::string>& values) -> std::string {
    auto output = std::string{"{"};
    auto first = true;
    for (const auto& [name, value] : values) {
        if (!first) {
            output.push_back(',');
        }
        first = false;
        output += json_string(name) + ":" + json_string(value);
    }
    output.push_back('}');
    return output;
}

auto materialise(repository& source, const action_result& value) -> runtime::task<void> {
    for (const auto& output : value.outputs) {
        auto contents = co_await source.blob(output.blob);
        if (!contents) {
            co_return std::unexpected(contents.error());
        }
        auto written = co_await source.write(
            output.path, std::move(*contents), write_options{.mode = 0100000U | output.mode});
        if (!written) {
            co_return std::unexpected(written.error());
        }
    }
    co_return result<void>{};
}

auto record_materialised_outputs(repository& source, digest before) -> runtime::task<void> {
    auto after = co_await source.root_digest();
    if (!after) {
        co_return std::unexpected(after.error());
    }
    if (to_string(before) == to_string(*after)) {
        co_return result<void>{};
    }
    auto recorded = co_await source.cut("record build action outputs");
    if (!recorded) {
        co_return std::unexpected(recorded.error());
    }
    co_return result<void>{};
}

auto elapsed(std::chrono::steady_clock::time_point started) -> std::chrono::milliseconds {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 started);
}

runtime::task<void> run_repository_build(build_detail::event_producer producer,
                                         repository& source,
                                         build_target target) {
    auto observed = co_await producer.await_observer();
    if (!observed) {
        co_return std::unexpected(observed.error());
    }
    const auto started = std::chrono::steady_clock::now();
    auto original = co_await create_plan(source, target);
    if (!original) {
        producer.fail(original.error(), {.duration = elapsed(started)});
        co_return {};
    }
    auto summary = build_summary{};
    for (auto index = std::size_t{}; index != original->size(); ++index) {
        auto current = co_await create_plan(source, target);
        if (!current) {
            producer.fail(current.error(), summary);
            co_return {};
        }
        if (index >= current->size()) {
            producer.fail(plan_error(errc::corrupt, "configured build action disappeared"),
                          summary);
            co_return {};
        }
        const auto& found = (*current)[index];
        ++summary.actions;
        producer.publish({.kind = build_event_kind::action_queued, .action = found.action});
        producer.publish({.kind = build_event_kind::action_started,
                          .action = found.action,
                          .execution = action_execution{action_execution_kind::local, {}}});
        auto id = found.result;
        if (!id) {
            auto executed = co_await build::execute(source, found.input);
            if (!executed) {
                ++summary.failures;
                producer.fail(executed.error(), summary);
                co_return {};
            }
            id = *executed;
        }
        auto value = co_await build::result(source, *id);
        if (!value) {
            producer.fail(value.error(), summary);
            co_return {};
        }
        const auto effective_cache = found.result ? cache_status::hit : value->cache;
        auto terminal = build_event{.action = value->action, .result = *id};
        if (effective_cache == cache_status::hit) {
            terminal.kind = build_event_kind::action_hit;
            ++summary.hits;
        } else if (value->exit == 0) {
            terminal.kind = build_event_kind::action_done;
            terminal.outputs = value->outputs;
        } else {
            terminal.kind = build_event_kind::action_failed;
            terminal.diagnostics.push_back(
                {.severity = frontend_diagnostic_severity::error,
                 .message = std::format("action exited with status {}", value->exit)});
            ++summary.failures;
        }
        producer.publish(std::move(terminal));
        const auto duration = value->timing.queued + value->timing.sandbox + value->timing.exec +
                              value->timing.hash + value->timing.store;
        producer.publish({.kind = build_event_kind::critical_path_updated,
                          .critical_path = {{.result = *id,
                                             .action = value->action,
                                             .duration = duration,
                                             .timing = value->timing,
                                             .cache = effective_cache,
                                             .schedule = value->schedule}}});
        if (value->exit != 0) {
            summary.exit = value->exit;
            break;
        }
        auto before = co_await source.root_digest();
        if (!before) {
            producer.fail(before.error(), summary);
            co_return {};
        }
        auto written = co_await materialise(source, *value);
        if (!written) {
            producer.fail(written.error(), summary);
            co_return {};
        }
        auto recorded = co_await record_materialised_outputs(source, *before);
        if (!recorded) {
            producer.fail(recorded.error(), summary);
            co_return {};
        }
    }
    summary.duration = elapsed(started);
    producer.finish(summary);
    co_return {};
}

} // namespace

result<std::string> plan_json(const build_plan_action& action) {
    auto dependencies = std::string{"["};
    for (auto index = std::size_t{}; index != action.dependencies.size(); ++index) {
        if (index != 0) {
            dependencies.push_back(',');
        }
        dependencies += json_string(to_string(action.dependencies[index]));
    }
    dependencies.push_back(']');
    auto timeout = action.input.timeout ? std::to_string(action.input.timeout->count()) : "null";
    return std::format(
        "{{\"v\":{},\"action\":{},\"dependencies\":{},\"state\":{},\"result\":{},"
        "\"input\":{{\"command\":{},\"cwd\":{},\"env\":{},\"outputs\":{},\"inputs\":{},"
        "\"toolchain\":{},\"platform\":{{\"os\":{},\"arch\":{}}},\"timeout_s\":{},"
        "\"remote_eligible\":{}}}}}",
        action.v,
        json_string(to_string(action.action)),
        dependencies,
        json_string(action.state == cache_status::hit ? "hit" : "miss"),
        action.result ? json_string(to_string(action.result->value)) : "null",
        strings_json(action.input.command),
        json_string(action.input.cwd),
        environment_json(action.input.env),
        strings_json(action.input.outputs),
        json_string(to_string(action.input.inputs)),
        json_string(to_string(action.input.toolchain)),
        json_string(action.input.platform.os),
        json_string(action.input.platform.arch),
        timeout,
        action.input.remote_eligible ? "true" : "false");
}

namespace build_detail {

result<build_handle> start_repository(repository& source, build_target target) {
    auto stream = event_access::create();
    auto producer = stream.producer;
    runtime::spawn_serial(run_repository_build(producer, source, std::move(target)),
                          [producer](result<void> completed) {
                              if (!completed) {
                                  producer.fail(completed.error(), {});
                              }
                          });
    return std::move(stream.handle);
}

} // namespace build_detail

runtime::task<std::vector<build_plan_action>> repository::build_plan(build_target target) {
    co_return co_await create_plan(*this, std::move(target));
}

result<build_handle> repository::build(build_target target) {
    return build_detail::start_repository(*this, std::move(target));
}

} // namespace liquid
