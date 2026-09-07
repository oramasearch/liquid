#include "liquid/frontend.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <string_view>
#include <utility>

#include "encoding/cbor.hpp"

namespace liquid {
namespace {

namespace cbor = encoding::cbor;

constexpr auto action_query_kind = std::string_view{"frontend.action.v1"};
constexpr auto assembly_query_kind = std::string_view{"frontend.assembly.v1"};
constexpr auto symbols_query_kind = std::string_view{"symbols"};
constexpr auto parse_query_kind = std::string_view{"parse_tree"};

auto frontend_error(errc code, std::string message, std::string context) -> error {
    return error{code, std::move(message), std::move(context)};
}

auto map(std::initializer_list<std::pair<std::string, cbor::value>> entries) -> cbor::value {
    auto output = cbor::value::map{};
    for (auto& entry : entries) {
        output.emplace_back(entry.first, entry.second);
    }
    return output;
}

auto find(const cbor::value& value, std::string_view key) -> const cbor::value* {
    const auto* object = std::get_if<cbor::value::map>(&value.data);
    if (!object) {
        return nullptr;
    }
    for (const auto& [candidate, child] : *object) {
        const auto* text = std::get_if<std::string>(&candidate.data);
        if (text && *text == key) {
            return &child;
        }
    }
    return nullptr;
}

auto canonical(std::span<const std::byte> value, std::string_view context) -> result<cbor::value> {
    auto decoded = cbor::decode(value);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    auto encoded = cbor::encode(*decoded);
    if (!encoded || encoded->size() != value.size() ||
        !std::equal(encoded->begin(), encoded->end(), value.begin())) {
        return std::unexpected(frontend_error(errc::invalid_argument,
                                              "frontend context is not canonical CBOR",
                                              std::string{context}));
    }
    return decoded;
}

auto with_source(frontend_build_request& request) -> result<std::vector<std::byte>> {
    auto value = request.context_cbor.empty()
                     ? result<cbor::value>{cbor::value{cbor::value::map{}}}
                     : canonical(request.context_cbor, "frontend build context");
    if (!value) {
        return std::unexpected(value.error());
    }
    auto* object = std::get_if<cbor::value::map>(&value->data);
    if (!object) {
        return std::unexpected(frontend_error(errc::invalid_argument,
                                              "frontend build context must be a CBOR map",
                                              request.source_path));
    }
    auto put = [&](std::string name, cbor::value child) {
        for (auto& [key, current] : *object) {
            const auto* text = std::get_if<std::string>(&key.data);
            if (text && *text == name) {
                current = std::move(child);
                return;
            }
        }
        object->emplace_back(std::move(name), std::move(child));
    };
    put("source", request.source);
    put("source_path", request.source_path);
    put("compile_action", request.compile_action);
    return cbor::encode(*value);
}

auto with_parse(std::span<const std::byte> context, std::span<const std::byte> parse)
    -> result<std::vector<std::byte>> {
    auto value = context.empty() ? result<cbor::value>{cbor::value{cbor::value::map{}}}
                                 : canonical(context, "frontend context");
    if (!value) {
        return std::unexpected(value.error());
    }
    auto* object = std::get_if<cbor::value::map>(&value->data);
    if (!object) {
        return std::unexpected(frontend_error(
            errc::invalid_argument, "frontend context must be a CBOR map", "frontend"));
    }
    const auto found = std::ranges::find_if(*object, [](const auto& field) {
        const auto* name = std::get_if<std::string>(&field.first.data);
        return name && *name == "parse";
    });
    auto parse_value = cbor::value{cbor::value::bytes{parse.begin(), parse.end()}};
    if (found == object->end()) {
        object->emplace_back("parse", std::move(parse_value));
    } else {
        found->second = std::move(parse_value);
    }
    return cbor::encode(*value);
}

auto parse_arguments(std::string_view language, const digest& blob)
    -> result<std::vector<std::byte>> {
    return cbor::encode(map({{"language", std::string{language}}, {"blob", to_string(blob)}}));
}

auto parse_input_name(const digest& blob) -> std::string {
    return "frontend-blob:" + to_string(blob);
}

auto action_reads(std::span<const std::byte> action) -> result<std::vector<std::string>> {
    auto decoded = canonical(action, "frontend action");
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    const auto* reads_value = find(*decoded, "reads");
    const auto* reads = reads_value ? std::get_if<cbor::value::array>(&reads_value->data) : nullptr;
    auto output = std::vector<std::string>{};
    if (!reads) {
        return output;
    }
    for (const auto& item : *reads) {
        const auto* path = std::get_if<std::string>(&item.data);
        if (!path || path->empty()) {
            return std::unexpected(frontend_error(
                errc::corrupt, "frontend action has an invalid read path", "frontend action"));
        }
        output.push_back(*path);
    }
    std::ranges::sort(output);
    output.erase(std::unique(output.begin(), output.end()), output.end());
    return output;
}

auto action_arguments(std::span<const std::byte> action) -> result<std::vector<std::byte>> {
    auto reads = action_reads(action);
    if (!reads) {
        return std::unexpected(reads.error());
    }
    auto read_values = cbor::value::array{};
    for (auto& path : *reads) {
        read_values.emplace_back(std::move(path));
    }
    return cbor::encode(map({
        {"action", cbor::value::bytes{action.begin(), action.end()}},
        {"reads", std::move(read_values)},
    }));
}

auto symbols_arguments(const frontend_build_request& request,
                       std::span<const std::byte> context,
                       std::span<const std::byte> parse_key) -> result<std::vector<std::byte>> {
    return cbor::encode(map({
        {"language", request.language},
        {"source", cbor::value::bytes{request.source.begin(), request.source.end()}},
        {"context", cbor::value::bytes{context.begin(), context.end()}},
        {"compile_action", request.compile_action},
        {"parse", cbor::value::bytes{parse_key.begin(), parse_key.end()}},
    }));
}

auto action_kind(std::span<const std::byte> action) -> result<std::optional<std::string>> {
    auto decoded = canonical(action, "frontend action");
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    const auto* value = find(*decoded, "kind");
    const auto* kind = value ? std::get_if<std::string>(&value->data) : nullptr;
    return kind ? std::optional<std::string>{*kind} : std::nullopt;
}

struct query_state {
    std::shared_ptr<frontend_module> module;
    frontend_action_executor execute;
    std::atomic_uint64_t actions{};
    std::atomic_uint64_t assemblies{};
    std::atomic_uint64_t parses{};
};

auto execute_parse_query(std::shared_ptr<query_state> state,
                         query_context& context,
                         std::vector<std::byte> arguments)
    -> runtime::task<std::vector<std::byte>> {
    auto decoded = canonical(arguments, "parse query");
    if (!decoded) {
        co_return std::unexpected(decoded.error());
    }
    const auto* language_value = find(*decoded, "language");
    const auto* blob_value = find(*decoded, "blob");
    const auto* language =
        language_value ? std::get_if<std::string>(&language_value->data) : nullptr;
    const auto* blob_text = blob_value ? std::get_if<std::string>(&blob_value->data) : nullptr;
    auto blob = blob_text ? parse_digest(*blob_text)
                          : result<digest>{std::unexpected(frontend_error(
                                errc::corrupt, "parse query has no blob digest", "parse"))};
    if (!language || language->empty() || !blob) {
        co_return std::unexpected(
            blob ? frontend_error(errc::corrupt, "parse query has no language", "parse")
                 : blob.error());
    }
    auto source = co_await context.read(
        graph_input_key{graph_input_kind::overlay_buffer, parse_input_name(*blob)});
    if (!source) {
        co_return std::unexpected(source.error());
    }
    auto actual = hash(algorithm::blake3, source->bytes);
    if (!actual) {
        co_return std::unexpected(actual.error());
    }
    if (actual->alg != blob->alg || actual->bytes != blob->bytes) {
        co_return std::unexpected(frontend_error(
            errc::corrupt, "parse input does not match its blob digest", to_string(*blob)));
    }
    ++state->parses;
    auto parsed = co_await runtime::offload(
        [module = state->module,
         language = *language,
         source = std::move(source->bytes)]() mutable { return module->parse(language, source); });
    if (!parsed) {
        co_return std::unexpected(parsed.error());
    }
    if (!*parsed) {
        co_return std::unexpected(parsed->error());
    }
    co_return std::move(**parsed);
}

auto execute_action_query(std::shared_ptr<query_state> state,
                          query_context& context,
                          std::vector<std::byte> arguments)
    -> runtime::task<std::vector<std::byte>> {
    auto decoded = canonical(arguments, "frontend action query");
    if (!decoded) {
        co_return std::unexpected(
            frontend_error(decoded.error().code,
                           "frontend action arguments: " + decoded.error().message,
                           "frontend action"));
    }
    const auto* action_value = find(*decoded, "action");
    const auto* reads_value = find(*decoded, "reads");
    const auto* action =
        action_value ? std::get_if<cbor::value::bytes>(&action_value->data) : nullptr;
    const auto* reads = reads_value ? std::get_if<cbor::value::array>(&reads_value->data) : nullptr;
    if (!action || !reads) {
        co_return std::unexpected(
            frontend_error(errc::corrupt, "frontend action query is invalid", "frontend action"));
    }
    for (const auto& item : *reads) {
        const auto* path = std::get_if<std::string>(&item.data);
        if (!path) {
            co_return std::unexpected(
                frontend_error(errc::corrupt, "frontend read path is invalid", "frontend action"));
        }
        auto dependency =
            co_await context.read(graph_input_key{graph_input_kind::working_tree_blob, *path});
        if (!dependency) {
            co_return std::unexpected(dependency.error());
        }
    }
    ++state->actions;
    auto output = co_await state->execute(*action);
    if (!output) {
        co_return std::unexpected(
            frontend_error(output.error().code,
                           "frontend action execution: " + output.error().message,
                           "frontend action"));
    }
    co_return std::move(*output);
}

auto execute_assembly_query(std::shared_ptr<query_state> state,
                            query_context& context,
                            std::vector<std::byte> arguments)
    -> runtime::task<std::vector<std::byte>> {
    auto decoded = canonical(arguments, "frontend assembly query");
    if (!decoded) {
        co_return std::unexpected(decoded.error());
    }
    const auto* actions_value = find(*decoded, "actions");
    const auto* context_value = find(*decoded, "context");
    const auto* actions =
        actions_value ? std::get_if<cbor::value::array>(&actions_value->data) : nullptr;
    const auto* context_bytes =
        context_value ? std::get_if<cbor::value::bytes>(&context_value->data) : nullptr;
    if (!actions || !context_bytes) {
        co_return std::unexpected(frontend_error(
            errc::corrupt, "frontend assembly query is invalid", "frontend assembly"));
    }
    auto unit_outputs = cbor::value::array{};
    for (const auto& item : *actions) {
        const auto* key_value = find(item, "key");
        const auto* fingerprint_value = find(item, "fingerprint");
        const auto* key_bytes =
            key_value ? std::get_if<cbor::value::bytes>(&key_value->data) : nullptr;
        const auto* fingerprint =
            fingerprint_value ? std::get_if<std::string>(&fingerprint_value->data) : nullptr;
        if (!key_bytes || !fingerprint) {
            co_return std::unexpected(frontend_error(
                errc::corrupt, "frontend assembly action is invalid", "frontend assembly"));
        }
        auto output = co_await context.read(query_key{std::string{action_query_kind}, *key_bytes});
        if (!output) {
            co_return std::unexpected(output.error());
        }
        unit_outputs.emplace_back(map({
            {"unit", *fingerprint},
            {"output", std::move(output->bytes)},
        }));
    }
    auto encoded_outputs = cbor::encode(cbor::value{std::move(unit_outputs)});
    if (!encoded_outputs) {
        co_return std::unexpected(encoded_outputs.error());
    }
    ++state->assemblies;
    auto assembled = co_await runtime::offload([module = state->module,
                                                outputs = std::move(*encoded_outputs),
                                                assembly_context = *context_bytes]() mutable {
        return module->assemble(outputs, assembly_context);
    });
    if (!assembled) {
        co_return std::unexpected(assembled.error());
    }
    if (!*assembled) {
        co_return std::unexpected(assembled->error());
    }
    co_return std::move(**assembled);
}

auto execute_symbols_query(std::shared_ptr<query_state> state,
                           query_context& context,
                           std::vector<std::byte> arguments)
    -> runtime::task<std::vector<std::byte>> {
    auto decoded = canonical(arguments, "symbols query");
    if (!decoded) {
        co_return std::unexpected(decoded.error());
    }
    const auto* language_value = find(*decoded, "language");
    const auto* source_value = find(*decoded, "source");
    const auto* context_value = find(*decoded, "context");
    const auto* parse_value = find(*decoded, "parse");
    const auto* language =
        language_value ? std::get_if<std::string>(&language_value->data) : nullptr;
    const auto* source =
        source_value ? std::get_if<cbor::value::bytes>(&source_value->data) : nullptr;
    const auto* context_bytes =
        context_value ? std::get_if<cbor::value::bytes>(&context_value->data) : nullptr;
    const auto* parse_key =
        parse_value ? std::get_if<cbor::value::bytes>(&parse_value->data) : nullptr;
    if (!language || !source || !context_bytes || !parse_key) {
        co_return std::unexpected(
            frontend_error(errc::corrupt, "symbols query is invalid", "symbols"));
    }
    auto parse = co_await context.read(query_key{std::string{parse_query_kind}, *parse_key});
    if (!parse) {
        co_return std::unexpected(parse.error());
    }
    auto derived_context = with_parse(*context_bytes, parse->bytes);
    if (!derived_context) {
        co_return std::unexpected(derived_context.error());
    }
    auto output = state->module->symbols(*language, *source, *derived_context);
    if (!output) {
        co_return std::unexpected(output.error());
    }
    auto kind = action_kind(*output);
    if (!kind) {
        co_return std::unexpected(kind.error());
    }
    if (*kind && **kind == "clang-symbols") {
        auto arguments = action_arguments(*output);
        if (!arguments) {
            co_return std::unexpected(arguments.error());
        }
        auto action_output =
            co_await context.read(query_key{std::string{action_query_kind}, std::move(*arguments)});
        if (!action_output) {
            co_return std::unexpected(
                frontend_error(action_output.error().code,
                               "symbol action query: " + action_output.error().message,
                               "symbols"));
        }
        output = state->module->symbols_from_output(*language, *source, action_output->bytes);
        if (!output) {
            co_return std::unexpected(
                frontend_error(output.error().code,
                               "symbol compiler output: " + output.error().message,
                               "symbols"));
        }
    }
    auto subject = hash(algorithm::blake3, *source);
    if (!subject) {
        co_return std::unexpected(subject.error());
    }
    auto symbols = decode_frontend_symbols(*subject, *output);
    if (!symbols) {
        co_return std::unexpected(frontend_error(
            symbols.error().code, "symbol record: " + symbols.error().message, "symbols"));
    }
    auto encoded = encode_symbols(*symbols);
    if (!encoded) {
        co_return std::unexpected(frontend_error(
            encoded.error().code, "symbol record encoding: " + encoded.error().message, "symbols"));
    }
    co_return std::move(*encoded);
}

} // namespace

struct frontend_graph::impl {
    graph subject;
    std::shared_ptr<query_state> state;
};

frontend_graph::frontend_graph() noexcept = default;
frontend_graph::frontend_graph(std::unique_ptr<impl> implementation)
    : impl_(std::move(implementation)) {}
frontend_graph::~frontend_graph() = default;
frontend_graph::frontend_graph(frontend_graph&&) noexcept = default;
auto frontend_graph::operator=(frontend_graph&&) noexcept -> frontend_graph& = default;

runtime::task<frontend_graph> frontend_graph::open(std::filesystem::path state_directory,
                                                   std::filesystem::path working_tree,
                                                   std::shared_ptr<frontend_module> module,
                                                   frontend_action_executor execute) {
    if (!module || !execute) {
        co_return std::unexpected(frontend_error(
            errc::invalid_argument, "frontend graph requires a module and executor", "frontend"));
    }
    auto state = std::make_shared<query_state>();
    state->module = std::move(module);
    state->execute = std::move(execute);
    auto registry = query_registry{};
    auto parse_added =
        registry.add({.name = std::string{parse_query_kind},
                      .compute = [state](query_context& context, std::vector<std::byte> arguments) {
                          return execute_parse_query(state, context, std::move(arguments));
                      }});
    if (!parse_added) {
        co_return std::unexpected(parse_added.error());
    }
    auto action_added =
        registry.add({.name = std::string{action_query_kind},
                      .compute = [state](query_context& context, std::vector<std::byte> arguments) {
                          return execute_action_query(state, context, std::move(arguments));
                      }});
    if (!action_added) {
        co_return std::unexpected(action_added.error());
    }
    auto assembly_added =
        registry.add({.name = std::string{assembly_query_kind},
                      .compute = [state](query_context& context, std::vector<std::byte> arguments) {
                          return execute_assembly_query(state, context, std::move(arguments));
                      }});
    if (!assembly_added) {
        co_return std::unexpected(assembly_added.error());
    }
    auto symbols_added =
        registry.add({.name = std::string{symbols_query_kind},
                      .compute = [state](query_context& context, std::vector<std::byte> arguments) {
                          return execute_symbols_query(state, context, std::move(arguments));
                      }});
    if (!symbols_added) {
        co_return std::unexpected(symbols_added.error());
    }
    auto opened = co_await graph::open(
        std::move(state_directory), std::move(registry), {.working_tree = std::move(working_tree)});
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    auto implementation = std::make_unique<impl>();
    implementation->subject = std::move(*opened);
    implementation->state = std::move(state);
    co_return frontend_graph{std::move(implementation)};
}

runtime::task<frontend_build_report> frontend_graph::build(frontend_build_request request) {
    if (!impl_) {
        co_return std::unexpected(
            frontend_error(errc::invalid_argument, "frontend graph is empty", "frontend"));
    }
    auto context = with_source(request);
    if (!context) {
        co_return std::unexpected(context.error());
    }
    auto blob = hash(algorithm::blake3, request.source);
    if (!blob) {
        co_return std::unexpected(blob.error());
    }
    auto parsed = co_await parse({request.language, *blob, request.source});
    if (!parsed) {
        co_return std::unexpected(parsed.error());
    }
    auto parse_key = parse_arguments(request.language, *blob);
    if (!parse_key) {
        co_return std::unexpected(parse_key.error());
    }
    auto parse_value = co_await impl_->subject.demand(
        query_key{std::string{parse_query_kind}, std::move(*parse_key)});
    if (!parse_value) {
        co_return std::unexpected(parse_value.error());
    }
    auto derived_context = with_parse(*context, parse_value->bytes);
    if (!derived_context) {
        co_return std::unexpected(derived_context.error());
    }
    auto units = impl_->state->module->units(request.language, request.source, *derived_context);
    if (!units) {
        co_return std::unexpected(units.error());
    }
    auto report = frontend_build_report{};
    auto assembly_actions = cbor::value::array{};
    const auto actions_before = impl_->state->actions.load();
    const auto assemblies_before = impl_->state->assemblies.load();
    for (auto& unit : *units) {
        if (unit.kind != frontend_unit_kind::definition) {
            continue;
        }
        auto action = impl_->state->module->compile_unit(unit, *derived_context);
        if (!action) {
            co_return std::unexpected(action.error());
        }
        auto arguments = action_arguments(*action);
        if (!arguments) {
            co_return std::unexpected(arguments.error());
        }
        const auto action_count = impl_->state->actions.load();
        auto output =
            co_await impl_->subject.demand(query_key{std::string{action_query_kind}, *arguments});
        if (!output) {
            co_return std::unexpected(output.error());
        }
        const auto ran = impl_->state->actions.load() != action_count;
        assembly_actions.emplace_back(map({
            {"fingerprint", to_string(unit.fingerprint)},
            {"key", cbor::value::bytes{arguments->begin(), arguments->end()}},
        }));
        report.units.push_back(frontend_unit_output{
            .unit = std::move(unit),
            .action = std::move(*action),
            .output = std::move(output->bytes),
            .action_ran = ran,
        });
    }
    auto assembly_arguments = cbor::encode(map({
        {"actions", std::move(assembly_actions)},
        {"context", cbor::value::bytes{context->begin(), context->end()}},
    }));
    if (!assembly_arguments) {
        co_return std::unexpected(assembly_arguments.error());
    }
    auto assembled = co_await impl_->subject.demand(
        query_key{std::string{assembly_query_kind}, std::move(*assembly_arguments)});
    if (!assembled) {
        co_return std::unexpected(assembled.error());
    }
    report.translation_unit_output = std::move(assembled->bytes);
    report.actions_run = impl_->state->actions.load() - actions_before;
    report.assemblies_run = impl_->state->assemblies.load() - assemblies_before;
    co_return report;
}

runtime::task<symbol_record> frontend_graph::symbols(frontend_build_request request) {
    if (!impl_) {
        co_return std::unexpected(
            frontend_error(errc::invalid_argument, "frontend graph is empty", "frontend"));
    }
    auto context = with_source(request);
    if (!context) {
        co_return std::unexpected(context.error());
    }
    auto blob = hash(algorithm::blake3, request.source);
    if (!blob) {
        co_return std::unexpected(blob.error());
    }
    auto parse_key = parse_arguments(request.language, *blob);
    if (!parse_key) {
        co_return std::unexpected(parse_key.error());
    }
    auto input = co_await impl_->subject.set_input(
        graph_input_key{graph_input_kind::overlay_buffer, parse_input_name(*blob)},
        request.source,
        durability::high);
    if (!input) {
        co_return std::unexpected(input.error());
    }
    auto arguments = symbols_arguments(request, *context, *parse_key);
    if (!arguments) {
        co_return std::unexpected(arguments.error());
    }
    auto value = co_await impl_->subject.demand(
        query_key{std::string{symbols_query_kind}, std::move(*arguments)});
    if (!value) {
        co_return std::unexpected(value.error());
    }
    co_return decode_symbols(value->bytes);
}

runtime::task<parse_tree> frontend_graph::parse(frontend_parse_request request) {
    if (!impl_) {
        co_return std::unexpected(
            frontend_error(errc::invalid_argument, "frontend graph is empty", "frontend"));
    }
    auto actual = hash(algorithm::blake3, request.source);
    if (!actual) {
        co_return std::unexpected(actual.error());
    }
    if (actual->alg != request.blob.alg || actual->bytes != request.blob.bytes) {
        co_return std::unexpected(frontend_error(errc::invalid_argument,
                                                 "parse source does not match blob digest",
                                                 to_string(request.blob)));
    }
    auto input = co_await impl_->subject.set_input(
        graph_input_key{graph_input_kind::overlay_buffer, parse_input_name(request.blob)},
        std::move(request.source),
        durability::high);
    if (!input) {
        co_return std::unexpected(input.error());
    }
    auto arguments = parse_arguments(request.language, request.blob);
    if (!arguments) {
        co_return std::unexpected(arguments.error());
    }
    auto value = co_await impl_->subject.demand(
        query_key{std::string{parse_query_kind}, std::move(*arguments)});
    if (!value) {
        co_return std::unexpected(value.error());
    }
    co_return decode_parse_tree(value->bytes);
}

auto frontend_graph::language_for_path(std::string_view path) const -> result<frontend_language> {
    if (!impl_) {
        return std::unexpected(
            frontend_error(errc::invalid_argument, "frontend graph is empty", "frontend"));
    }
    const auto filename = std::filesystem::path{path}.filename().string();
    const auto extension = std::filesystem::path{path}.extension().string();
    for (auto& language : impl_->state->module->language_details()) {
        if (std::ranges::find(language.extensions, extension) != language.extensions.end() ||
            std::ranges::find(language.extensions, filename) != language.extensions.end()) {
            return language;
        }
    }
    return std::unexpected(frontend_error(
        errc::not_found, "no grammar is registered for the path", std::string{path}));
}

auto frontend_graph::parse_count() const -> std::uint64_t {
    return impl_ ? impl_->state->parses.load() : 0;
}

auto frontend_graph::mark_changed(std::string path, durability tier) -> result<std::uint64_t> {
    if (!impl_) {
        return std::unexpected(
            frontend_error(errc::invalid_argument, "frontend graph is empty", "frontend"));
    }
    return impl_->subject.mark_changed(
        graph_input_key{graph_input_kind::working_tree_blob, std::move(path)}, tier);
}

auto frontend_graph::statistics() const -> graph_statistics {
    return impl_ ? impl_->subject.statistics() : graph_statistics{};
}

} // namespace liquid
