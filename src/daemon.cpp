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
#include "daemon/fanout.hpp"
#include "encoding/cbor.hpp"
#include "liquid/graph.hpp"
#include "liquid/hash.hpp"
#include "liquid/repository.hpp"
#include "search/index.hpp"

namespace liquid {
namespace {

namespace cbor = encoding::cbor;

constexpr auto maximum_frame = std::uint32_t{16U * 1024U * 1024U};

auto failure(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto map(std::initializer_list<std::pair<cbor::value, cbor::value>> fields) -> cbor::value {
    return cbor::value{cbor::value::map{fields}};
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

auto strings_value(const std::vector<std::string>& values) -> cbor::value {
    auto output = cbor::value::array{};
    output.reserve(values.size());
    for (const auto& value : values) {
        output.emplace_back(value);
    }
    return cbor::value{std::move(output)};
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
                               {"last_error", cbor::value{target.last_error.value_or("")}}}));
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
        const auto* message = text_field(*decoded, "message");
        const auto* context = text_field(*decoded, "context");
        return std::unexpected(failure(errc::internal,
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
    const auto* invalidated =
        invalidated_value ? std::get_if<cbor::value::array>(&invalidated_value->data) : nullptr;
    const auto* targets =
        targets_value ? std::get_if<cbor::value::array>(&targets_value->data) : nullptr;
    if (!running || !pid || !endpoint || !last_drip || !invalidated || !targets || !graph_keys ||
        !graph_values || !graph_memory || !graph_ceiling || !graph_validation_backlog ||
        !last_graph_divergence_value || !capture_state || !capture_pending || !capture_overflow ||
        !capture_recrawl || !capture_cursor) {
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
        if (!name || !cursor || !behind || !deliveries || !last_error) {
            return std::unexpected(failure(errc::corrupt, "invalid daemon target status"));
        }
        output.targets.push_back({.name = *name,
                                  .cursor = {*cursor},
                                  .behind = *behind,
                                  .deliveries = *deliveries,
                                  .last_error = last_error->empty()
                                                    ? std::optional<std::string>{}
                                                    : std::optional<std::string>{*last_error}});
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
        repository source;
        build_handle handle;
        build_event_subscription subscription;
    };
    std::map<std::uint64_t, build_session> builds;
    std::uint64_t next_build{};
    std::mutex queue_mutex;
    std::mutex completion_mutex;
    std::deque<daemon_delivery> queue;
    std::optional<error> completion_error;
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
    auto capture_cursor = journal_cursor{};
    auto current_capture = capture_status{};
    if (state->capture_handle) {
        auto status = state->capture_handle->status();
        if (status) {
            current_capture = *status;
            capture_cursor = status->last_cursor;
        }
    }
    const auto graph_status = state->query_graph.statistics();
    const auto validation_status = state->query_graph.validation_status();
#ifdef _WIN32
    const auto process_id = std::uint64_t{};
#else
    const auto process_id = static_cast<std::uint64_t>(::getpid());
#endif
    return {.running = !state->stopped.load(std::memory_order_acquire),
            .process_id = process_id,
            .endpoint = state->endpoint.string(),
            .last_drip = state->fanout->last_drip(),
            .invalidated_paths = state->fanout->invalidated_paths(),
            .targets = state->fanout->target_status(capture_cursor),
            .capture = std::move(current_capture),
            .graph_keys = graph_status.keys,
            .graph_values = graph_status.resident_values,
            .graph_memory_bytes = graph_status.resident_value_bytes,
            .graph_memory_ceiling = graph_status.memory_ceiling,
            .graph_validation_backlog = validation_status.backlog,
            .last_graph_divergence = validation_status.last_divergence};
}

auto service_state::enqueue(daemon_delivery delivery) -> void {
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
runtime::task<std::vector<std::byte>>
process_build_request(const std::shared_ptr<service_state>& state,
                      const cbor::value& request_value,
                      std::string_view operation) {
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
        auto handle = source->build(std::move(*input));
        if (!handle) {
            co_return encode_error(handle.error());
        }
        auto subscription = handle->events();
        if (!subscription) {
            co_return encode_error(subscription.error());
        }
        const auto id = ++state->next_build;
        state->builds.emplace(id,
                              service_state::build_session{std::move(*source),
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
            } else if (*operation == "build_start" || *operation == "build_next") {
                response = co_await process_build_request(state, *decoded, *operation);
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
        checkout = std::filesystem::weakly_canonical(std::filesystem::absolute(checkout));
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
        auto targets = std::vector<std::shared_ptr<daemon_target>>{};
        targets.push_back(std::make_shared<graph_target>(state->query_graph));
        if (state->options.targets.empty()) {
            targets.push_back(search_detail::make_daemon_target(state->checkout));
            targets.push_back(std::make_shared<no_op_target>("collaborators"));
            targets.push_back(std::make_shared<no_op_target>("lake"));
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
    checkout = std::filesystem::weakly_canonical(std::filesystem::absolute(checkout));
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
