// SPDX-License-Identifier: MIT

#include "liquid/configuration.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <set>
#include <utility>

#include "build/content.hpp"
#include "build/relocation.hpp"
#include "build/repository_access.hpp"
#include "encoding/cbor.hpp"
#include "liquid/repository.hpp"
#include "net/http_client.hpp"
#include "objects/records.hpp"

namespace liquid {
namespace {

namespace cbor = encoding::cbor;

auto configuration_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto current_platform() -> platform {
#if defined(__APPLE__)
    auto os = std::string{"macos"};
#elif defined(__linux__)
    auto os = std::string{"linux"};
#elif defined(_WIN32)
    auto os = std::string{"windows"};
#else
    auto os = std::string{"unknown"};
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    auto arch = std::string{"arm64"};
#elif defined(__x86_64__) || defined(_M_X64)
    auto arch = std::string{"x86_64"};
#else
    auto arch = std::string{"unknown"};
#endif
    return {.os = std::move(os), .arch = std::move(arch)};
}

auto find_value(const cbor::value& source, std::string_view name) -> const cbor::value* {
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

auto generated_records(std::span<const std::byte> bytes,
                       digest result_value,
                       std::filesystem::path origin) -> result<lowered_configuration> {
    auto decoded = cbor::decode(bytes);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    auto canonical = cbor::encode(*decoded);
    if (!canonical || !std::ranges::equal(*canonical, bytes)) {
        return std::unexpected(configuration_error(
            errc::corrupt, "generator output is not canonical CBOR", origin.generic_string()));
    }
    const auto* records = std::get_if<cbor::value::array>(&decoded->data);
    if (!records) {
        return std::unexpected(configuration_error(
            errc::corrupt, "generator CBOR output is not a record array", origin.generic_string()));
    }
    auto output = lowered_configuration{};
    const auto generated = configuration_record_origin{.kind = configuration_origin_kind::generated,
                                                       .file = std::move(origin),
                                                       .result = result_value};
    for (const auto& record : *records) {
        const auto* raw = std::get_if<cbor::value::bytes>(&record.data);
        if (!raw) {
            return std::unexpected(configuration_error(
                errc::corrupt, "generator record set contains a non-byte record", "generator"));
        }
        auto body = cbor::decode(*raw);
        const auto* kind_value = body ? find_value(*body, "kind") : nullptr;
        const auto* kind = kind_value ? std::get_if<std::string>(&kind_value->data) : nullptr;
        if (!kind) {
            return std::unexpected(
                configuration_error(errc::corrupt, "generator record has no kind", "generator"));
        }
        if (*kind == "package") {
            auto package = decode_package(*raw);
            if (!package) {
                return std::unexpected(package.error());
            }
            output.packages.push_back(std::move(*package));
            output.package_origins.push_back(generated);
        } else if (*kind == "target") {
            auto target_value = decode_target(*raw);
            if (!target_value) {
                return std::unexpected(target_value.error());
            }
            target_value->origin = generated;
            output.targets.push_back(std::move(*target_value));
        } else if (*kind == "source") {
            auto source_value = decode_source(*raw);
            if (!source_value) {
                return std::unexpected(source_value.error());
            }
            source_value->origin = generated;
            output.sources.push_back(std::move(*source_value));
        } else {
            return std::unexpected(
                configuration_error(errc::corrupt,
                                    "generator emitted unsupported record kind '" + *kind + "'",
                                    "generator"));
        }
    }
    return output;
}

auto append_generated(lowered_configuration& into,
                      lowered_configuration generated,
                      digest result_value,
                      const std::filesystem::path& origin) -> result<void> {
    const auto record_origin = configuration_record_origin{
        .kind = configuration_origin_kind::generated, .file = origin, .result = result_value};
    for (auto& package_origin : generated.package_origins) {
        package_origin = record_origin;
    }
    for (auto& target_value : generated.targets) {
        target_value.origin = record_origin;
    }
    for (auto& source_value : generated.sources) {
        source_value.origin = record_origin;
    }
    if (!generated.generators.empty()) {
        return std::unexpected(
            configuration_error(errc::invalid_argument,
                                "generated configuration cannot declare another generator",
                                "generator"));
    }
    into.packages.insert(into.packages.end(),
                         std::make_move_iterator(generated.packages.begin()),
                         std::make_move_iterator(generated.packages.end()));
    into.package_origins.insert(into.package_origins.end(),
                                std::make_move_iterator(generated.package_origins.begin()),
                                std::make_move_iterator(generated.package_origins.end()));
    into.targets.insert(into.targets.end(),
                        std::make_move_iterator(generated.targets.begin()),
                        std::make_move_iterator(generated.targets.end()));
    into.sources.insert(into.sources.end(),
                        std::make_move_iterator(generated.sources.begin()),
                        std::make_move_iterator(generated.sources.end()));
    into.locks.rules.insert(into.locks.rules.end(),
                            std::make_move_iterator(generated.locks.rules.begin()),
                            std::make_move_iterator(generated.locks.rules.end()));
    return {};
}

auto execute_generator(repository& source, const configuration_generator& generator)
    -> runtime::task<std::pair<result_id, std::vector<std::byte>>> {
    auto id = co_await build::execute(source,
                                      {.command = generator.command,
                                       .cwd = generator.cwd,
                                       .env = generator.environment,
                                       .outputs = generator.declared_outputs,
                                       .inputs = generator.inputs,
                                       .toolchain = generator.toolchain,
                                       .platform = current_platform()});
    if (!id) {
        co_return std::unexpected(id.error());
    }
    auto provenance = co_await build::verify_provenance(source, *id);
    if (!provenance) {
        co_return std::unexpected(provenance.error());
    }
    if (!provenance->verified) {
        co_return std::unexpected(configuration_error(
            errc::corrupt, "generator result provenance did not verify", provenance->reason));
    }
    const auto selected =
        std::ranges::find(provenance->value.outputs, generator.output, &action_output::path);
    if (selected == provenance->value.outputs.end()) {
        co_return std::unexpected(configuration_error(
            errc::not_found, "generator did not produce declared output", generator.output));
    }
    auto contents = co_await repository_detail::build_access::get(source, selected->blob);
    if (!contents) {
        co_return std::unexpected(contents.error());
    }
    co_return std::pair{*id, std::move(*contents)};
}

auto source_input(repository& repository,
                  const lowered_configuration& configuration,
                  std::span<const digest> requested) -> runtime::task<std::optional<named_input>> {
    if (requested.empty()) {
        co_return std::optional<named_input>{};
    }
    auto outputs = std::vector<action_output>{};
    auto seen = std::set<std::string>{};
    for (const auto& content : requested) {
        const auto content_text = to_string(content);
        if (!seen.insert(content_text).second) {
            continue;
        }
        const auto declared = std::ranges::find_if(configuration.sources, [&](const auto& value) {
            return to_string(value.content) == content_text;
        });
        if (declared == configuration.sources.end()) {
            co_return std::unexpected(configuration_error(
                errc::invalid_argument, "target source digest is not declared", content_text));
        }
        auto fetched = co_await repository.fetch_source(*declared);
        if (!fetched) {
            co_return std::unexpected(fetched.error());
        }
        auto contents = co_await repository_detail::build_access::get(repository, *fetched);
        if (!contents) {
            co_return std::unexpected(contents.error());
        }
        const auto size = contents->size();
        auto encoded = objects::encode(
            blob{.chunks = {*fetched}, .size = size, .chunk_sizes = {size}, .content = *fetched});
        if (!encoded) {
            co_return std::unexpected(encoded.error());
        }
        auto blob_record =
            co_await repository_detail::build_access::put_record(repository, std::move(*encoded));
        if (!blob_record) {
            co_return std::unexpected(blob_record.error());
        }
        auto path = content_text;
        std::ranges::replace(path, ':', '/');
        outputs.push_back({.path = std::move(path), .mode = 0100444, .blob = *blob_record});
    }
    auto tree = co_await build_detail::store_output_tree(repository, outputs);
    if (!tree) {
        co_return std::unexpected(tree.error());
    }
    co_return std::optional<named_input>{named_input{"__liquid_sources", *tree}};
}

auto require_relocatable_result(repository& source, result_id id) -> runtime::task<result_id> {
    auto value = co_await build::result(source, id);
    if (!value) {
        co_return std::unexpected(value.error());
    }
    auto relocation = co_await build_detail::relocation_check(source, value->outputs, std::nullopt);
    if (!relocation) {
        co_return std::unexpected(relocation.error());
    }
    if (!relocation->relocatable) {
        co_return std::unexpected(build_detail::relocation_failure(*relocation));
    }
    co_return id;
}

auto build_named(repository& source,
                 const lowered_configuration& configuration,
                 const target& selected,
                 std::map<std::string, result_id>& completed,
                 std::set<std::string>& active) -> runtime::task<result_id> {
    if (const auto found = completed.find(selected.name); found != completed.end()) {
        co_return found->second;
    }
    if (!active.insert(selected.name).second) {
        co_return std::unexpected(
            configuration_error(errc::invalid_argument, "target dependency cycle", selected.name));
    }
    auto dependency_inputs = std::vector<named_input>{};
    auto sources = co_await source_input(source, configuration, selected.sources);
    if (!sources) {
        co_return std::unexpected(sources.error());
    }
    if (*sources) {
        dependency_inputs.push_back(std::move(**sources));
    }
    for (const auto& dependency : selected.dependencies) {
        if (dependency.when) {
            continue;
        }
        const auto found = std::ranges::find(configuration.targets, dependency.name, &target::name);
        if (found == configuration.targets.end()) {
            co_return std::unexpected(configuration_error(
                errc::not_found, "target dependency is not declared", dependency.name));
        }
        auto built = co_await build_named(source, configuration, *found, completed, active);
        if (!built) {
            co_return std::unexpected(built.error());
        }
        auto value = co_await build::result(source, *built);
        if (!value) {
            co_return std::unexpected(value.error());
        }
        if (!value->output_tree) {
            co_return std::unexpected(configuration_error(
                errc::corrupt, "dependency result has no output tree", dependency.name));
        }
        dependency_inputs.push_back({dependency.name, *value->output_tree});
    }
    auto input_tree = selected.inputs;
    if (!input_tree) {
        auto root = co_await source.root_digest();
        if (!root) {
            co_return std::unexpected(root.error());
        }
        input_tree = *root;
    }
    if (!selected.toolchain) {
        co_return std::unexpected(configuration_error(
            errc::invalid_argument, "target requires a toolchain digest", selected.name));
    }
    auto id = co_await build::execute(source,
                                      {.command = selected.command,
                                       .cwd = selected.cwd,
                                       .env = selected.environment,
                                       .outputs = selected.outputs,
                                       .inputs = *input_tree,
                                       .toolchain = *selected.toolchain,
                                       .platform = current_platform(),
                                       .dependency_inputs = std::move(dependency_inputs)});
    if (!id) {
        co_return std::unexpected(id.error());
    }
    auto checked = co_await require_relocatable_result(source, *id);
    if (!checked) {
        co_return std::unexpected(checked.error());
    }
    active.erase(selected.name);
    completed.emplace(selected.name, *checked);
    co_return *checked;
}

} // namespace

runtime::task<digest> repository::fetch_source(const source& value) {
    auto existing = co_await repository_detail::build_access::get(*this, value.content);
    if (existing) {
        co_return value.content;
    }
    if (existing.error().code != errc::not_found) {
        co_return std::unexpected(existing.error());
    }
    auto parsed = net_detail::parse_url(value.url, "source URL");
    if (!parsed) {
        co_return std::unexpected(parsed.error());
    }
    if (!parsed->tls) {
        co_return std::unexpected(
            configuration_error(errc::invalid_argument, "source URL must use HTTPS", value.url));
    }
    auto response = co_await net_detail::perform_request(
        {.method = net_detail::http_method::get, .url = *parsed, .target = parsed->path});
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status < 200 || response->status >= 300) {
        co_return std::unexpected(configuration_error(
            errc::io, std::format("source server returned HTTP {}", response->status), value.url));
    }
    auto actual = hash(value.content.alg, response->body);
    if (!actual) {
        co_return std::unexpected(actual.error());
    }
    if (to_string(*actual) != to_string(value.content)) {
        co_return std::unexpected(configuration_error(errc::corrupt,
                                                      "source digest mismatch: expected " +
                                                          to_string(value.content) + ", found " +
                                                          to_string(*actual),
                                                      value.url));
    }
    co_return co_await repository_detail::build_access::put_record(
        *this, std::move(response->body), value.content.alg);
}

runtime::task<lowered_configuration> run_generators(repository& source,
                                                    configuration_document document) {
    auto output = lower(document);
    if (!output) {
        co_return std::unexpected(output.error());
    }
    const auto generators = std::exchange(output->generators, {});
    for (const auto& generator : generators) {
        auto generated = co_await execute_generator(source, generator);
        if (!generated) {
            co_return std::unexpected(generated.error());
        }
        auto records = lowered_configuration{};
        const auto origin = document.origin.parent_path() / generator.output;
        if (generator.format == "cbor") {
            auto decoded = generated_records(generated->second, generated->first.value, origin);
            if (!decoded) {
                co_return std::unexpected(decoded.error());
            }
            records = std::move(*decoded);
        } else {
            const auto text = std::string{reinterpret_cast<const char*>(generated->second.data()),
                                          generated->second.size()};
            auto parsed = parse_configuration(text, origin);
            if (!parsed) {
                co_return std::unexpected(parsed.error());
            }
            auto lowered = lower(*parsed);
            if (!lowered) {
                co_return std::unexpected(lowered.error());
            }
            records = std::move(*lowered);
        }
        auto appended =
            append_generated(*output, std::move(records), generated->first.value, origin);
        if (!appended) {
            co_return std::unexpected(appended.error());
        }
    }
    co_return *output;
}

runtime::task<result_id> execute_target(repository& source, const target& selected) {
    if (!selected.toolchain) {
        co_return std::unexpected(configuration_error(
            errc::invalid_argument, "target requires a toolchain digest", selected.name));
    }
    auto input_tree = selected.inputs;
    if (!input_tree) {
        auto root = co_await source.root_digest();
        if (!root) {
            co_return std::unexpected(root.error());
        }
        input_tree = *root;
    }
    auto id = co_await build::execute(source,
                                      {.command = selected.command,
                                       .cwd = selected.cwd,
                                       .env = selected.environment,
                                       .outputs = selected.outputs,
                                       .inputs = *input_tree,
                                       .toolchain = *selected.toolchain,
                                       .platform = current_platform()});
    if (!id) {
        co_return std::unexpected(id.error());
    }
    co_return co_await require_relocatable_result(source, *id);
}

runtime::task<result_id> build_configuration(repository& source,
                                             const lowered_configuration& configuration,
                                             std::string target_name) {
    const auto selected = std::ranges::find(configuration.targets, target_name, &target::name);
    if (selected == configuration.targets.end()) {
        co_return std::unexpected(configuration_error(
            errc::not_found, "configuration target is not declared", target_name));
    }
    auto completed = std::map<std::string, result_id>{};
    auto active = std::set<std::string>{};
    co_return co_await build_named(source, configuration, *selected, completed, active);
}

} // namespace liquid
