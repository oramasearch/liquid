#include "liquid/frontend.hpp"

#include <algorithm>
#include <atomic>
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

auto symbols_arguments(const frontend_build_request& request, std::span<const std::byte> context)
    -> result<std::vector<std::byte>> {
    return cbor::encode(map({
        {"language", request.language},
        {"source", cbor::value::bytes{request.source.begin(), request.source.end()}},
        {"context", cbor::value::bytes{context.begin(), context.end()}},
        {"compile_action", request.compile_action},
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
};

auto execute_action_query(std::shared_ptr<query_state> state,
                          query_context& context,
                          std::vector<std::byte> arguments)
    -> runtime::task<std::vector<std::byte>> {
    auto decoded = canonical(arguments, "frontend action query");
    if (!decoded) {
        co_return std::unexpected(decoded.error());
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
    co_return co_await state->execute(*action);
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
    const auto* language =
        language_value ? std::get_if<std::string>(&language_value->data) : nullptr;
    const auto* source =
        source_value ? std::get_if<cbor::value::bytes>(&source_value->data) : nullptr;
    const auto* context_bytes =
        context_value ? std::get_if<cbor::value::bytes>(&context_value->data) : nullptr;
    if (!language || !source || !context_bytes) {
        co_return std::unexpected(
            frontend_error(errc::corrupt, "symbols query is invalid", "symbols"));
    }
    auto output = state->module->symbols(*language, *source, *context_bytes);
    if (!output) {
        co_return std::unexpected(output.error());
    }
    auto kind = action_kind(*output);
    if (!kind) {
        co_return std::unexpected(kind.error());
    }
    if (state->module->name() == "clang-cpp") {
        if (!*kind || **kind != "clang-symbols") {
            co_return std::unexpected(frontend_error(
                errc::corrupt, "clang frontend did not describe a symbol action", "symbols"));
        }
        auto arguments = action_arguments(*output);
        if (!arguments) {
            co_return std::unexpected(arguments.error());
        }
        auto action_output =
            co_await context.read(query_key{std::string{action_query_kind}, std::move(*arguments)});
        if (!action_output) {
            co_return std::unexpected(action_output.error());
        }
        output = state->module->symbols_from_output(*language, *source, action_output->bytes);
        if (!output) {
            co_return std::unexpected(output.error());
        }
    }
    auto subject = hash(algorithm::blake3, *source);
    if (!subject) {
        co_return std::unexpected(subject.error());
    }
    auto symbols = decode_frontend_symbols(*subject, *output);
    if (!symbols) {
        co_return std::unexpected(symbols.error());
    }
    auto encoded = encode_symbols(*symbols);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
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
    auto units = impl_->state->module->units(request.language, request.source, *context);
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
        auto action = impl_->state->module->compile_unit(unit, *context);
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
    auto arguments = symbols_arguments(request, *context);
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
