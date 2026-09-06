#include "liquid/daemon.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <ranges>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "build/events.hpp"
#include "build/wrap.hpp"
#include "daemon/fanout.hpp"
#include "encoding/cbor.hpp"
#include "flow_control.hpp"
#include "liquid/drive.hpp"
#include "liquid/edit.hpp"
#include "liquid/graph.hpp"
#include "liquid/hash.hpp"
#include "liquid/hot_update.hpp"
#include "liquid/repository.hpp"
#include "search/index.hpp"

namespace liquid {
namespace {

namespace cbor = encoding::cbor;

constexpr auto maximum_frame = std::uint32_t{16U * 1024U * 1024U};

auto failure(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto daemon_checkout(std::filesystem::path checkout) -> result<std::filesystem::path> {
    checkout = std::filesystem::weakly_canonical(std::filesystem::absolute(checkout));
    if (std::filesystem::is_regular_file(checkout / "LIQUID_REPO")) {
        return checkout;
    }
    auto cache = build::wrap_cache_directory(checkout);
    if (!cache) {
        return std::unexpected(cache.error());
    }
    const auto repository = *cache / "repository";
    if (!std::filesystem::is_regular_file(repository / "LIQUID_REPO")) {
        return std::unexpected(failure(
            errc::not_configured,
            "repository has no recorded build configuration; run liquid wrap <command> first",
            checkout.string()));
    }
    return repository;
}

auto error_code(std::string_view name) -> errc {
    for (const auto code : {errc::ok,
                            errc::not_found,
                            errc::already_exists,
                            errc::conflict,
                            errc::invalid_argument,
                            errc::unsupported,
                            errc::corrupt,
                            errc::io,
                            errc::locked,
                            errc::unauthorized,
                            errc::unavailable,
                            errc::busy,
                            errc::internal,
                            errc::not_configured}) {
        if (to_string(code) == name) {
            return code;
        }
    }
    return errc::internal;
}

auto map(std::initializer_list<std::pair<cbor::value, cbor::value>> fields) -> cbor::value {
    return cbor::value{cbor::value::map{fields}};
}

auto presence_value(const liquid::presence& value) -> cbor::value {
    auto range = cbor::value{cbor::null{}};
    if (value.range) {
        range = map({{"start_byte", value.range->start.byte},
                     {"start_line", value.range->start.line},
                     {"start_column", value.range->start.column},
                     {"end_byte", value.range->end.byte},
                     {"end_line", value.range->end.line},
                     {"end_column", value.range->end.column}});
    }
    return map({{"actor", value.actor.taap_id},
                {"session", to_string(value.session)},
                {"path", value.path},
                {"range", std::move(range)},
                {"activity",
                 value.activity == presence_activity::editing   ? cbor::value{"editing"}
                 : value.activity == presence_activity::reading ? cbor::value{"reading"}
                                                                : cbor::value{"idle"}},
                {"at", static_cast<std::uint64_t>(value.at.time_since_epoch().count())}});
}

auto field(const cbor::value& source, std::string_view name) -> const cbor::value* {
    const auto* fields = std::get_if<cbor::value::map>(&source.data);
    if (!fields) {
        return nullptr;
    }
    for (const auto& [key, value] : *fields) {
        const auto* text = std::get_if<std::string>(&key.data);
        if (text && *text == name) {
            return &value;
        }
    }
    return nullptr;
}

auto text_field(const cbor::value& source, std::string_view name) -> const std::string* {
    const auto* value = field(source, name);
    return value ? std::get_if<std::string>(&value->data) : nullptr;
}

auto unsigned_field(const cbor::value& source, std::string_view name) -> const std::uint64_t* {
    const auto* value = field(source, name);
    return value ? std::get_if<std::uint64_t>(&value->data) : nullptr;
}

auto bool_field(const cbor::value& source, std::string_view name) -> const bool* {
    const auto* value = field(source, name);
    return value ? std::get_if<bool>(&value->data) : nullptr;
}

auto decode_presence_value(const cbor::value& source) -> result<liquid::presence> {
    const auto* actor = text_field(source, "actor");
    const auto* session_text = text_field(source, "session");
    const auto* path = text_field(source, "path");
    const auto* activity = text_field(source, "activity");
    const auto* at = unsigned_field(source, "at");
    const auto* range_value = field(source, "range");
    if (!actor || actor->empty() || !session_text || !path || path->empty() || !activity ||
        (*activity != "editing" && *activity != "reading" && *activity != "idle") || !at ||
        !range_value) {
        return std::unexpected(failure(errc::invalid_argument, "invalid presence report"));
    }
    auto session = parse_session_id(*session_text);
    if (!session) {
        return std::unexpected(session.error());
    }
    auto output = liquid::presence{.actor = {.taap_id = *actor},
                                   .session = *session,
                                   .path = *path,
                                   .activity = *activity == "editing"   ? presence_activity::editing
                                               : *activity == "reading" ? presence_activity::reading
                                                                        : presence_activity::idle,
                                   .at = std::chrono::sys_seconds{std::chrono::seconds{*at}}};
    if (!std::holds_alternative<cbor::null>(range_value->data)) {
        const auto* start_byte = unsigned_field(*range_value, "start_byte");
        const auto* start_line = unsigned_field(*range_value, "start_line");
        const auto* start_column = unsigned_field(*range_value, "start_column");
        const auto* end_byte = unsigned_field(*range_value, "end_byte");
        const auto* end_line = unsigned_field(*range_value, "end_line");
        const auto* end_column = unsigned_field(*range_value, "end_column");
        if (!start_byte || !start_line || !start_column || !end_byte || !end_line || !end_column ||
            *start_byte > *end_byte) {
            return std::unexpected(failure(errc::invalid_argument, "invalid presence range"));
        }
        output.range = frontend_source_range{{*start_byte, *start_line, *start_column},
                                             {*end_byte, *end_line, *end_column}};
    }
    return output;
}

auto attribution_value(std::span<const line_attribution> values) -> cbor::value {
    auto lines = cbor::value::array{};
    lines.reserve(values.size());
    for (const auto& value : values) {
        lines.push_back(map({{"line", value.line},
                             {"text", value.text},
                             {"actor", value.by.taap_id},
                             {"session", to_string(value.session)},
                             {"change", to_string(value.change.value)},
                             {"agent", value.agent}}));
    }
    return cbor::value{std::move(lines)};
}

auto decode_attribution_value(const cbor::value& source) -> result<std::vector<line_attribution>> {
    const auto* values = std::get_if<cbor::value::array>(&source.data);
    if (!values) {
        return std::unexpected(failure(errc::corrupt, "invalid daemon attribution response"));
    }
    auto output = std::vector<line_attribution>{};
    output.reserve(values->size());
    for (const auto& value : *values) {
        const auto* line = unsigned_field(value, "line");
        const auto* text = text_field(value, "text");
        const auto* actor = text_field(value, "actor");
        const auto* session_text = text_field(value, "session");
        const auto* change_text = text_field(value, "change");
        const auto* agent = bool_field(value, "agent");
        if (!line || *line == 0 || !text || !actor || !session_text || !change_text || !agent) {
            return std::unexpected(failure(errc::corrupt, "invalid daemon attribution line"));
        }
        auto session = parse_session_id(*session_text);
        auto change = parse_digest(*change_text);
        if (!session || !change) {
            return std::unexpected(session ? change.error() : session.error());
        }
        output.push_back({.line = *line,
                          .text = *text,
                          .by = {.taap_id = *actor},
                          .session = *session,
                          .change = {*change},
                          .agent = *agent});
    }
    return output;
}

auto strings_value(const std::vector<std::string>& values) -> cbor::value {
    auto output = cbor::value::array{};
    output.reserve(values.size());
    for (const auto& value : values) {
        output.emplace_back(value);
    }
    return cbor::value{std::move(output)};
}

auto flow_scope_name(flow_scope scope) -> std::string_view {
    switch (scope) {
    case flow_scope::actor:
        return "actor";
    case flow_scope::document:
        return "document";
    case flow_scope::repository:
        return "repository";
    }
    return "repository";
}

auto flow_reason_name(flow_pause_reason reason) -> std::string_view {
    switch (reason) {
    case flow_pause_reason::none:
        return "none";
    case flow_pause_reason::persistence:
        return "persistence";
    case flow_pause_reason::consumers:
        return "consumers";
    }
    return "none";
}

auto decode_strings(const cbor::value& source, std::string_view name)
    -> result<std::vector<std::string>> {
    const auto* value = field(source, name);
    const auto* array = value ? std::get_if<cbor::value::array>(&value->data) : nullptr;
    if (!array) {
        return std::unexpected(failure(errc::corrupt, "invalid daemon string array"));
    }
    auto output = std::vector<std::string>{};
    output.reserve(array->size());
    for (const auto& item : *array) {
        const auto* text = std::get_if<std::string>(&item.data);
        if (!text) {
            return std::unexpected(failure(errc::corrupt, "invalid daemon string value"));
        }
        output.push_back(*text);
    }
    return output;
}

auto action_input_value(const action_input& input) -> cbor::value {
    auto environment = cbor::value::array{};
    for (const auto& [name, value] : input.env) {
        environment.push_back(map({{"name", name}, {"value", value}}));
    }
    return map(
        {{"command", strings_value(input.command)},
         {"cwd", input.cwd},
         {"environment", cbor::value{std::move(environment)}},
         {"outputs", strings_value(input.outputs)},
         {"inputs", to_string(input.inputs)},
         {"toolchain", to_string(input.toolchain)},
         {"os", input.platform.os},
         {"arch", input.platform.arch},
         {"timeout",
          input.timeout ? static_cast<std::uint64_t>(input.timeout->count()) : std::uint64_t{}},
         {"has_timeout", input.timeout.has_value()},
         {"remote_eligible", input.remote_eligible}});
}

auto decode_action_input(const cbor::value& source) -> result<action_input> {
    auto command = decode_strings(source, "command");
    auto outputs = decode_strings(source, "outputs");
    const auto* cwd = text_field(source, "cwd");
    const auto* environment_value = field(source, "environment");
    const auto* environment =
        environment_value ? std::get_if<cbor::value::array>(&environment_value->data) : nullptr;
    const auto* inputs_text = text_field(source, "inputs");
    const auto* toolchain_text = text_field(source, "toolchain");
    const auto* os = text_field(source, "os");
    const auto* arch = text_field(source, "arch");
    const auto* timeout = unsigned_field(source, "timeout");
    const auto* has_timeout = bool_field(source, "has_timeout");
    const auto* remote_eligible = bool_field(source, "remote_eligible");
    if (!command || !outputs || !cwd || !environment || !inputs_text || !toolchain_text || !os ||
        !arch || !timeout || !has_timeout || !remote_eligible) {
        return std::unexpected(failure(errc::corrupt, "invalid daemon build input"));
    }
    auto inputs = parse_digest(*inputs_text);
    auto toolchain = parse_digest(*toolchain_text);
    if (!inputs || !toolchain) {
        return std::unexpected(failure(errc::corrupt, "invalid daemon build digest"));
    }
    auto output = action_input{.command = std::move(*command),
                               .cwd = *cwd,
                               .outputs = std::move(*outputs),
                               .inputs = *inputs,
                               .toolchain = *toolchain,
                               .platform = {*os, *arch},
                               .remote_eligible = *remote_eligible};
    if (*has_timeout) {
        output.timeout = std::chrono::seconds{*timeout};
    }
    for (const auto& item : *environment) {
        const auto* name = text_field(item, "name");
        const auto* value = text_field(item, "value");
        if (!name || !value) {
            return std::unexpected(failure(errc::corrupt, "invalid daemon build environment"));
        }
        output.env.emplace(*name, *value);
    }
    return output;
}

auto build_target_value(const build_target& target) -> cbor::value {
    return map({{"name", target.name}});
}

auto decode_build_target(const cbor::value& source) -> result<build_target> {
    const auto* name = text_field(source, "name");
    if (!name) {
        return std::unexpected(failure(errc::corrupt, "invalid daemon build target"));
    }
    return build_target{*name};
}

auto build_plan_input_value(const action_input& input) -> cbor::value {
    auto environment = cbor::value::map{};
    for (const auto& [name, value] : input.env) {
        environment.emplace_back(name, value);
    }
    return map({{"command", strings_value(input.command)},
                {"cwd", input.cwd},
                {"env", cbor::value{std::move(environment)}},
                {"outputs", strings_value(input.outputs)},
                {"inputs", to_string(input.inputs)},
                {"toolchain", to_string(input.toolchain)},
                {"platform", map({{"os", input.platform.os}, {"arch", input.platform.arch}})},
                {"timeout_s",
                 input.timeout ? cbor::value{static_cast<std::uint64_t>(input.timeout->count())}
                               : cbor::value{cbor::null{}}},
                {"remote_eligible", input.remote_eligible}});
}

auto decode_build_plan_input(const cbor::value& source) -> result<action_input> {
    auto command = decode_strings(source, "command");
    auto outputs = decode_strings(source, "outputs");
    const auto* cwd = text_field(source, "cwd");
    const auto* environment_value = field(source, "env");
    const auto* environment =
        environment_value ? std::get_if<cbor::value::map>(&environment_value->data) : nullptr;
    const auto* inputs_text = text_field(source, "inputs");
    const auto* toolchain_text = text_field(source, "toolchain");
    const auto* platform = field(source, "platform");
    const auto* os = platform ? text_field(*platform, "os") : nullptr;
    const auto* arch = platform ? text_field(*platform, "arch") : nullptr;
    const auto* timeout_value = field(source, "timeout_s");
    const auto* remote_eligible = bool_field(source, "remote_eligible");
    if (!command || !outputs || !cwd || !environment || !inputs_text || !toolchain_text || !os ||
        !arch || !timeout_value || !remote_eligible) {
        return std::unexpected(failure(errc::corrupt, "invalid daemon build plan input"));
    }
    auto inputs = parse_digest(*inputs_text);
    auto toolchain = parse_digest(*toolchain_text);
    if (!inputs || !toolchain) {
        return std::unexpected(failure(errc::corrupt, "invalid daemon build plan digest"));
    }
    auto output = action_input{.command = std::move(*command),
                               .cwd = *cwd,
                               .outputs = std::move(*outputs),
                               .inputs = *inputs,
                               .toolchain = *toolchain,
                               .platform = {*os, *arch},
                               .remote_eligible = *remote_eligible};
    for (const auto& [name_value, value_value] : *environment) {
        const auto* name = std::get_if<std::string>(&name_value.data);
        const auto* value = std::get_if<std::string>(&value_value.data);
        if (!name || !value) {
            return std::unexpected(failure(errc::corrupt, "invalid daemon build plan environment"));
        }
        output.env.emplace(*name, *value);
    }
    if (const auto* timeout = std::get_if<std::uint64_t>(&timeout_value->data)) {
        output.timeout = std::chrono::seconds{*timeout};
    } else if (!std::holds_alternative<cbor::null>(timeout_value->data)) {
        return std::unexpected(failure(errc::corrupt, "invalid daemon build plan timeout"));
    }
    return output;
}

auto build_plan_action_value(const build_plan_action& action) -> cbor::value {
    auto dependencies = cbor::value::array{};
    dependencies.reserve(action.dependencies.size());
    for (const auto& dependency : action.dependencies) {
        dependencies.emplace_back(to_string(dependency));
    }
    return map(
        {{"v", action.v},
         {"action", to_string(action.action)},
         {"dependencies", cbor::value{std::move(dependencies)}},
         {"state", action.state == cache_status::hit ? "hit" : "miss"},
         {"result",
          action.result ? cbor::value{to_string(action.result->value)} : cbor::value{cbor::null{}}},
         {"input", build_plan_input_value(action.input)}});
}

auto decode_build_plan_action(const cbor::value& source) -> result<build_plan_action> {
    const auto* version = unsigned_field(source, "v");
    const auto* action_text = text_field(source, "action");
    const auto* dependencies_value = field(source, "dependencies");
    const auto* dependencies =
        dependencies_value ? std::get_if<cbor::value::array>(&dependencies_value->data) : nullptr;
    const auto* state = text_field(source, "state");
    const auto* result_value = field(source, "result");
    const auto* input_value = field(source, "input");
    if (!version || *version != 1 || !action_text || !dependencies || !state ||
        (*state != "hit" && *state != "miss") || !result_value || !input_value) {
        return std::unexpected(failure(errc::corrupt, "invalid daemon build plan action"));
    }
    auto action = parse_digest(*action_text);
    auto input = decode_build_plan_input(*input_value);
    if (!action || !input) {
        return std::unexpected(action ? input.error() : action.error());
    }
    auto output =
        build_plan_action{.action = *action,
                          .input = std::move(*input),
                          .state = *state == "hit" ? cache_status::hit : cache_status::miss};
    for (const auto& item : *dependencies) {
        const auto* text = std::get_if<std::string>(&item.data);
        if (!text) {
            return std::unexpected(failure(errc::corrupt, "invalid daemon build plan dependency"));
        }
        auto dependency = parse_digest(*text);
        if (!dependency) {
            return std::unexpected(dependency.error());
        }
        output.dependencies.push_back(*dependency);
    }
    if (const auto* result_text = std::get_if<std::string>(&result_value->data)) {
        auto result = parse_digest(*result_text);
        if (!result) {
            return std::unexpected(result.error());
        }
        output.result = result_id{*result};
    } else if (!std::holds_alternative<cbor::null>(result_value->data)) {
        return std::unexpected(failure(errc::corrupt, "invalid daemon build plan result"));
    }
    return output;
}

auto build_plan_value(const std::vector<build_plan_action>& plan) -> cbor::value {
    auto actions = cbor::value::array{};
    actions.reserve(plan.size());
    for (const auto& action : plan) {
        actions.push_back(build_plan_action_value(action));
    }
    return cbor::value{std::move(actions)};
}

auto decode_build_plan(const cbor::value& source) -> result<std::vector<build_plan_action>> {
    const auto* actions = std::get_if<cbor::value::array>(&source.data);
    if (!actions) {
        return std::unexpected(failure(errc::corrupt, "invalid daemon build plan"));
    }
    auto output = std::vector<build_plan_action>{};
    output.reserve(actions->size());
    for (const auto& item : *actions) {
        auto action = decode_build_plan_action(item);
        if (!action) {
            return std::unexpected(action.error());
        }
        output.push_back(std::move(*action));
    }
    return output;
}

auto build_event_value(const build_event& event) -> cbor::value {
    auto outputs = cbor::value::array{};
    for (const auto& output : event.outputs) {
        outputs.push_back(map({{"path", output.path},
                               {"mode", static_cast<std::uint64_t>(output.mode)},
                               {"blob", to_string(output.blob)}}));
    }
    auto diagnostics = cbor::value::array{};
    for (const auto& diagnostic : event.diagnostics) {
        diagnostics.push_back(map({{"file", diagnostic.file},
                                   {"line", diagnostic.line},
                                   {"column", diagnostic.column},
                                   {"severity", static_cast<std::uint64_t>(diagnostic.severity)},
                                   {"message", diagnostic.message}}));
    }
    auto critical_path = cbor::value::array{};
    for (const auto& action : event.critical_path) {
        critical_path.push_back(
            map({{"action", to_string(action.action)},
                 {"result", to_string(action.result.value)},
                 {"duration", static_cast<std::uint64_t>(action.duration.count())},
                 {"cache", static_cast<std::uint64_t>(action.cache)}}));
    }
    auto execution = cbor::value{};
    if (event.execution) {
        execution = map({{"kind", static_cast<std::uint64_t>(event.execution->kind)},
                         {"worker", event.execution->worker}});
    }
    auto test = cbor::value{};
    if (event.test) {
        test = map({{"name", event.test->name},
                    {"status", static_cast<std::uint64_t>(event.test->status)},
                    {"output", event.test->output},
                    {"duration", static_cast<std::uint64_t>(event.test->duration.count())},
                    {"hit", event.test->hit}});
    }
    auto summary = cbor::value{};
    if (event.summary) {
        summary = map({{"exit", static_cast<std::uint64_t>(event.summary->exit)},
                       {"actions", event.summary->actions},
                       {"hits", event.summary->hits},
                       {"failures", event.summary->failures},
                       {"duration", static_cast<std::uint64_t>(event.summary->duration.count())}});
    }
    return map({{"sequence", event.sequence},
                {"kind", static_cast<std::uint64_t>(event.kind)},
                {"action", event.action ? to_string(*event.action) : ""},
                {"result", event.result ? to_string(event.result->value) : ""},
                {"execution", std::move(execution)},
                {"outputs", cbor::value{std::move(outputs)}},
                {"diagnostics", cbor::value{std::move(diagnostics)}},
                {"critical_path", cbor::value{std::move(critical_path)}},
                {"test", std::move(test)},
                {"summary", std::move(summary)}});
}

auto parse_event_digest(const std::string& text, std::string_view field_name)
    -> result<std::optional<digest>> {
    if (text.empty()) {
        return std::optional<digest>{};
    }
    auto parsed = parse_digest(text);
    if (!parsed) {
        return std::unexpected(
            failure(errc::corrupt, "invalid daemon build event digest", std::string{field_name}));
    }
    return std::optional<digest>{*parsed};
}

auto decode_build_event(const cbor::value& source) -> result<build_event> {
    const auto* sequence = unsigned_field(source, "sequence");
    const auto* kind = unsigned_field(source, "kind");
    const auto* action_text = text_field(source, "action");
    const auto* result_text = text_field(source, "result");
    const auto* execution_value = field(source, "execution");
    const auto* outputs_value = field(source, "outputs");
    const auto* diagnostics_value = field(source, "diagnostics");
    const auto* critical_value = field(source, "critical_path");
    const auto* test_value = field(source, "test");
    const auto* summary_value = field(source, "summary");
    const auto* outputs =
        outputs_value ? std::get_if<cbor::value::array>(&outputs_value->data) : nullptr;
    const auto* diagnostics =
        diagnostics_value ? std::get_if<cbor::value::array>(&diagnostics_value->data) : nullptr;
    const auto* critical =
        critical_value ? std::get_if<cbor::value::array>(&critical_value->data) : nullptr;
    if (!sequence || !kind ||
        *kind > static_cast<std::uint64_t>(build_event_kind::build_finished) || !action_text ||
        !result_text || !execution_value || !outputs || !diagnostics || !critical || !test_value ||
        !summary_value) {
        return std::unexpected(failure(errc::corrupt, "invalid daemon build event"));
    }
    auto action = parse_event_digest(*action_text, "action");
    auto result_value = parse_event_digest(*result_text, "result");
    if (!action || !result_value) {
        return std::unexpected(action ? result_value.error() : action.error());
    }
    auto event = build_event{
        .kind = static_cast<build_event_kind>(*kind), .sequence = *sequence, .action = *action};
    if (*result_value) {
        event.result = result_id{**result_value};
    }
    if (!std::holds_alternative<cbor::null>(execution_value->data)) {
        const auto* execution_kind = unsigned_field(*execution_value, "kind");
        const auto* worker = text_field(*execution_value, "worker");
        if (!execution_kind || *execution_kind > 1 || !worker) {
            return std::unexpected(failure(errc::corrupt, "invalid daemon build execution"));
        }
        event.execution =
            action_execution{static_cast<action_execution_kind>(*execution_kind), *worker};
    }
    for (const auto& item : *outputs) {
        const auto* path = text_field(item, "path");
        const auto* mode = unsigned_field(item, "mode");
        const auto* blob_text = text_field(item, "blob");
        if (!path || !mode || !blob_text) {
            return std::unexpected(failure(errc::corrupt, "invalid daemon build output"));
        }
        auto blob = parse_digest(*blob_text);
        if (!blob) {
            return std::unexpected(blob.error());
        }
        event.outputs.push_back({*path, static_cast<std::uint32_t>(*mode), *blob});
    }
    for (const auto& item : *diagnostics) {
        const auto* file = text_field(item, "file");
        const auto* line = unsigned_field(item, "line");
        const auto* column = unsigned_field(item, "column");
        const auto* severity = unsigned_field(item, "severity");
        const auto* message = text_field(item, "message");
        if (!file || !line || !column || !severity || *severity > 3 || !message) {
            return std::unexpected(failure(errc::corrupt, "invalid daemon build diagnostic"));
        }
        event.diagnostics.push_back({*file,
                                     *line,
                                     *column,
                                     static_cast<frontend_diagnostic_severity>(*severity),
                                     *message});
    }
    for (const auto& item : *critical) {
        const auto* action_value = text_field(item, "action");
        const auto* result_id_value = text_field(item, "result");
        const auto* duration = unsigned_field(item, "duration");
        const auto* cache = unsigned_field(item, "cache");
        if (!action_value || !result_id_value || !duration || !cache || *cache > 2) {
            return std::unexpected(failure(errc::corrupt, "invalid daemon critical path"));
        }
        auto action_id = parse_digest(*action_value);
        auto result_id_digest = parse_digest(*result_id_value);
        if (!action_id || !result_id_digest) {
            return std::unexpected(failure(errc::corrupt, "invalid daemon critical path digest"));
        }
        event.critical_path.push_back({.result = {std::move(*result_id_digest)},
                                       .action = std::move(*action_id),
                                       .duration = std::chrono::milliseconds{*duration},
                                       .cache = static_cast<cache_status>(*cache)});
    }
    if (!std::holds_alternative<cbor::null>(test_value->data)) {
        const auto* name = text_field(*test_value, "name");
        const auto* status = unsigned_field(*test_value, "status");
        const auto* output = text_field(*test_value, "output");
        const auto* duration = unsigned_field(*test_value, "duration");
        const auto* hit = bool_field(*test_value, "hit");
        if (!name || !status || *status > 1 || !output || !duration || !hit) {
            return std::unexpected(failure(errc::corrupt, "invalid daemon test result"));
        }
        event.test = test_result{*name,
                                 static_cast<test_status>(*status),
                                 *output,
                                 std::chrono::milliseconds{*duration},
                                 *hit};
    }
    if (!std::holds_alternative<cbor::null>(summary_value->data)) {
        const auto* exit = unsigned_field(*summary_value, "exit");
        const auto* actions = unsigned_field(*summary_value, "actions");
        const auto* hits = unsigned_field(*summary_value, "hits");
        const auto* failures = unsigned_field(*summary_value, "failures");
        const auto* duration = unsigned_field(*summary_value, "duration");
        if (!exit || !actions || !hits || !failures || !duration) {
            return std::unexpected(failure(errc::corrupt, "invalid daemon build summary"));
        }
        event.summary = build_summary{static_cast<int>(*exit),
                                      *actions,
                                      *hits,
                                      *failures,
                                      std::chrono::milliseconds{*duration}};
    }
    return event;
}

auto validation_value(const graph_validation_record& record) -> cbor::value {
    auto inputs = cbor::value::array{};
    for (const auto& input : record.inputs) {
        if (const auto* key = std::get_if<graph_input_key>(&input.key)) {
            inputs.push_back(map({{"type", "input"},
                                  {"kind", static_cast<std::uint64_t>(key->kind)},
                                  {"name", key->name},
                                  {"fingerprint", to_string(input.fingerprint)}}));
            continue;
        }
        const auto& key = std::get<query_key>(input.key);
        inputs.push_back(
            map({{"type", "query"},
                 {"kind", key.kind},
                 {"arguments", cbor::value::bytes{key.arguments.begin(), key.arguments.end()}},
                 {"fingerprint", to_string(input.fingerprint)}}));
    }
    return map({{"id", to_string(record.id)},
                {"query_kind", record.query.kind},
                {"query_arguments",
                 cbor::value::bytes{record.query.arguments.begin(), record.query.arguments.end()}},
                {"inputs", cbor::value{std::move(inputs)}},
                {"incremental", to_string(record.incremental)},
                {"clean", to_string(record.clean)},
                {"agreement", record.agreement},
                {"at", static_cast<std::uint64_t>(record.at.time_since_epoch().count())}});
}

auto decode_validation(const cbor::value& value) -> result<graph_validation_record> {
    const auto* id_text = text_field(value, "id");
    const auto* query_kind = text_field(value, "query_kind");
    const auto* query_arguments_value = field(value, "query_arguments");
    const auto* inputs_value = field(value, "inputs");
    const auto* incremental_text = text_field(value, "incremental");
    const auto* clean_text = text_field(value, "clean");
    const auto* agreement = bool_field(value, "agreement");
    const auto* at = unsigned_field(value, "at");
    const auto* query_arguments =
        query_arguments_value ? std::get_if<cbor::value::bytes>(&query_arguments_value->data)
                              : nullptr;
    const auto* inputs =
        inputs_value ? std::get_if<cbor::value::array>(&inputs_value->data) : nullptr;
    if (!id_text || !query_kind || !query_arguments || !inputs || !incremental_text ||
        !clean_text || !agreement || !at) {
        return std::unexpected(failure(errc::corrupt, "invalid daemon validation status"));
    }
    auto id = parse_digest(*id_text);
    auto incremental = parse_digest(*incremental_text);
    auto clean = parse_digest(*clean_text);
    if (!id || !incremental || !clean) {
        return std::unexpected(failure(errc::corrupt, "invalid daemon validation digest"));
    }
    auto output =
        graph_validation_record{.id = *id,
                                .query = {*query_kind, *query_arguments},
                                .incremental = *incremental,
                                .clean = *clean,
                                .agreement = *agreement,
                                .at = std::chrono::sys_seconds{std::chrono::seconds{*at}}};
    for (const auto& input : *inputs) {
        const auto* type = text_field(input, "type");
        const auto* fingerprint_text = text_field(input, "fingerprint");
        if (!type || !fingerprint_text) {
            return std::unexpected(failure(errc::corrupt, "invalid daemon validation input"));
        }
        auto fingerprint = parse_digest(*fingerprint_text);
        if (!fingerprint) {
            return std::unexpected(fingerprint.error());
        }
        if (*type == "input") {
            const auto* input_kind = unsigned_field(input, "kind");
            const auto* name = text_field(input, "name");
            if (!input_kind || *input_kind < 1 || *input_kind > 4 || !name) {
                return std::unexpected(failure(errc::corrupt, "invalid daemon graph input key"));
            }
            output.inputs.push_back(
                {graph_input_key{static_cast<graph_input_kind>(*input_kind), *name}, *fingerprint});
            continue;
        }
        const auto* kind = text_field(input, "kind");
        const auto* arguments_value = field(input, "arguments");
        const auto* arguments =
            arguments_value ? std::get_if<cbor::value::bytes>(&arguments_value->data) : nullptr;
        if (*type != "query" || !kind || !arguments) {
            return std::unexpected(failure(errc::corrupt, "invalid daemon query dependency"));
        }
        output.inputs.push_back({query_key{*kind, *arguments}, *fingerprint});
    }
    return output;
}

auto encode_frame(std::uint8_t type, std::span<const std::byte> payload)
    -> result<std::vector<std::byte>> {
    if (payload.size() > maximum_frame) {
        return std::unexpected(failure(errc::invalid_argument, "daemon frame is too large"));
    }
    const auto size = static_cast<std::uint32_t>(payload.size());
    auto output = std::vector<std::byte>{};
    output.reserve(5 + payload.size());
    output.push_back(std::byte{type});
    output.push_back(std::byte{static_cast<std::uint8_t>(size >> 24U)});
    output.push_back(std::byte{static_cast<std::uint8_t>(size >> 16U)});
    output.push_back(std::byte{static_cast<std::uint8_t>(size >> 8U)});
    output.push_back(std::byte{static_cast<std::uint8_t>(size)});
    output.insert(output.end(), payload.begin(), payload.end());
    return output;
}

auto encode_request(cbor::value request) -> result<std::vector<std::byte>> {
    auto payload = cbor::encode(std::move(request));
    if (!payload) {
        return std::unexpected(payload.error());
    }
    return encode_frame(1, *payload);
}

auto encode_request(std::string_view operation) -> result<std::vector<std::byte>> {
    return encode_request(
        map({{"v", cbor::value{std::uint64_t{1}}}, {"op", std::string{operation}}}));
}

auto encode_success(cbor::value value) -> result<std::vector<std::byte>> {
    auto payload = cbor::encode(std::move(value));
    if (!payload) {
        return std::unexpected(payload.error());
    }
    return encode_frame(1, *payload);
}

auto encode_error(const error& value) -> result<std::vector<std::byte>> {
    auto payload = cbor::encode(map({{"v", cbor::value{std::uint64_t{1}}},
                                     {"code", cbor::value{std::string{to_string(value.code)}}},
                                     {"message", cbor::value{value.message}},
                                     {"context", cbor::value{value.context}}}));
    if (!payload) {
        return std::unexpected(payload.error());
    }
    return encode_frame(2, *payload);
}

auto status_value(const daemon_status& status) -> cbor::value {
    auto paths = cbor::value::array{};
    for (const auto& path : status.invalidated_paths) {
        paths.emplace_back(path);
    }
    auto targets = cbor::value::array{};
    for (const auto& target : status.targets) {
        targets.push_back(map({{"name", cbor::value{target.name}},
                               {"cursor", cbor::value{target.cursor.token}},
                               {"behind", cbor::value{target.behind}},
                               {"deliveries", cbor::value{target.deliveries}},
                               {"last_error", cbor::value{target.last_error.value_or("")}},
                               {"detail", cbor::value{target.detail.value_or("")}}}));
    }
    auto buckets = cbor::value::array{};
    for (const auto& bucket : status.flow.buckets) {
        buckets.push_back(
            map({{"scope", cbor::value{std::string{flow_scope_name(bucket.scope)}}},
                 {"key", cbor::value{bucket.key}},
                 {"tokens_milli",
                  cbor::value{static_cast<std::uint64_t>(std::max(0.0, bucket.tokens) * 1000.0)}},
                 {"rate", cbor::value{bucket.rate}},
                 {"burst", cbor::value{bucket.burst}},
                 {"rejected", cbor::value{bucket.rejected}}}));
    }
    return map(
        {{"v", cbor::value{std::uint64_t{1}}},
         {"running", cbor::value{status.running}},
         {"pid", cbor::value{status.process_id}},
         {"endpoint", cbor::value{status.endpoint}},
         {"last_drip", cbor::value{status.last_drip ? to_string(status.last_drip->value) : ""}},
         {"invalidated", cbor::value{std::move(paths)}},
         {"targets", cbor::value{std::move(targets)}},
         {"capture_state",
          cbor::value{status.capture.state == capture_status::state::paused ? "paused"
                                                                            : "running"}},
         {"capture_pending", cbor::value{status.capture.pending_drips}},
         {"capture_overflow", cbor::value{status.capture.overflow_count}},
         {"capture_recrawl", cbor::value{status.capture.recrawl_count}},
         {"capture_cursor", cbor::value{status.capture.last_cursor.token}},
         {"flow",
          map({{"paused", cbor::value{status.flow.paused}},
               {"reason", cbor::value{std::string{flow_reason_name(status.flow.reason)}}},
               {"persistence_lag", cbor::value{status.flow.persistence_lag}},
               {"consumer_lag", cbor::value{status.flow.consumer_lag}},
               {"buckets", cbor::value{std::move(buckets)}}})},
         {"graph_keys", cbor::value{status.graph_keys}},
         {"graph_values", cbor::value{status.graph_values}},
         {"graph_memory", cbor::value{status.graph_memory_bytes}},
         {"graph_ceiling", cbor::value{status.graph_memory_ceiling}},
         {"graph_validation_backlog", cbor::value{status.graph_validation_backlog}},
         {"last_graph_divergence",
          status.last_graph_divergence ? validation_value(*status.last_graph_divergence)
                                       : cbor::value{}}});
}

auto encode_status(const daemon_status& status) -> result<std::vector<std::byte>> {
    auto payload = cbor::encode(status_value(status));
    if (!payload) {
        return std::unexpected(payload.error());
    }
    return encode_frame(1, *payload);
}

auto encode_acknowledgement() -> result<std::vector<std::byte>> {
    auto payload =
        cbor::encode(map({{"v", cbor::value{std::uint64_t{1}}}, {"ok", cbor::value{true}}}));
    if (!payload) {
        return std::unexpected(payload.error());
    }
    return encode_frame(1, *payload);
}

auto decode_payload(std::span<const std::byte> frame, std::uint8_t expected_type)
    -> result<cbor::value> {
    if (frame.size() < 5 || std::to_integer<std::uint8_t>(frame.front()) != expected_type) {
        return std::unexpected(failure(errc::corrupt, "invalid daemon frame type"));
    }
    auto size = std::uint32_t{};
    for (std::size_t index = 1; index != 5; ++index) {
        size = (size << 8U) | std::to_integer<std::uint8_t>(frame[index]);
    }
    if (size > maximum_frame || frame.size() != 5 + size) {
        return std::unexpected(failure(errc::corrupt, "invalid daemon frame length"));
    }
    return cbor::decode(frame.subspan(5));
}

auto decode_response(std::span<const std::byte> frame) -> result<cbor::value> {
    if (!frame.empty() && std::to_integer<std::uint8_t>(frame.front()) == 2) {
        auto decoded = decode_payload(frame, 2);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        const auto* code = text_field(*decoded, "code");
        const auto* message = text_field(*decoded, "message");
        const auto* context = text_field(*decoded, "context");
        return std::unexpected(failure(code ? error_code(*code) : errc::internal,
                                       message ? *message : "daemon request failed",
                                       context ? *context : "daemon protocol"));
    }
    return decode_payload(frame, 1);
}

auto decode_status(std::span<const std::byte> frame) -> result<daemon_status> {
    auto decoded = decode_payload(frame, 1);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    const auto* running = bool_field(*decoded, "running");
    const auto* pid = unsigned_field(*decoded, "pid");
    const auto* endpoint = text_field(*decoded, "endpoint");
    const auto* last_drip = text_field(*decoded, "last_drip");
    const auto* invalidated_value = field(*decoded, "invalidated");
    const auto* targets_value = field(*decoded, "targets");
    const auto* graph_keys = unsigned_field(*decoded, "graph_keys");
    const auto* graph_values = unsigned_field(*decoded, "graph_values");
    const auto* graph_memory = unsigned_field(*decoded, "graph_memory");
    const auto* graph_ceiling = unsigned_field(*decoded, "graph_ceiling");
    const auto* graph_validation_backlog = unsigned_field(*decoded, "graph_validation_backlog");
    const auto* last_graph_divergence_value = field(*decoded, "last_graph_divergence");
    const auto* capture_state = text_field(*decoded, "capture_state");
    const auto* capture_pending = unsigned_field(*decoded, "capture_pending");
    const auto* capture_overflow = unsigned_field(*decoded, "capture_overflow");
    const auto* capture_recrawl = unsigned_field(*decoded, "capture_recrawl");
    const auto* capture_cursor = text_field(*decoded, "capture_cursor");
    const auto* flow_value = field(*decoded, "flow");
    const auto* invalidated =
        invalidated_value ? std::get_if<cbor::value::array>(&invalidated_value->data) : nullptr;
    const auto* targets =
        targets_value ? std::get_if<cbor::value::array>(&targets_value->data) : nullptr;
    if (!running || !pid || !endpoint || !last_drip || !invalidated || !targets || !graph_keys ||
        !graph_values || !graph_memory || !graph_ceiling || !graph_validation_backlog ||
        !last_graph_divergence_value || !capture_state || !capture_pending || !capture_overflow ||
        !capture_recrawl || !capture_cursor || !flow_value) {
        return std::unexpected(failure(errc::corrupt, "invalid daemon status"));
    }
    auto output = daemon_status{.running = *running,
                                .process_id = *pid,
                                .endpoint = *endpoint,
                                .capture = {.state = *capture_state == "paused"
                                                         ? capture_status::state::paused
                                                         : capture_status::state::running,
                                            .pending_drips = *capture_pending,
                                            .overflow_count = *capture_overflow,
                                            .recrawl_count = *capture_recrawl,
                                            .last_cursor = {*capture_cursor}},
                                .graph_keys = *graph_keys,
                                .graph_values = *graph_values,
                                .graph_memory_bytes = *graph_memory,
                                .graph_memory_ceiling = *graph_ceiling,
                                .graph_validation_backlog = *graph_validation_backlog};
    const auto* flow_paused = bool_field(*flow_value, "paused");
    const auto* flow_reason = text_field(*flow_value, "reason");
    const auto* persistence_lag = unsigned_field(*flow_value, "persistence_lag");
    const auto* consumer_lag = unsigned_field(*flow_value, "consumer_lag");
    const auto* buckets_value = field(*flow_value, "buckets");
    const auto* buckets =
        buckets_value ? std::get_if<cbor::value::array>(&buckets_value->data) : nullptr;
    if (!flow_paused || !flow_reason || !persistence_lag || !consumer_lag || !buckets) {
        return std::unexpected(failure(errc::corrupt, "invalid daemon flow status"));
    }
    output.flow.paused = *flow_paused;
    output.flow.reason = *flow_reason == "persistence" ? flow_pause_reason::persistence
                         : *flow_reason == "consumers" ? flow_pause_reason::consumers
                                                       : flow_pause_reason::none;
    output.flow.persistence_lag = *persistence_lag;
    output.flow.consumer_lag = *consumer_lag;
    for (const auto& value : *buckets) {
        const auto* scope = text_field(value, "scope");
        const auto* key = text_field(value, "key");
        const auto* tokens = unsigned_field(value, "tokens_milli");
        const auto* rate = unsigned_field(value, "rate");
        const auto* burst = unsigned_field(value, "burst");
        const auto* rejected = unsigned_field(value, "rejected");
        if (!scope || !key || !tokens || !rate || !burst || !rejected) {
            return std::unexpected(failure(errc::corrupt, "invalid daemon flow bucket"));
        }
        output.flow.buckets.push_back({.scope = *scope == "actor"      ? flow_scope::actor
                                                : *scope == "document" ? flow_scope::document
                                                                       : flow_scope::repository,
                                       .key = *key,
                                       .tokens = static_cast<double>(*tokens) / 1000.0,
                                       .rate = *rate,
                                       .burst = *burst,
                                       .rejected = *rejected});
    }
    if (!std::holds_alternative<cbor::null>(last_graph_divergence_value->data)) {
        auto divergence = decode_validation(*last_graph_divergence_value);
        if (!divergence) {
            return std::unexpected(divergence.error());
        }
        output.last_graph_divergence = std::move(*divergence);
    }
    if (!last_drip->empty()) {
        auto parsed = parse_digest(*last_drip);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        output.last_drip = drip_id{*parsed};
    }
    for (const auto& value : *invalidated) {
        const auto* path = std::get_if<std::string>(&value.data);
        if (!path) {
            return std::unexpected(failure(errc::corrupt, "invalid daemon path status"));
        }
        output.invalidated_paths.push_back(*path);
    }
    for (const auto& value : *targets) {
        const auto* name = text_field(value, "name");
        const auto* cursor = text_field(value, "cursor");
        const auto* behind = unsigned_field(value, "behind");
        const auto* deliveries = unsigned_field(value, "deliveries");
        const auto* last_error = text_field(value, "last_error");
        const auto* detail = text_field(value, "detail");
        if (!name || !cursor || !behind || !deliveries || !last_error) {
            return std::unexpected(failure(errc::corrupt, "invalid daemon target status"));
        }
        output.targets.push_back(
            {.name = *name,
             .cursor = {*cursor},
             .behind = *behind,
             .deliveries = *deliveries,
             .last_error = last_error->empty() ? std::optional<std::string>{}
                                               : std::optional<std::string>{*last_error},
             .detail = !detail || detail->empty() ? std::optional<std::string>{}
                                                  : std::optional<std::string>{*detail}});
    }
    return output;
}

class no_op_target final : public daemon_target {
public:
    explicit no_op_target(std::string target_name) : name_(std::move(target_name)) {}

    auto name() const -> std::string override {
        return name_;
    }

    runtime::task<void> deliver(daemon_delivery) override {
        co_return {};
    }

private:
    std::string name_;
};

class graph_target final : public daemon_target {
public:
    explicit graph_target(graph& value) : graph_(value) {}

    auto name() const -> std::string override {
        return "build";
    }

    runtime::task<void> deliver(daemon_delivery delivery) override {
        for (const auto& operation : delivery.value.ops) {
            auto invalidated = graph_.mark_changed(
                {graph_input_kind::working_tree_blob, operation.path}, durability::low);
            if (!invalidated) {
                co_return std::unexpected(invalidated.error());
            }
        }
        co_return {};
    }

private:
    graph& graph_;
};

class hot_update_target final : public daemon_target {
public:
    explicit hot_update_target(std::filesystem::path checkout) : checkout_(std::move(checkout)) {}

    auto name() const -> std::string override {
        return "hot-update";
    }

    auto detail() const -> std::optional<std::string> override {
        std::lock_guard lock(mutex_);
        return detail_;
    }

    runtime::task<void> deliver(daemon_delivery) override {
        auto source = co_await repository::open(checkout_);
        if (!source && source.error().code == errc::not_found) {
            source = co_await repository::open_build(checkout_);
        }
        if (!source) {
            co_return std::unexpected(source.error());
        }
        const auto config = source->hot_update_configuration();
        if (config.targets.empty()) {
            set_detail("not configured");
            co_return {};
        }
        auto descriptions = std::vector<std::string>{};
        for (const auto& configured : config.targets) {
            auto delivered = co_await update(*source, config, configured);
            if (!delivered) {
                co_return std::unexpected(delivered.error());
            }
            descriptions.push_back(std::move(*delivered));
        }
        auto description = std::string{};
        for (const auto& value : descriptions) {
            description += description.empty() ? value : "; " + value;
        }
        set_detail(std::move(description));
        co_return {};
    }

private:
    static auto result_key(std::string_view name) -> std::string {
        return "hot_update/current/" + std::string{name};
    }

    static auto evidence_key(result_id value) -> std::string {
        return "hot_update/evidence/" + to_string(value.value);
    }

    static auto encoded(std::string_view value) -> std::vector<std::byte> {
        return {reinterpret_cast<const std::byte*>(value.data()),
                reinterpret_cast<const std::byte*>(value.data() + value.size())};
    }

    static auto decoded(std::span<const std::byte> value) -> std::string_view {
        return {reinterpret_cast<const char*>(value.data()), value.size()};
    }

    runtime::task<std::string> update(repository& source,
                                      const hot_update_config& config,
                                      const hot_update_target_config& configured) {
        auto handle = source.build(build_target{configured.name});
        if (!handle) {
            co_return std::unexpected(handle.error());
        }
        auto events = handle->events();
        if (!events) {
            co_return std::unexpected(events.error());
        }
        auto results = std::vector<result_id>{};
        while (true) {
            auto event = co_await events->next();
            if (!event) {
                co_return std::unexpected(event.error());
            }
            if (!*event) {
                break;
            }
            if ((**event).result) {
                results.push_back(*(**event).result);
            }
        }
        auto summary = co_await handle->wait();
        if (!summary) {
            co_return std::unexpected(summary.error());
        }
        if (summary->exit != 0 || summary->failures != 0) {
            co_return std::unexpected(
                failure(errc::io, "hot-update target build failed", configured.name));
        }
        if (results.empty()) {
            co_return std::unexpected(
                failure(errc::not_found, "hot-update target produced no result", configured.name));
        }
        auto target = results.back();
        for (auto iterator = results.rbegin(); iterator != results.rend(); ++iterator) {
            auto evidence = co_await source.metadata(evidence_key(*iterator));
            if (!evidence) {
                co_return std::unexpected(evidence.error());
            }
            if (*evidence) {
                target = *iterator;
                break;
            }
        }
        auto current = co_await source.metadata(result_key(configured.name));
        if (!current) {
            co_return std::unexpected(current.error());
        }
        if (!*current) {
            auto stored = co_await source.set_metadata(result_key(configured.name),
                                                       encoded(to_string(target.value)));
            if (!stored) {
                co_return std::unexpected(stored.error());
            }
            co_return configured.name + " baseline=" + to_string(target.value);
        }
        auto previous_digest = parse_digest(decoded(**current));
        if (!previous_digest) {
            co_return std::unexpected(previous_digest.error());
        }
        auto plan = co_await hot_update::classify(source, result_id{*previous_digest}, target);
        if (!plan) {
            co_return std::unexpected(plan.error());
        }
        auto record = digest{};
        if (plan->tier == hot_update_tier::tier_0) {
            auto unchanged = co_await hot_update::record_outcome(source, *plan);
            if (!unchanged) {
                co_return std::unexpected(unchanged.error());
            }
            record = unchanged->record;
        } else if (plan->outcome == hot_update_outcome::patched) {
            auto patch = co_await hot_update::create_patch(source, *plan);
            if (!patch) {
                co_return std::unexpected(patch.error());
            }
            auto applied = co_await hot_update::apply(source, patch->record, configured.endpoint);
            if (!applied) {
                co_return std::unexpected(applied.error());
            }
            record = applied->record;
        } else {
            auto restarted =
                co_await hot_update::restart(source, *plan, checkout_, config.restart_hook);
            if (!restarted) {
                co_return std::unexpected(restarted.error());
            }
            record = restarted->record;
        }
        auto stored = co_await source.set_metadata(result_key(configured.name),
                                                   encoded(to_string(target.value)));
        if (!stored) {
            co_return std::unexpected(stored.error());
        }
        co_return configured.name + " outcome=" + std::string{to_string(plan->outcome)} +
            " tier=" + std::string{to_string(plan->tier)} + " record=" + to_string(record);
    }

    auto set_detail(std::string value) -> void {
        std::lock_guard lock(mutex_);
        detail_ = std::move(value);
    }

    std::filesystem::path checkout_;
    mutable std::mutex mutex_;
    std::optional<std::string> detail_;
};

class drive_target final : public daemon_target {
public:
    explicit drive_target(drive_client client) : client_(std::move(client)) {}

    auto name() const -> std::string override {
        return "drive";
    }

    runtime::task<void> deliver(daemon_delivery) override {
        auto synchronized = co_await client_.sync();
        if (!synchronized) {
            co_return std::unexpected(synchronized.error());
        }
        const auto refused =
            std::ranges::find_if(synchronized->status.references, [](const auto& reference) {
                return reference.outcome == sync_ref_outcome_kind::refused;
            });
        if (refused != synchronized->status.references.end()) {
            co_return std::unexpected(
                failure(errc::unauthorized,
                        "drive remote refused " + refused->name + ": " + refused->reason));
        }
        co_return result<void>{};
    }

private:
    drive_client client_;
};

#ifndef _WIN32

struct accepted_request {
    int connection{-1};
    std::vector<std::byte> frame;
};

struct unix_handles {
    int lock{-1};
    int listener{-1};
};

auto close_descriptor(int descriptor) -> void {
    if (descriptor >= 0) {
        ::close(descriptor);
    }
}

auto write_all(int descriptor, std::span<const std::byte> bytes) -> result<void> {
    auto offset = std::size_t{};
    while (offset != bytes.size()) {
        const auto written = ::write(
            descriptor, bytes.data() + offset, static_cast<std::size_t>(bytes.size() - offset));
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return std::unexpected(failure(errc::io, std::strerror(errno), "daemon socket"));
        }
        offset += static_cast<std::size_t>(written);
    }
    return {};
}

auto read_all(int descriptor, std::span<std::byte> bytes) -> result<void> {
    auto offset = std::size_t{};
    while (offset != bytes.size()) {
        const auto received = ::read(
            descriptor, bytes.data() + offset, static_cast<std::size_t>(bytes.size() - offset));
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            return std::unexpected(
                failure(errc::io,
                        received == 0 ? "daemon socket closed" : std::strerror(errno),
                        "daemon socket"));
        }
        offset += static_cast<std::size_t>(received);
    }
    return {};
}

auto read_frame(int descriptor) -> result<std::vector<std::byte>> {
    auto header = std::array<std::byte, 5>{};
    auto header_read = read_all(descriptor, header);
    if (!header_read) {
        return std::unexpected(header_read.error());
    }
    auto size = std::uint32_t{};
    for (std::size_t index = 1; index != header.size(); ++index) {
        size = (size << 8U) | std::to_integer<std::uint8_t>(header[index]);
    }
    if (size > maximum_frame) {
        return std::unexpected(failure(errc::corrupt, "daemon frame is too large"));
    }
    auto output = std::vector<std::byte>{header.begin(), header.end()};
    output.resize(header.size() + size);
    auto payload_read = read_all(descriptor, std::span{output}.subspan(header.size()));
    if (!payload_read) {
        return std::unexpected(payload_read.error());
    }
    return output;
}

auto socket_address(const std::filesystem::path& endpoint) -> result<sockaddr_un> {
    const auto text = endpoint.string();
    auto output = sockaddr_un{};
    output.sun_family = AF_UNIX;
    if (text.size() >= sizeof(output.sun_path)) {
        return std::unexpected(
            failure(errc::invalid_argument, "daemon socket path is too long", text));
    }
    std::memcpy(output.sun_path, text.c_str(), text.size() + 1);
    return output;
}

auto open_listener(const std::filesystem::path& directory, const std::filesystem::path& endpoint)
    -> result<unix_handles> {
    std::filesystem::create_directories(directory);
    const auto lock_path = directory / "lock";
    const auto lock_descriptor = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0600);
    if (lock_descriptor < 0) {
        return std::unexpected(failure(errc::io, std::strerror(errno), lock_path.string()));
    }
    if (::flock(lock_descriptor, LOCK_EX | LOCK_NB) != 0) {
        close_descriptor(lock_descriptor);
        return std::unexpected(
            failure(errc::already_exists, "daemon is already running", directory.string()));
    }
    auto address = socket_address(endpoint);
    if (!address) {
        close_descriptor(lock_descriptor);
        return std::unexpected(address.error());
    }
    std::filesystem::remove(endpoint);
    const auto listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0) {
        close_descriptor(lock_descriptor);
        return std::unexpected(failure(errc::io, std::strerror(errno), endpoint.string()));
    }
    if (::bind(listener, reinterpret_cast<const sockaddr*>(&*address), sizeof(*address)) != 0 ||
        ::listen(listener, 16) != 0) {
        const auto message = std::string{std::strerror(errno)};
        close_descriptor(listener);
        close_descriptor(lock_descriptor);
        std::filesystem::remove(endpoint);
        return std::unexpected(failure(errc::io, message, endpoint.string()));
    }
    if (::chmod(endpoint.c_str(), 0600) != 0) {
        const auto message = std::string{std::strerror(errno)};
        close_descriptor(listener);
        close_descriptor(lock_descriptor);
        std::filesystem::remove(endpoint);
        return std::unexpected(failure(errc::io, message, endpoint.string()));
    }
    return unix_handles{lock_descriptor, listener};
}

auto accept_request(int listener) -> result<std::optional<accepted_request>> {
    auto descriptor = pollfd{.fd = listener, .events = POLLIN, .revents = 0};
    const auto ready = ::poll(&descriptor, 1, 50);
    if (ready < 0 && errno == EINTR) {
        return std::optional<accepted_request>{};
    }
    if (ready < 0) {
        return std::unexpected(failure(errc::io, std::strerror(errno), "daemon socket"));
    }
    if (ready == 0) {
        return std::optional<accepted_request>{};
    }
    const auto connection = ::accept(listener, nullptr, nullptr);
    if (connection < 0) {
        return std::unexpected(failure(errc::io, std::strerror(errno), "daemon socket"));
    }
    auto frame = read_frame(connection);
    if (!frame) {
        close_descriptor(connection);
        return std::unexpected(frame.error());
    }
    return std::optional<accepted_request>{{connection, std::move(*frame)}};
}

auto request(const std::filesystem::path& endpoint, std::vector<std::byte> frame)
    -> result<std::vector<std::byte>> {
    auto address = socket_address(endpoint);
    if (!address) {
        return std::unexpected(address.error());
    }
    const auto descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor < 0) {
        return std::unexpected(failure(errc::io, std::strerror(errno), endpoint.string()));
    }
    if (::connect(descriptor, reinterpret_cast<const sockaddr*>(&*address), sizeof(*address)) !=
        0) {
        const auto code = errno;
        close_descriptor(descriptor);
        return std::unexpected(
            failure(code == ENOENT || code == ECONNREFUSED ? errc::unavailable : errc::io,
                    std::strerror(code),
                    endpoint.string()));
    }
    auto written = write_all(descriptor, frame);
    if (!written) {
        close_descriptor(descriptor);
        return std::unexpected(written.error());
    }
    auto response = read_frame(descriptor);
    close_descriptor(descriptor);
    return response;
}

auto request(const std::filesystem::path& endpoint, std::string_view operation)
    -> result<std::vector<std::byte>> {
    auto frame = encode_request(operation);
    if (!frame) {
        return std::unexpected(frame.error());
    }
    return request(endpoint, std::move(*frame));
}

#endif

struct service_state {
    ~service_state() {
#ifndef _WIN32
        const auto owns_endpoint = handles.listener >= 0;
        close_descriptor(handles.listener);
        if (handles.lock >= 0) {
            ::flock(handles.lock, LOCK_UN);
            close_descriptor(handles.lock);
        }
        if (owns_endpoint) {
            auto ignored = std::error_code{};
            std::filesystem::remove(endpoint, ignored);
        }
#endif
    }

    std::filesystem::path checkout;
    std::filesystem::path directory;
    std::filesystem::path endpoint;
    daemon_options options;
    graph query_graph;
    std::unique_ptr<daemon_detail::fanout> fanout;
    std::optional<capture> capture_handle;
    std::optional<subscription> capture_subscription;
    struct build_session {
        std::shared_ptr<repository> source;
        build_handle handle;
        build_event_subscription subscription;
    };
    std::map<std::uint64_t, build_session> builds;
    std::uint64_t next_build{};
    std::mutex queue_mutex;
    std::mutex completion_mutex;
    std::mutex flow_mutex;
    std::deque<daemon_delivery> queue;
    std::optional<error> completion_error;
    flow_config flow_configuration;
    std::shared_ptr<flow_detail::controller> flow_limits;
    std::map<std::string, std::uint64_t> consumer_lags;
    flow_pause_reason flow_pause{flow_pause_reason::none};
    std::atomic<bool> stopping{};
    std::atomic<bool> stopped{};
    std::atomic<std::int64_t> last_activity_ns{};
    std::chrono::steady_clock::time_point last_validation{};
#ifndef _WIN32
    unix_handles handles;
#endif

    auto touch() -> void {
        last_activity_ns.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                               std::memory_order_release);
    }

    auto enqueue(daemon_delivery delivery) -> void;
    auto dequeue() -> std::optional<daemon_delivery>;
};

auto current_status(const std::shared_ptr<service_state>& state) -> daemon_status {
    auto current_capture = capture_status{};
    if (state->capture_handle) {
        auto status = state->capture_handle->status();
        if (status) {
            current_capture = *status;
        }
    }
    const auto graph_status = state->query_graph.statistics();
    const auto validation_status = state->query_graph.validation_status();
#ifdef _WIN32
    const auto process_id = std::uint64_t{};
#else
    const auto process_id = static_cast<std::uint64_t>(::getpid());
#endif
    auto targets = state->fanout->target_status();
    auto consumer_lag = std::uint64_t{};
    for (const auto& target : targets) {
        consumer_lag = std::max(consumer_lag, target.behind);
    }
    auto flow = flow_status{};
    {
        std::lock_guard lock(state->flow_mutex);
        for (const auto& [name, behind] : state->consumer_lags) {
            static_cast<void>(name);
            consumer_lag = std::max(consumer_lag, behind);
        }
        const auto persistence_lag = current_capture.pending_drips;
        const auto consumer_high = std::min(state->flow_configuration.pause_above,
                                            state->flow_configuration.max_unacknowledged);
        if (state->flow_pause == flow_pause_reason::persistence &&
            persistence_lag <= state->flow_configuration.resume_below) {
            state->flow_pause = flow_pause_reason::none;
        }
        if (state->flow_pause == flow_pause_reason::consumers &&
            consumer_lag <= state->flow_configuration.resume_below) {
            state->flow_pause = flow_pause_reason::none;
        }
        if (consumer_lag > consumer_high) {
            state->flow_pause = flow_pause_reason::consumers;
        } else if (persistence_lag > state->flow_configuration.pause_above) {
            state->flow_pause = flow_pause_reason::persistence;
        }
        state->flow_limits->set_pause(state->flow_pause);
        flow = {.paused = state->flow_pause != flow_pause_reason::none,
                .reason = state->flow_pause,
                .persistence_lag = persistence_lag,
                .consumer_lag = consumer_lag,
                .buckets = state->flow_limits->status()};
    }
    return {.running = !state->stopped.load(std::memory_order_acquire),
            .process_id = process_id,
            .endpoint = state->endpoint.string(),
            .last_drip = state->fanout->last_drip(),
            .invalidated_paths = state->fanout->invalidated_paths(),
            .targets = std::move(targets),
            .capture = std::move(current_capture),
            .graph_keys = graph_status.keys,
            .graph_values = graph_status.resident_values,
            .graph_memory_bytes = graph_status.resident_value_bytes,
            .graph_memory_ceiling = graph_status.memory_ceiling,
            .graph_validation_backlog = validation_status.backlog,
            .last_graph_divergence = validation_status.last_divergence,
            .flow = std::move(flow)};
}

auto service_state::enqueue(daemon_delivery delivery) -> void {
    auto observed = fanout->observe(delivery.cursor);
    if (!observed) {
        std::lock_guard lock(completion_mutex);
        completion_error = observed.error();
        stopping.store(true, std::memory_order_release);
        return;
    }
    {
        std::lock_guard lock(queue_mutex);
        queue.push_back(std::move(delivery));
    }
    touch();
}

auto service_state::dequeue() -> std::optional<daemon_delivery> {
    std::lock_guard lock(queue_mutex);
    if (queue.empty()) {
        return std::nullopt;
    }
    auto output = std::move(queue.front());
    queue.pop_front();
    return output;
}

#ifndef _WIN32
runtime::task<repository> open_configured_build(const std::filesystem::path& checkout) {
    auto wrapped = co_await repository::open_build(checkout);
    if (wrapped) {
        co_return std::move(*wrapped);
    }
    if (wrapped.error().code != errc::not_configured) {
        co_return std::unexpected(wrapped.error());
    }
    auto source = co_await repository::open(checkout);
    if (!source) {
        co_return std::unexpected(wrapped.error());
    }
    auto plan = co_await source->build_plan();
    if (!plan) {
        co_return std::unexpected(plan.error());
    }
    co_return std::move(*source);
}

runtime::task<std::vector<std::byte>>
process_build_request(const std::shared_ptr<service_state>& state,
                      const cbor::value& request_value,
                      std::string_view operation) {
    if (operation == "build_plan") {
        const auto* target_value = field(request_value, "target");
        if (!target_value) {
            co_return encode_error(failure(errc::invalid_argument, "missing daemon build target"));
        }
        auto target = decode_build_target(*target_value);
        if (!target) {
            co_return encode_error(target.error());
        }
        auto source = co_await open_configured_build(state->checkout);
        if (!source) {
            co_return encode_error(source.error());
        }
        auto plan = co_await source->build_plan(std::move(*target));
        if (!plan) {
            co_return encode_error(plan.error());
        }
        co_return encode_success(map({{"v", std::uint64_t{1}}, {"plan", build_plan_value(*plan)}}));
    }
    if (operation == "build_repository") {
        const auto* target_value = field(request_value, "target");
        if (!target_value) {
            co_return encode_error(failure(errc::invalid_argument, "missing daemon build target"));
        }
        auto target = decode_build_target(*target_value);
        if (!target) {
            co_return encode_error(target.error());
        }
        auto source = co_await open_configured_build(state->checkout);
        if (!source) {
            co_return encode_error(source.error());
        }
        auto owned_source = std::make_shared<repository>(std::move(*source));
        auto handle = owned_source->build(std::move(*target));
        if (!handle) {
            co_return encode_error(handle.error());
        }
        auto subscription = handle->events();
        if (!subscription) {
            co_return encode_error(subscription.error());
        }
        const auto id = ++state->next_build;
        state->builds.emplace(id,
                              service_state::build_session{std::move(owned_source),
                                                           std::move(*handle),
                                                           std::move(*subscription)});
        co_return encode_success(map({{"v", std::uint64_t{1}}, {"build", id}}));
    }
    if (operation == "build_start") {
        const auto* input_value = field(request_value, "input");
        if (!input_value) {
            co_return encode_error(failure(errc::invalid_argument, "missing daemon build input"));
        }
        auto input = decode_action_input(*input_value);
        if (!input) {
            co_return encode_error(input.error());
        }
        auto source = co_await repository::open(state->checkout);
        if (!source) {
            co_return encode_error(source.error());
        }
        auto owned_source = std::make_shared<repository>(std::move(*source));
        auto handle = owned_source->build(std::move(*input));
        if (!handle) {
            co_return encode_error(handle.error());
        }
        auto subscription = handle->events();
        if (!subscription) {
            co_return encode_error(subscription.error());
        }
        const auto id = ++state->next_build;
        state->builds.emplace(id,
                              service_state::build_session{std::move(owned_source),
                                                           std::move(*handle),
                                                           std::move(*subscription)});
        co_return encode_success(map({{"v", std::uint64_t{1}}, {"build", id}}));
    }
    const auto* id = unsigned_field(request_value, "build");
    if (!id) {
        co_return encode_error(failure(errc::invalid_argument, "missing daemon build id"));
    }
    auto session = state->builds.find(*id);
    if (session == state->builds.end()) {
        co_return encode_error(failure(errc::not_found, "daemon build is not active"));
    }
    auto event = co_await session->second.subscription.next();
    if (!event) {
        co_return encode_error(event.error());
    }
    if (!*event) {
        state->builds.erase(session);
        co_return encode_success(map({{"v", std::uint64_t{1}}, {"done", true}}));
    }
    auto response = encode_success(
        map({{"v", std::uint64_t{1}}, {"done", false}, {"event", build_event_value(**event)}}));
    if ((**event).kind == build_event_kind::build_finished) {
        state->builds.erase(session);
    }
    co_return response;
}

runtime::task<std::vector<std::byte>>
process_edit_request(const std::shared_ptr<service_state>& state,
                     const cbor::value& request_value,
                     std::string_view operation) {
    const auto* record_value = field(request_value, "record");
    const auto* record =
        record_value ? std::get_if<cbor::value::bytes>(&record_value->data) : nullptr;
    if (!record) {
        co_return encode_error(failure(errc::invalid_argument, "missing daemon edit record"));
    }
    auto source = co_await repository::open(state->checkout);
    if (!source) {
        co_return encode_error(source.error());
    }
    if (operation == "edit_preview") {
        auto request = decode_edit_request(*record);
        const auto* tool = text_field(request_value, "tool");
        const auto* edit_operation = text_field(request_value, "operation");
        const auto* agent = text_field(request_value, "agent");
        if (!request || !tool || tool->empty() || !edit_operation || edit_operation->empty() ||
            !agent) {
            co_return encode_error(
                request ? failure(errc::invalid_argument, "invalid daemon edit attribution")
                        : request.error());
        }
        auto attribution = edit_attribution{*tool, *edit_operation, std::nullopt};
        if (!agent->empty()) {
            attribution.agent = *agent;
        }
        auto preview = co_await source->preview_edit(std::move(*request), std::move(attribution));
        if (!preview) {
            co_return encode_error(preview.error());
        }
        auto encoded = encode_edit_preview(*preview);
        if (!encoded) {
            co_return encode_error(encoded.error());
        }
        co_return encode_success(
            map({{"v", std::uint64_t{1}},
                 {"preview", cbor::value::bytes{encoded->begin(), encoded->end()}}}));
    }
    auto preview = decode_edit_preview(*record);
    if (!preview) {
        co_return encode_error(preview.error());
    }
    auto applied = co_await source->apply_edit(*preview);
    if (!applied) {
        co_return encode_error(applied.error());
    }
    auto encoded = encode_edit_result(*applied);
    if (!encoded) {
        co_return encode_error(encoded.error());
    }
    co_return encode_success(
        map({{"v", std::uint64_t{1}},
             {"result", cbor::value::bytes{encoded->begin(), encoded->end()}}}));
}

runtime::task<std::vector<std::byte>> send_daemon_request(std::filesystem::path endpoint,
                                                          std::vector<std::byte> frame) {
    auto response = co_await runtime::offload(
        [endpoint = std::move(endpoint), frame = std::move(frame)]() mutable {
            return request(endpoint, std::move(frame));
        });
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (!*response) {
        co_return std::unexpected(response->error());
    }
    co_return std::move(**response);
}

runtime::task<void> proxy_build_events(std::filesystem::path endpoint,
                                       std::vector<std::byte> start_frame,
                                       build_detail::event_producer producer) {
    auto response = co_await send_daemon_request(endpoint, std::move(start_frame));
    if (!response) {
        co_return std::unexpected(response.error());
    }
    auto started = decode_response(*response);
    if (!started) {
        co_return std::unexpected(started.error());
    }
    const auto* id = unsigned_field(*started, "build");
    if (!id) {
        co_return std::unexpected(failure(errc::corrupt, "invalid daemon build response"));
    }
    while (true) {
        auto next_frame =
            encode_request(map({{"v", std::uint64_t{1}}, {"op", "build_next"}, {"build", *id}}));
        if (!next_frame) {
            co_return std::unexpected(next_frame.error());
        }
        response = co_await send_daemon_request(endpoint, std::move(*next_frame));
        if (!response) {
            co_return std::unexpected(response.error());
        }
        auto next = decode_response(*response);
        if (!next) {
            co_return std::unexpected(next.error());
        }
        const auto* done = bool_field(*next, "done");
        if (!done) {
            co_return std::unexpected(failure(errc::corrupt, "invalid daemon event response"));
        }
        if (*done) {
            co_return std::unexpected(
                failure(errc::corrupt, "daemon build ended without a summary"));
        }
        const auto* event_value = field(*next, "event");
        if (!event_value) {
            co_return std::unexpected(failure(errc::corrupt, "missing daemon build event"));
        }
        auto event = decode_build_event(*event_value);
        if (!event) {
            co_return std::unexpected(event.error());
        }
        if (event->kind == build_event_kind::build_finished) {
            if (!event->summary) {
                co_return std::unexpected(
                    failure(errc::corrupt, "daemon build summary is missing"));
            }
            producer.finish(*event->summary);
            co_return {};
        }
        producer.publish(std::move(*event));
    }
}

runtime::task<void> run_service(std::shared_ptr<service_state> state) {
    auto retry_after = std::chrono::steady_clock::time_point::min();
    while (!state->stopping.load(std::memory_order_acquire)) {
        static_cast<void>(current_status(state));
        if (std::chrono::steady_clock::now() >= retry_after) {
            if (auto delivery = state->dequeue()) {
                auto delivered = co_await state->fanout->deliver(*delivery);
                if (!delivered) {
                    {
                        std::lock_guard lock(state->queue_mutex);
                        state->queue.push_front(std::move(*delivery));
                    }
                    retry_after = std::chrono::steady_clock::now() + std::chrono::milliseconds{100};
                } else {
                    state->touch();
                    retry_after = std::chrono::steady_clock::time_point::min();
                }
                continue;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (state->query_graph.validation_status().backlog != 0 &&
            now - state->last_validation >= state->options.graph_validation_interval) {
            auto validated = co_await state->query_graph.validate(
                {.rate = state->options.graph_validation_rate, .limit = 1});
            if (!validated) {
                co_return std::unexpected(validated.error());
            }
            state->last_validation = now;
            continue;
        }
        const auto idle = std::chrono::steady_clock::duration{
            std::chrono::steady_clock::now().time_since_epoch().count() -
            state->last_activity_ns.load(std::memory_order_acquire)};
        if (idle >= state->options.idle_timeout) {
            state->stopping.store(true, std::memory_order_release);
            break;
        }
        auto accepted = co_await runtime::offload(
            [listener = state->handles.listener] { return accept_request(listener); });
        if (!accepted) {
            co_return std::unexpected(accepted.error());
        }
        if (!*accepted) {
            continue;
        }
        if (!(**accepted)) {
            continue;
        }
        auto connection = std::move(***accepted);
        state->touch();
        auto decoded = decode_payload(connection.frame, 1);
        auto response = result<std::vector<std::byte>>{};
        if (!decoded) {
            response = encode_error(decoded.error());
        } else {
            const auto* version = unsigned_field(*decoded, "v");
            const auto* operation = text_field(*decoded, "op");
            if (!version || *version != 1 || !operation) {
                response = encode_error(failure(errc::invalid_argument, "invalid daemon request"));
            } else if (*operation == "status") {
                response = encode_status(current_status(state));
            } else if (*operation == "shutdown") {
                response = encode_acknowledgement();
                state->stopping.store(true, std::memory_order_release);
            } else if (*operation == "pause" || *operation == "resume") {
                auto changed = *operation == "pause" ? co_await state->capture_handle->pause()
                                                     : co_await state->capture_handle->resume();
                response = changed ? encode_acknowledgement() : encode_error(changed.error());
            } else if (*operation == "consumer_lag") {
                const auto* name = text_field(*decoded, "name");
                const auto* behind = unsigned_field(*decoded, "behind");
                if (!name || name->empty() || !behind) {
                    response = encode_error(
                        failure(errc::invalid_argument, "invalid consumer lag report"));
                } else {
                    {
                        std::lock_guard lock(state->flow_mutex);
                        state->consumer_lags.insert_or_assign(*name, *behind);
                    }
                    response = encode_acknowledgement();
                }
            } else if (*operation == "presence") {
                const auto* encoded_presence = field(*decoded, "presence");
                auto value = encoded_presence
                                 ? decode_presence_value(*encoded_presence)
                                 : result<liquid::presence>{std::unexpected(failure(
                                       errc::invalid_argument, "presence report is missing"))};
                if (!value) {
                    response = encode_error(value.error());
                } else {
                    auto delivered = co_await state->fanout->deliver_presence(std::move(*value));
                    response =
                        delivered ? encode_acknowledgement() : encode_error(delivered.error());
                }
            } else if (*operation == "lock" || *operation == "unlock") {
                const auto* path = text_field(*decoded, "path");
                const auto* reason = text_field(*decoded, "reason");
                if (!path || path->empty() ||
                    (*operation == "lock" && (!reason || reason->empty()))) {
                    response = encode_error(
                        failure(errc::invalid_argument, "invalid daemon lock request"));
                } else {
                    auto repository_options = liquid::repository_options{};
                    repository_options.authorize_lock = state->options.authorize_lock;
                    auto repository =
                        co_await repository::open(state->checkout, repository_options);
                    if (!repository) {
                        response = encode_error(repository.error());
                    } else {
                        auto changed = *operation == "lock"
                                           ? co_await repository->lock(*path, *reason)
                                           : co_await repository->unlock(*path);
                        if (!changed) {
                            response = encode_error(changed.error());
                        } else {
                            auto locks = co_await repository->locks();
                            auto delivered =
                                locks ? co_await state->fanout->deliver_locks(std::move(*locks))
                                      : result<void>{std::unexpected(locks.error())};
                            response = delivered ? encode_acknowledgement()
                                                 : encode_error(delivered.error());
                        }
                    }
                }
            } else if (*operation == "attribution") {
                const auto* path = text_field(*decoded, "path");
                const auto* change_text = text_field(*decoded, "change");
                auto change = change_text
                                  ? parse_digest(*change_text)
                                  : result<digest>{std::unexpected(failure(
                                        errc::invalid_argument, "attribution change is missing"))};
                if (!path || path->empty() || !change) {
                    response = encode_error(
                        change ? failure(errc::invalid_argument, "attribution path is missing")
                               : change.error());
                } else {
                    auto source = co_await repository::open(state->checkout);
                    auto lines = source ? co_await source->attribution(*path, change_id{*change})
                                        : result<std::vector<line_attribution>>{
                                              std::unexpected(source.error())};
                    response = lines ? encode_success(map({{"v", std::uint64_t{1}},
                                                           {"lines", attribution_value(*lines)}}))
                                     : encode_error(lines.error());
                }
            } else if (*operation == "build_start" || *operation == "build_next" ||
                       *operation == "build_plan" || *operation == "build_repository") {
                response = co_await process_build_request(state, *decoded, *operation);
            } else if (*operation == "edit_preview" || *operation == "edit_apply") {
                response = co_await process_edit_request(state, *decoded, *operation);
            } else {
                response = encode_error(
                    failure(errc::invalid_argument, "unknown daemon request", *operation));
            }
        }
        if (!response) {
            close_descriptor(connection.connection);
            co_return std::unexpected(response.error());
        }
        auto written = co_await runtime::offload(
            [descriptor = connection.connection, bytes = std::move(*response)] {
                auto output = write_all(descriptor, bytes);
                close_descriptor(descriptor);
                return output;
            });
        if (!written) {
            co_return std::unexpected(written.error());
        }
        if (!*written) {
            co_return std::unexpected(written->error());
        }
    }
    if (state->capture_handle) {
        auto closed = co_await state->capture_handle->close();
        if (!closed) {
            co_return std::unexpected(closed.error());
        }
    }
    auto persisted = co_await state->query_graph.flush();
    if (!persisted) {
        co_return std::unexpected(persisted.error());
    }
    close_descriptor(state->handles.listener);
    state->handles.listener = -1;
    std::filesystem::remove(state->endpoint);
    if (state->handles.lock >= 0) {
        ::flock(state->handles.lock, LOCK_UN);
        close_descriptor(state->handles.lock);
        state->handles.lock = -1;
    }
    co_return {};
}
#endif

auto unsupported_endpoint() -> error {
    return failure(errc::unsupported,
                   "the current Corosio dependency has no Windows named-pipe transport",
                   "daemon endpoint");
}

auto endpoint_path(const std::filesystem::path& checkout) -> result<std::filesystem::path> {
    const auto text = checkout.generic_string();
    auto id = hash(algorithm::blake3,
                   std::span{reinterpret_cast<const std::byte*>(text.data()), text.size()});
    if (!id) {
        return std::unexpected(id.error());
    }
    const auto digest_text = to_string(*id);
    return std::filesystem::temp_directory_path() /
           ("liquid-daemon-" + digest_text.substr(digest_text.find(':') + 1, 24) + ".sock");
}

} // namespace

struct daemon::impl {
    std::filesystem::path endpoint;
    std::shared_ptr<service_state> hosted;
};

daemon::daemon() noexcept = default;
daemon::~daemon() = default;
daemon::daemon(daemon&&) noexcept = default;
daemon& daemon::operator=(daemon&&) noexcept = default;
daemon::daemon(std::unique_ptr<impl> implementation) noexcept : impl_(std::move(implementation)) {}

runtime::task<daemon> daemon::start(std::filesystem::path checkout, daemon_options options) {
#ifdef _WIN32
    static_cast<void>(checkout);
    static_cast<void>(options);
    co_return std::unexpected(unsupported_endpoint());
#else
    try {
        auto resolved = daemon_checkout(std::move(checkout));
        if (!resolved) {
            co_return std::unexpected(resolved.error());
        }
        checkout = std::move(*resolved);
        if (!std::filesystem::exists(checkout / "LIQUID_REPO")) {
            co_return std::unexpected(
                failure(errc::not_found, "checkout is not a liquid repository", checkout.string()));
        }
        if (options.idle_timeout <= std::chrono::milliseconds{0}) {
            co_return std::unexpected(
                failure(errc::invalid_argument, "daemon idle timeout must be positive"));
        }
        if (options.graph_validation_rate == 0 ||
            options.graph_validation_interval <= std::chrono::milliseconds{0}) {
            co_return std::unexpected(failure(errc::invalid_argument,
                                              "daemon graph validation settings must be positive"));
        }
        auto state = std::make_shared<service_state>();
        state->checkout = std::move(checkout);
        state->directory = state->checkout / ".liquid" / "daemon";
        auto endpoint = endpoint_path(state->checkout);
        if (!endpoint) {
            co_return std::unexpected(endpoint.error());
        }
        state->endpoint = std::move(*endpoint);
        state->options = std::move(options);
        auto flow_repository = co_await repository::open(state->checkout);
        if (!flow_repository) {
            co_return std::unexpected(flow_repository.error());
        }
        state->flow_configuration = flow_repository->flow_configuration();
        state->flow_limits = flow_detail::shared_controller(state->checkout.generic_string(),
                                                            state->flow_configuration);
        state->options.capture.exclude.push_back(".liquid-store/**");
        state->options.capture.exclude.push_back("LIQUID_REPO");
        state->options.capture.exclude.push_back("LIQUID_JOURNAL");
        auto handles =
            co_await runtime::offload([directory = state->directory, endpoint = state->endpoint] {
                return open_listener(directory, endpoint);
            });
        if (!handles) {
            co_return std::unexpected(handles.error());
        }
        if (!*handles) {
            co_return std::unexpected(handles->error());
        }
        state->handles = **handles;
        auto loaded_graph = co_await graph::open(
            state->directory / "query-graph",
            std::move(state->options.graph_queries),
            {.working_tree = state->checkout, .memory_ceiling = state->options.graph_memory});
        if (!loaded_graph) {
            co_return std::unexpected(loaded_graph.error());
        }
        state->query_graph = std::move(*loaded_graph);
        auto repository_config = co_await repository::open(state->checkout);
        if (!repository_config) {
            co_return std::unexpected(repository_config.error());
        }
        const auto hot_update_config = repository_config->hot_update_configuration();
        auto targets = std::vector<std::shared_ptr<daemon_target>>{};
        targets.push_back(std::make_shared<graph_target>(state->query_graph));
        if (!hot_update_config.targets.empty()) {
            targets.push_back(std::make_shared<hot_update_target>(state->checkout));
        }
        if (state->options.targets.empty()) {
            targets.push_back(search_detail::make_daemon_target(state->checkout));
            targets.push_back(std::make_shared<no_op_target>("collaborators"));
            auto drive = co_await drive_client::open(
                state->checkout, drive_options{.bearer_token = state->options.drive_bearer_token});
            if (drive) {
                targets.push_back(std::make_shared<drive_target>(std::move(*drive)));
            } else if (drive.error().code == errc::not_found) {
                targets.push_back(std::make_shared<no_op_target>("lake"));
            } else {
                co_return std::unexpected(drive.error());
            }
        } else {
            targets.insert(
                targets.end(), state->options.targets.begin(), state->options.targets.end());
        }
        auto names = std::set<std::string>{};
        for (const auto& target : targets) {
            if (!target || target->name().empty() || !names.insert(target->name()).second) {
                co_return std::unexpected(
                    failure(errc::invalid_argument, "daemon target names must be unique"));
            }
        }
        state->fanout = std::make_unique<daemon_detail::fanout>(state->directory / "state.cbor",
                                                                std::move(targets));
        auto loaded_state = co_await runtime::offload([state] { return state->fanout->load(); });
        if (!loaded_state) {
            co_return std::unexpected(loaded_state.error());
        }
        if (!*loaded_state) {
            co_return std::unexpected(loaded_state->error());
        }
        auto capture_handle = co_await capture::open(state->checkout, state->options.capture);
        if (!capture_handle) {
            co_return std::unexpected(capture_handle.error());
        }
        state->capture_handle.emplace(std::move(*capture_handle));
        auto subscribed = state->capture_handle->subscribe_from(
            state->fanout->replay_cursor(),
            [weak = std::weak_ptr{state}](const drip& value, const journal_cursor& cursor) {
                if (auto shared = weak.lock()) {
                    shared->enqueue({value, cursor});
                }
            });
        if (!subscribed) {
            co_return std::unexpected(subscribed.error());
        }
        state->capture_subscription.emplace(std::move(*subscribed));
        state->touch();
        runtime::spawn(run_service(state), [state](result<void> completed) {
            if (!completed) {
                std::lock_guard lock(state->completion_mutex);
                state->completion_error = completed.error();
            }
            state->stopped.store(true, std::memory_order_release);
        });
        auto implementation = std::make_unique<impl>();
        implementation->endpoint = state->endpoint;
        implementation->hosted = std::move(state);
        co_return daemon{std::move(implementation)};
    } catch (const std::exception& caught) {
        co_return std::unexpected(failure(errc::internal, caught.what(), "daemon::start"));
    }
#endif
}

runtime::task<daemon> daemon::attach(std::filesystem::path checkout) {
#ifdef _WIN32
    static_cast<void>(checkout);
    co_return std::unexpected(unsupported_endpoint());
#else
    auto resolved = daemon_checkout(std::move(checkout));
    if (!resolved) {
        co_return std::unexpected(resolved.error());
    }
    checkout = std::move(*resolved);
    auto implementation = std::make_unique<impl>();
    auto endpoint = endpoint_path(checkout);
    if (!endpoint) {
        co_return std::unexpected(endpoint.error());
    }
    implementation->endpoint = std::move(*endpoint);
    auto response = co_await runtime::offload(
        [endpoint = implementation->endpoint] { return request(endpoint, "status"); });
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (!*response) {
        co_return std::unexpected(response->error());
    }
    auto decoded = decode_status(**response);
    if (!decoded) {
        co_return std::unexpected(decoded.error());
    }
    co_return daemon{std::move(implementation)};
#endif
}

runtime::task<daemon_status> daemon::status() {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "daemon is not attached"));
    }
#ifdef _WIN32
    co_return std::unexpected(unsupported_endpoint());
#else
    auto response = co_await runtime::offload(
        [endpoint = impl_->endpoint] { return request(endpoint, "status"); });
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (!*response) {
        co_return std::unexpected(response->error());
    }
    co_return decode_status(**response);
#endif
}

runtime::task<void> daemon::shutdown() {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "daemon is not attached"));
    }
#ifdef _WIN32
    co_return std::unexpected(unsupported_endpoint());
#else
    auto response = co_await runtime::offload(
        [endpoint = impl_->endpoint] { return request(endpoint, "shutdown"); });
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (!*response) {
        co_return std::unexpected(response->error());
    }
    auto decoded = decode_payload(**response, 1);
    if (!decoded) {
        co_return std::unexpected(decoded.error());
    }
    for (auto attempt = 0; attempt != 500; ++attempt) {
        auto probe = co_await runtime::offload(
            [endpoint = impl_->endpoint] { return request(endpoint, "status"); });
        if (!probe) {
            co_return std::unexpected(probe.error());
        }
        if (!*probe &&
            (probe->error().code == errc::unavailable || probe->error().code == errc::io)) {
            co_return {};
        }
        if (!*probe) {
            co_return std::unexpected(probe->error());
        }
        auto [delay_error] = co_await runtime::delay(std::chrono::milliseconds{10});
        if (delay_error) {
            co_return std::unexpected(
                failure(errc::internal, delay_error.message(), "daemon shutdown"));
        }
    }
    co_return std::unexpected(failure(
        errc::unavailable, "daemon endpoint remained after shutdown", impl_->endpoint.string()));
#endif
}

runtime::task<void> daemon::pause_capture() {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "daemon is not attached"));
    }
#ifdef _WIN32
    co_return std::unexpected(unsupported_endpoint());
#else
    auto response = co_await runtime::offload(
        [endpoint = impl_->endpoint] { return request(endpoint, "pause"); });
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (!*response) {
        co_return std::unexpected(response->error());
    }
    auto decoded = decode_payload(**response, 1);
    if (!decoded) {
        co_return std::unexpected(decoded.error());
    }
    co_return {};
#endif
}

runtime::task<void> daemon::resume_capture() {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "daemon is not attached"));
    }
#ifdef _WIN32
    co_return std::unexpected(unsupported_endpoint());
#else
    auto response = co_await runtime::offload(
        [endpoint = impl_->endpoint] { return request(endpoint, "resume"); });
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (!*response) {
        co_return std::unexpected(response->error());
    }
    auto decoded = decode_payload(**response, 1);
    if (!decoded) {
        co_return std::unexpected(decoded.error());
    }
    co_return {};
#endif
}

runtime::task<void> daemon::report_consumer_lag(std::string name, std::uint64_t behind) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "daemon is not attached"));
    }
#ifdef _WIN32
    static_cast<void>(name);
    static_cast<void>(behind);
    co_return std::unexpected(unsupported_endpoint());
#else
    auto frame = encode_request(map({{"v", std::uint64_t{1}},
                                     {"op", "consumer_lag"},
                                     {"name", std::move(name)},
                                     {"behind", behind}}));
    if (!frame) {
        co_return std::unexpected(frame.error());
    }
    auto response = co_await runtime::offload(
        [endpoint = impl_->endpoint, request_frame = std::move(*frame)]() mutable {
            return request(endpoint, std::move(request_frame));
        });
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (!*response) {
        co_return std::unexpected(response->error());
    }
    auto decoded = decode_payload(**response, 1);
    if (!decoded) {
        co_return std::unexpected(decoded.error());
    }
    co_return {};
#endif
}

runtime::task<void> daemon::report_presence(liquid::presence value) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "daemon is not attached"));
    }
#ifdef _WIN32
    static_cast<void>(value);
    co_return std::unexpected(unsupported_endpoint());
#else
    auto frame = encode_request(
        map({{"v", std::uint64_t{1}}, {"op", "presence"}, {"presence", presence_value(value)}}));
    if (!frame) {
        co_return std::unexpected(frame.error());
    }
    auto response = co_await send_daemon_request(impl_->endpoint, std::move(*frame));
    if (!response) {
        co_return std::unexpected(response.error());
    }
    auto decoded = decode_response(*response);
    if (!decoded) {
        co_return std::unexpected(decoded.error());
    }
    co_return result<void>{};
#endif
}

runtime::task<void> daemon::lock(std::string path, std::string reason) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "daemon is not attached"));
    }
#ifdef _WIN32
    static_cast<void>(path);
    static_cast<void>(reason);
    co_return std::unexpected(unsupported_endpoint());
#else
    auto frame = encode_request(map({{"v", std::uint64_t{1}},
                                     {"op", "lock"},
                                     {"path", std::move(path)},
                                     {"reason", std::move(reason)}}));
    if (!frame) {
        co_return std::unexpected(frame.error());
    }
    auto response = co_await send_daemon_request(impl_->endpoint, std::move(*frame));
    if (!response) {
        co_return std::unexpected(response.error());
    }
    auto decoded = decode_response(*response);
    if (!decoded) {
        co_return std::unexpected(decoded.error());
    }
    co_return result<void>{};
#endif
}

runtime::task<void> daemon::unlock(std::string path) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "daemon is not attached"));
    }
#ifdef _WIN32
    static_cast<void>(path);
    co_return std::unexpected(unsupported_endpoint());
#else
    auto frame = encode_request(map(
        {{"v", std::uint64_t{1}}, {"op", "unlock"}, {"path", std::move(path)}, {"reason", ""}}));
    if (!frame) {
        co_return std::unexpected(frame.error());
    }
    auto response = co_await send_daemon_request(impl_->endpoint, std::move(*frame));
    if (!response) {
        co_return std::unexpected(response.error());
    }
    auto decoded = decode_response(*response);
    if (!decoded) {
        co_return std::unexpected(decoded.error());
    }
    co_return result<void>{};
#endif
}

runtime::task<std::vector<line_attribution>> daemon::attribution(std::string path,
                                                                 change_id change) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "daemon is not attached"));
    }
#ifdef _WIN32
    static_cast<void>(path);
    static_cast<void>(change);
    co_return std::unexpected(unsupported_endpoint());
#else
    auto frame = encode_request(map({{"v", std::uint64_t{1}},
                                     {"op", "attribution"},
                                     {"path", std::move(path)},
                                     {"change", to_string(change.value)}}));
    if (!frame) {
        co_return std::unexpected(frame.error());
    }
    auto response = co_await send_daemon_request(impl_->endpoint, std::move(*frame));
    if (!response) {
        co_return std::unexpected(response.error());
    }
    auto decoded = decode_response(*response);
    if (!decoded) {
        co_return std::unexpected(decoded.error());
    }
    const auto* lines = field(*decoded, "lines");
    if (!lines) {
        co_return std::unexpected(failure(errc::corrupt, "missing daemon attribution response"));
    }
    co_return decode_attribution_value(*lines);
#endif
}

runtime::task<build_handle> daemon::build(action_input input) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "daemon is not attached"));
    }
#ifdef _WIN32
    static_cast<void>(input);
    co_return std::unexpected(unsupported_endpoint());
#else
    auto frame = encode_request(map(
        {{"v", std::uint64_t{1}}, {"op", "build_start"}, {"input", action_input_value(input)}}));
    if (!frame) {
        co_return std::unexpected(frame.error());
    }
    auto stream = build_detail::event_access::create();
    auto producer = stream.producer;
    runtime::spawn(proxy_build_events(impl_->endpoint, std::move(*frame), producer),
                   [producer](result<void> completed) {
                       if (!completed) {
                           producer.fail(completed.error(), {});
                       }
                   });
    co_return std::move(stream.handle);
#endif
}

runtime::task<edit_preview> daemon::preview_edit(edit_request request_value,
                                                 edit_attribution attribution) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "daemon is not attached"));
    }
#ifdef _WIN32
    static_cast<void>(request_value);
    static_cast<void>(attribution);
    co_return std::unexpected(unsupported_endpoint());
#else
    auto record = encode_edit_request(request_value);
    if (!record) {
        co_return std::unexpected(record.error());
    }
    auto frame = encode_request(map({{"v", std::uint64_t{1}},
                                     {"op", "edit_preview"},
                                     {"record", cbor::value::bytes{record->begin(), record->end()}},
                                     {"tool", attribution.tool},
                                     {"operation", attribution.operation},
                                     {"agent", attribution.agent.value_or("")}}));
    if (!frame) {
        co_return std::unexpected(frame.error());
    }
    auto response = co_await send_daemon_request(impl_->endpoint, std::move(*frame));
    if (!response) {
        co_return std::unexpected(response.error());
    }
    auto decoded = decode_response(*response);
    if (!decoded) {
        co_return std::unexpected(decoded.error());
    }
    const auto* preview_value = field(*decoded, "preview");
    const auto* preview =
        preview_value ? std::get_if<cbor::value::bytes>(&preview_value->data) : nullptr;
    if (!preview) {
        co_return std::unexpected(failure(errc::corrupt, "missing daemon edit preview"));
    }
    co_return decode_edit_preview(*preview);
#endif
}

runtime::task<edit_result> daemon::apply_edit(edit_preview preview) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "daemon is not attached"));
    }
#ifdef _WIN32
    static_cast<void>(preview);
    co_return std::unexpected(unsupported_endpoint());
#else
    auto record = encode_edit_preview(preview);
    if (!record) {
        co_return std::unexpected(record.error());
    }
    auto frame =
        encode_request(map({{"v", std::uint64_t{1}},
                            {"op", "edit_apply"},
                            {"record", cbor::value::bytes{record->begin(), record->end()}}}));
    if (!frame) {
        co_return std::unexpected(frame.error());
    }
    auto response = co_await send_daemon_request(impl_->endpoint, std::move(*frame));
    if (!response) {
        co_return std::unexpected(response.error());
    }
    auto decoded = decode_response(*response);
    if (!decoded) {
        co_return std::unexpected(decoded.error());
    }
    const auto* result_value = field(*decoded, "result");
    const auto* result =
        result_value ? std::get_if<cbor::value::bytes>(&result_value->data) : nullptr;
    if (!result) {
        co_return std::unexpected(failure(errc::corrupt, "missing daemon edit result"));
    }
    co_return decode_edit_result(*result);
#endif
}

runtime::task<std::vector<build_plan_action>> daemon::build_plan(build_target target) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "daemon is not attached"));
    }
#ifdef _WIN32
    static_cast<void>(target);
    co_return std::unexpected(unsupported_endpoint());
#else
    auto frame = encode_request(map(
        {{"v", std::uint64_t{1}}, {"op", "build_plan"}, {"target", build_target_value(target)}}));
    if (!frame) {
        co_return std::unexpected(frame.error());
    }
    auto response = co_await send_daemon_request(impl_->endpoint, std::move(*frame));
    if (!response) {
        co_return std::unexpected(response.error());
    }
    auto decoded = decode_response(*response);
    if (!decoded) {
        co_return std::unexpected(decoded.error());
    }
    const auto* plan_value = field(*decoded, "plan");
    if (!plan_value) {
        co_return std::unexpected(failure(errc::corrupt, "missing daemon build plan"));
    }
    co_return decode_build_plan(*plan_value);
#endif
}

runtime::task<build_handle> daemon::build(build_target target) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "daemon is not attached"));
    }
#ifdef _WIN32
    static_cast<void>(target);
    co_return std::unexpected(unsupported_endpoint());
#else
    auto frame = encode_request(map({{"v", std::uint64_t{1}},
                                     {"op", "build_repository"},
                                     {"target", build_target_value(target)}}));
    if (!frame) {
        co_return std::unexpected(frame.error());
    }
    auto stream = build_detail::event_access::create();
    auto producer = stream.producer;
    runtime::spawn(proxy_build_events(impl_->endpoint, std::move(*frame), producer),
                   [producer](result<void> completed) {
                       if (!completed) {
                           producer.fail(completed.error(), {});
                       }
                   });
    co_return std::move(stream.handle);
#endif
}

runtime::task<void> daemon::wait() {
    if (!impl_ || !impl_->hosted) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "only a started daemon can be waited"));
    }
    while (!impl_->hosted->stopped.load(std::memory_order_acquire)) {
        auto [delay_error] = co_await runtime::delay(std::chrono::milliseconds{5});
        if (delay_error) {
            co_return std::unexpected(
                failure(errc::internal, delay_error.message(), "daemon wait"));
        }
    }
    {
        std::lock_guard lock(impl_->hosted->completion_mutex);
        if (impl_->hosted->completion_error) {
            co_return std::unexpected(*impl_->hosted->completion_error);
        }
    }
    co_return {};
}

} // namespace liquid
