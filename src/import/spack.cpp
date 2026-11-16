// SPDX-License-Identifier: MIT

#include "liquid/import.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>

#include "formats/json.hpp"
#include "liquid/configuration.hpp"
#include "liquid/hash.hpp"
#include "liquid/resolve.hpp"
#include "net/http_client.hpp"

namespace liquid {
namespace {

namespace json = formats::json;

struct imported_source {
    std::string name;
    std::string version;
    std::string url;
    digest content;
};

struct imported_patch {
    std::string url;
    std::optional<digest> content;
    std::optional<std::string> when;
};

struct imported_build {
    std::string system;
    std::vector<std::string> arguments;
    std::optional<digest> toolchain;
};

struct imported_package {
    package_record record;
    std::vector<imported_source> sources;
    std::vector<imported_patch> patches;
    std::optional<imported_build> build;
    spack_package_import report;
};

struct parsed_export {
    std::vector<std::string> roots;
    std::vector<imported_package> packages;
};

struct output_file {
    std::filesystem::path path;
    std::vector<std::byte> contents;
};

auto import_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto object(const json::value& value, std::string_view context)
    -> result<const json::value::object*> {
    const auto* output = std::get_if<json::value::object>(&value.data);
    if (!output) {
        return std::unexpected(
            import_error(errc::invalid_argument, "expected JSON object", std::string{context}));
    }
    return output;
}

auto array(const json::value* value, std::string_view context)
    -> result<const json::value::array*> {
    const auto* output = value ? std::get_if<json::value::array>(&value->data) : nullptr;
    if (!output) {
        return std::unexpected(
            import_error(errc::invalid_argument, "expected JSON array", std::string{context}));
    }
    return output;
}

auto member(const json::value& value, std::string_view name) -> const json::value* {
    const auto* fields = std::get_if<json::value::object>(&value.data);
    if (!fields) {
        return nullptr;
    }
    const auto found =
        std::ranges::find(*fields, name, &std::pair<std::string, json::value>::first);
    return found == fields->end() ? nullptr : &found->second;
}

auto text(const json::value* value, std::string_view context) -> result<std::string> {
    const auto* output = value ? std::get_if<std::string>(&value->data) : nullptr;
    if (!output) {
        return std::unexpected(
            import_error(errc::invalid_argument, "expected JSON string", std::string{context}));
    }
    return *output;
}

auto optional_text(const json::value& value, std::string_view name, std::string_view context)
    -> result<std::optional<std::string>> {
    const auto* selected = member(value, name);
    if (!selected || std::holds_alternative<json::null>(selected->data)) {
        return std::optional<std::string>{};
    }
    auto output = text(selected, context);
    if (!output) {
        return std::unexpected(output.error());
    }
    if (output->empty()) {
        return std::optional<std::string>{};
    }
    return std::optional<std::string>{std::move(*output)};
}

auto boolean(const json::value* value, std::string_view context, bool fallback = false)
    -> result<bool> {
    if (!value) {
        return fallback;
    }
    const auto* output = std::get_if<bool>(&value->data);
    if (!output) {
        return std::unexpected(
            import_error(errc::invalid_argument, "expected JSON boolean", std::string{context}));
    }
    return *output;
}

auto bytes(std::string_view value) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

auto loss_name(spack_loss_kind kind) -> std::string_view {
    switch (kind) {
    case spack_loss_kind::dynamic_value:
        return "dynamic_value";
    case spack_loss_kind::custom_phase:
        return "custom_phase";
    case spack_loss_kind::untranslatable_when:
        return "untranslatable_when";
    case spack_loss_kind::unknown_build_system:
        return "unknown_build_system";
    case spack_loss_kind::unsupported_value:
        return "unsupported_value";
    }
    return "unsupported_value";
}

auto parse_loss_kind(std::string_view name) -> result<spack_loss_kind> {
    if (name == "dynamic_value") {
        return spack_loss_kind::dynamic_value;
    }
    if (name == "custom_phase") {
        return spack_loss_kind::custom_phase;
    }
    if (name == "untranslatable_when") {
        return spack_loss_kind::untranslatable_when;
    }
    if (name == "unknown_build_system") {
        return spack_loss_kind::unknown_build_system;
    }
    if (name == "unsupported_value") {
        return spack_loss_kind::unsupported_value;
    }
    return std::unexpected(
        import_error(errc::invalid_argument, "unknown Spack loss kind", std::string{name}));
}

auto add_loss(imported_package& package,
              spack_loss_kind kind,
              std::string field,
              std::string reason) -> void {
    package.report.losses.push_back({kind, std::move(field), std::move(reason)});
}

auto add_translated(imported_package& package, std::string field) -> void {
    if (std::ranges::find(package.report.translated_fields, field) ==
        package.report.translated_fields.end()) {
        package.report.translated_fields.push_back(std::move(field));
    }
}

auto parse_sha256(std::string_view value, std::string_view context) -> result<digest> {
    auto input = std::string{value};
    if (!input.starts_with("sha256:")) {
        input.insert(0, "sha256:");
    }
    auto output = parse_digest(input);
    if (!output || output->alg != algorithm::sha256) {
        return std::unexpected(
            import_error(errc::invalid_argument, "expected SHA-256 digest", std::string{context}));
    }
    return *output;
}

auto scalar(const json::value& value, std::string_view context) -> result<std::string> {
    if (const auto* item = std::get_if<std::string>(&value.data)) {
        return *item;
    }
    if (const auto* item = std::get_if<bool>(&value.data)) {
        return *item ? "true" : "false";
    }
    if (const auto* item = std::get_if<json::number>(&value.data)) {
        return item->text;
    }
    return std::unexpected(
        import_error(errc::invalid_argument, "expected scalar JSON value", std::string{context}));
}

auto valid_version(std::string_view version) -> bool {
    return !version.empty() && version.contains('.') &&
           std::isdigit(static_cast<unsigned char>(version.front())) != 0 &&
           std::ranges::all_of(version, [](char value) {
               return std::isdigit(static_cast<unsigned char>(value)) != 0 || value == '.' ||
                      value == '-' || value == '+';
           });
}

auto condition(std::string_view package, std::string_view input) -> result<std::string> {
    if (input.empty()) {
        return std::string{};
    }
    auto output = std::string{input};
    if (!output.starts_with(package)) {
        if (output.front() == '@' || output.front() == '+' || output.front() == '~' ||
            output.front() == '%' || output.front() == '^') {
            output.insert(0, package);
        } else {
            output.insert(0, std::string{package} + " ");
        }
    }
    auto parsed = parse_spec(output);
    if (!parsed) {
        return std::unexpected(import_error(
            errc::invalid_argument, "condition is not a liquid spec", std::string{input}));
    }
    return output;
}

auto parse_declared_losses(const json::value& value, imported_package& package) -> result<void> {
    const auto* selected = member(value, "losses");
    if (!selected) {
        return {};
    }
    auto values = array(selected, "package losses");
    if (!values) {
        return std::unexpected(values.error());
    }
    for (const auto& item : **values) {
        auto kind_text = text(member(item, "kind"), "loss kind");
        auto field = text(member(item, "field"), "loss field");
        auto reason = text(member(item, "reason"), "loss reason");
        if (!kind_text || !field || !reason) {
            return std::unexpected(!kind_text ? kind_text.error()
                                   : !field   ? field.error()
                                              : reason.error());
        }
        auto kind = parse_loss_kind(*kind_text);
        if (!kind) {
            return std::unexpected(kind.error());
        }
        add_loss(package, *kind, std::move(*field), std::move(*reason));
    }
    return {};
}

auto parse_versions(const json::value& value, imported_package& package) -> result<void> {
    auto values = array(member(value, "versions"), "package versions");
    if (!values) {
        return std::unexpected(values.error());
    }
    for (const auto& item : **values) {
        auto version = text(member(item, "version"), "package version");
        if (!version) {
            return std::unexpected(version.error());
        }
        if (!valid_version(*version)) {
            add_loss(package,
                     spack_loss_kind::unsupported_value,
                     "versions." + *version,
                     "the configuration language requires a numeric version token");
            continue;
        }
        auto selected = package_version{.version = *version};
        auto url = optional_text(item, "url", "version URL");
        auto sha = optional_text(item, "sha256", "version SHA-256");
        if (!url || !sha) {
            return std::unexpected(!url ? url.error() : sha.error());
        }
        if (*url && *sha) {
            auto content = parse_sha256(**sha, "version " + *version);
            if (!content) {
                return std::unexpected(content.error());
            }
            selected.source = *content;
            if ((*url)->starts_with("https://")) {
                package.sources.push_back(
                    {package.record.name + "-" + *version, *version, **url, *content});
            } else {
                add_loss(package,
                         spack_loss_kind::unsupported_value,
                         "versions." + *version + ".url",
                         "source URL is not HTTPS and cannot be fetched by liquid");
            }
        } else if (*url || *sha) {
            add_loss(package,
                     spack_loss_kind::unsupported_value,
                     "versions." + *version + ".source",
                     "a source requires both URL and SHA-256");
        }
        package.record.versions.push_back(std::move(selected));
    }
    if (package.record.versions.empty()) {
        add_loss(package,
                 spack_loss_kind::unsupported_value,
                 "versions",
                 "package has no numeric version representable by package/1");
        return {};
    }
    add_translated(package, "versions");
    if (!package.sources.empty()) {
        add_translated(package, "sources");
    }
    return {};
}

auto parse_variants(const json::value& value, imported_package& package) -> result<void> {
    const auto* selected = member(value, "variants");
    if (!selected) {
        return {};
    }
    auto values = array(selected, "package variants");
    if (!values) {
        return std::unexpected(values.error());
    }
    auto configurations = std::size_t{1};
    for (const auto& item : **values) {
        auto name = text(member(item, "name"), "variant name");
        auto choices = array(member(item, "values"), "variant values");
        const auto* default_value = member(item, "default");
        auto when = optional_text(item, "when", "variant condition");
        if (!name || !choices || !default_value || !when) {
            return std::unexpected(!name      ? name.error()
                                   : !choices ? choices.error()
                                   : !when    ? when.error()
                                              : import_error(errc::invalid_argument,
                                                          "variant has no default",
                                                          package.record.name));
        }
        auto definition = variant_definition{.name = std::move(*name)};
        for (const auto& choice : **choices) {
            auto value_text = scalar(choice, "variant value");
            if (!value_text) {
                add_loss(package,
                         spack_loss_kind::unsupported_value,
                         "variants." + definition.name,
                         "non-scalar variant value");
                definition.values.clear();
                break;
            }
            definition.values.push_back(std::move(*value_text));
        }
        auto default_text = scalar(*default_value, "variant default");
        if (!default_text) {
            add_loss(package,
                     spack_loss_kind::unsupported_value,
                     "variants." + definition.name,
                     "non-scalar variant default");
            continue;
        }
        definition.default_value = std::move(*default_text);
        if (std::ranges::find(definition.values, definition.default_value) ==
            definition.values.end()) {
            add_loss(package,
                     spack_loss_kind::unsupported_value,
                     "variants." + definition.name + ".default",
                     "default is not one scalar member of the variant values");
            continue;
        }
        if (definition.values.size() > 1 && configurations > 4096 / definition.values.size()) {
            add_loss(package,
                     spack_loss_kind::unsupported_value,
                     "variants." + definition.name + ".values",
                     "non-default values omitted because the package variant space exceeds 4096");
            definition.values = {definition.default_value};
        }
        configurations *= definition.values.size();
        if (*when) {
            auto translated = condition(package.record.name, **when);
            if (!translated) {
                add_loss(package,
                         spack_loss_kind::untranslatable_when,
                         "variants." + definition.name + ".when",
                         **when);
                continue;
            }
            definition.when = std::move(*translated);
        }
        if (!definition.values.empty()) {
            package.record.variants.push_back(std::move(definition));
        }
    }
    if (!package.record.variants.empty()) {
        add_translated(package, "variants");
    }
    return {};
}

auto dependency_type_from(const json::value& item, imported_package& package)
    -> result<dependency_type> {
    const auto* selected = member(item, "types");
    if (!selected) {
        return dependency_type::link;
    }
    auto values = array(selected, "dependency types");
    if (!values) {
        return std::unexpected(values.error());
    }
    auto names = std::vector<std::string>{};
    for (const auto& value : **values) {
        auto name = text(&value, "dependency type");
        if (!name) {
            return std::unexpected(name.error());
        }
        names.push_back(std::move(*name));
    }
    if (names.size() > 1) {
        add_loss(package,
                 spack_loss_kind::unsupported_value,
                 "dependencies.types",
                 "package/1 records one dependency type; link, run, then build precedence applied");
    }
    if (std::ranges::find(names, "link") != names.end()) {
        return dependency_type::link;
    }
    if (std::ranges::find(names, "run") != names.end()) {
        return dependency_type::run;
    }
    return dependency_type::build;
}

auto parse_dependencies(const json::value& value, imported_package& package) -> result<void> {
    const auto* selected = member(value, "dependencies");
    if (!selected) {
        return {};
    }
    auto values = array(selected, "package dependencies");
    if (!values) {
        return std::unexpected(values.error());
    }
    for (const auto& item : **values) {
        auto name = text(member(item, "name"), "dependency name");
        auto constraint = optional_text(item, "constraint", "dependency constraint");
        auto when = optional_text(item, "when", "dependency condition");
        auto virtual_dependency = boolean(member(item, "virtual"), "virtual dependency");
        auto type = dependency_type_from(item, package);
        if (!name || !constraint || !when || !virtual_dependency || !type) {
            return std::unexpected(!name                 ? name.error()
                                   : !constraint         ? constraint.error()
                                   : !when               ? when.error()
                                   : !virtual_dependency ? virtual_dependency.error()
                                                         : type.error());
        }
        auto definition = dependency_definition{
            .name = std::move(*name), .virtual_dependency = *virtual_dependency, .type = *type};
        if (*constraint && !(*constraint)->empty()) {
            definition.version =
                (*constraint)->starts_with('@') ? (*constraint)->substr(1) : **constraint;
        }
        if (*when) {
            auto translated = condition(package.record.name, **when);
            if (!translated) {
                add_loss(package,
                         spack_loss_kind::untranslatable_when,
                         "dependencies." + definition.name + ".when",
                         **when);
                continue;
            }
            definition.when = std::move(*translated);
        }
        package.record.dependencies.push_back(std::move(definition));
    }
    if (!package.record.dependencies.empty()) {
        add_translated(package, "dependencies");
    }
    return {};
}

auto parse_provides(const json::value& value, imported_package& package) -> result<void> {
    const auto* selected = member(value, "provides");
    if (!selected) {
        return {};
    }
    auto values = array(selected, "package provides");
    if (!values) {
        return std::unexpected(values.error());
    }
    for (const auto& item : **values) {
        auto name = text(member(item, "name"), "provided virtual name");
        auto version = optional_text(item, "version", "provided virtual version");
        if (!name || !version) {
            return std::unexpected(!name ? name.error() : version.error());
        }
        package.record.provides.push_back({std::move(*name), std::move(*version)});
    }
    if (!package.record.provides.empty()) {
        add_translated(package, "provides");
    }
    return {};
}

auto parse_conflicts(const json::value& value, imported_package& package) -> result<void> {
    const auto* selected = member(value, "conflicts");
    if (!selected) {
        return {};
    }
    auto values = array(selected, "package conflicts");
    if (!values) {
        return std::unexpected(values.error());
    }
    for (const auto& item : **values) {
        auto raw = text(member(item, "constraint"), "conflict constraint");
        auto when = optional_text(item, "when", "conflict condition");
        auto message = optional_text(item, "message", "conflict message");
        if (!raw || !when || !message) {
            return std::unexpected(!raw ? raw.error() : !when ? when.error() : message.error());
        }
        auto translated = condition(package.record.name, *raw);
        if (!translated) {
            add_loss(package, spack_loss_kind::untranslatable_when, "conflicts.constraint", *raw);
            continue;
        }
        auto parsed = parse_spec(*translated);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        auto definition =
            conflict_definition{.constraint = std::move(*parsed), .message = message->value_or("")};
        if (*when) {
            auto translated_when = condition(package.record.name, **when);
            if (!translated_when) {
                add_loss(package, spack_loss_kind::untranslatable_when, "conflicts.when", **when);
                continue;
            }
            definition.when = std::move(*translated_when);
        }
        package.record.conflicts.push_back(std::move(definition));
    }
    if (!package.record.conflicts.empty()) {
        add_translated(package, "conflicts");
    }
    return {};
}

auto parse_patches(const json::value& value, imported_package& package) -> result<void> {
    const auto* selected = member(value, "patches");
    if (!selected) {
        return {};
    }
    auto values = array(selected, "package patches");
    if (!values) {
        return std::unexpected(values.error());
    }
    for (const auto& item : **values) {
        auto url = optional_text(item, "url", "patch URL");
        auto sha = optional_text(item, "sha256", "patch SHA-256");
        auto when = optional_text(item, "when", "patch condition");
        if (!url || !sha || !when) {
            return std::unexpected(!url ? url.error() : !sha ? sha.error() : when.error());
        }
        auto patch = imported_patch{.url = url->value_or("")};
        if (*sha) {
            auto content = parse_sha256(**sha, "patch digest");
            if (!content) {
                return std::unexpected(content.error());
            }
            patch.content = *content;
        }
        if (*when) {
            auto translated = condition(package.record.name, **when);
            if (!translated) {
                add_loss(package, spack_loss_kind::untranslatable_when, "patches.when", **when);
                continue;
            }
            patch.when = std::move(*translated);
        }
        if (!patch.content && patch.url.empty()) {
            add_loss(package,
                     spack_loss_kind::unsupported_value,
                     "patches",
                     "patch has neither a digest nor a fetchable URL");
            continue;
        }
        package.patches.push_back(std::move(patch));
    }
    return {};
}

auto parse_build(const json::value& value, imported_package& package) -> result<void> {
    const auto* selected = member(value, "build");
    if (!selected || std::holds_alternative<json::null>(selected->data)) {
        return {};
    }
    auto system = text(member(*selected, "system"), "build system");
    if (!system) {
        return std::unexpected(system.error());
    }
    auto lowered = *system;
    std::ranges::transform(lowered, lowered.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    if (!lowered.contains("cmake") && !lowered.contains("autotools") &&
        !lowered.contains("makefile")) {
        add_loss(package, spack_loss_kind::unknown_build_system, "build.system", *system);
        return {};
    }
    auto build = imported_build{.system = std::move(lowered)};
    if (const auto* arguments = member(*selected, "arguments")) {
        auto values = array(arguments, "build arguments");
        if (!values) {
            return std::unexpected(values.error());
        }
        for (const auto& argument : **values) {
            auto argument_text = text(&argument, "build argument");
            if (!argument_text) {
                add_loss(package,
                         spack_loss_kind::dynamic_value,
                         "build.arguments",
                         "argument is computed by Python at build time");
                continue;
            }
            build.arguments.push_back(std::move(*argument_text));
        }
    }
    package.build = std::move(build);
    add_translated(package, "build");
    return {};
}

auto parse_package(const json::value& value) -> result<imported_package> {
    auto name = text(member(value, "name"), "package name");
    if (!name || name->empty() || name->contains('/') || name->contains('\\')) {
        return std::unexpected(!name ? name.error()
                                     : import_error(errc::invalid_argument,
                                                    "invalid Spack package name",
                                                    name.value_or("")));
    }
    auto output = imported_package{.record = package_record{.name = *name},
                                   .report = spack_package_import{.name = *name}};
    auto declared_losses = parse_declared_losses(value, output);
    auto versions = parse_versions(value, output);
    auto variants = parse_variants(value, output);
    auto dependencies = parse_dependencies(value, output);
    auto provides = parse_provides(value, output);
    auto conflicts = parse_conflicts(value, output);
    auto patches = parse_patches(value, output);
    auto build = parse_build(value, output);
    if (!declared_losses || !versions || !variants || !dependencies || !provides || !conflicts ||
        !patches || !build) {
        if (!declared_losses) {
            return std::unexpected(declared_losses.error());
        }
        if (!versions) {
            return std::unexpected(versions.error());
        }
        if (!variants) {
            return std::unexpected(variants.error());
        }
        if (!dependencies) {
            return std::unexpected(dependencies.error());
        }
        if (!provides) {
            return std::unexpected(provides.error());
        }
        if (!conflicts) {
            return std::unexpected(conflicts.error());
        }
        if (!patches) {
            return std::unexpected(patches.error());
        }
        return std::unexpected(build.error());
    }
    std::ranges::sort(output.report.translated_fields);
    return output;
}

auto parse_export(std::string_view document) -> result<parsed_export> {
    auto decoded = json::parse(document);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    auto fields = object(*decoded, "Spack export");
    if (!fields) {
        return std::unexpected(fields.error());
    }
    auto schema = text(member(*decoded, "schema"), "Spack export schema");
    if (!schema || *schema != "spack-export/1") {
        return std::unexpected(
            !schema
                ? schema.error()
                : import_error(errc::invalid_argument, "unsupported Spack export schema", *schema));
    }
    auto output = parsed_export{};
    if (const auto* roots = member(*decoded, "roots")) {
        auto values = array(roots, "Spack export roots");
        if (!values) {
            return std::unexpected(values.error());
        }
        for (const auto& root : **values) {
            auto root_text = text(&root, "Spack export root");
            if (!root_text) {
                return std::unexpected(root_text.error());
            }
            output.roots.push_back(std::move(*root_text));
        }
    }
    auto packages = array(member(*decoded, "packages"), "Spack export packages");
    if (!packages) {
        return std::unexpected(packages.error());
    }
    for (const auto& package_value : **packages) {
        auto package = parse_package(package_value);
        if (!package) {
            return std::unexpected(package.error());
        }
        output.packages.push_back(std::move(*package));
    }
    std::ranges::sort(
        output.packages, {}, [](const imported_package& value) { return value.record.name; });
    return output;
}

auto escape(std::string_view value) -> std::string {
    auto output = std::string{};
    for (const auto character : value) {
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
    return output;
}

auto config_quote(std::string_view value) -> std::string {
    return "\"" + escape(value) + "\"";
}

auto value_token(std::string_view value) -> std::string {
    if (value == "true" || value == "false") {
        return std::string{value};
    }
    return config_quote(value);
}

auto shell_quote(std::string_view value) -> std::string {
    auto output = std::string{"'"};
    for (const auto character : value) {
        if (character == '\'') {
            output += "'\\''";
        } else {
            output.push_back(character);
        }
    }
    return output + "'";
}

auto dependency_document(const dependency_definition& dependency) -> std::string {
    auto output = std::string{"depends_on "} + config_quote(dependency.name) + " {";
    output += " type: ";
    output += dependency.type == dependency_type::build ? "build"
              : dependency.type == dependency_type::run ? "run"
                                                        : "link";
    if (dependency.virtual_dependency) {
        output += " virtual: true";
    }
    if (dependency.version) {
        output += " version: " + config_quote(*dependency.version);
    }
    return output + " }";
}

auto write_package_sources(std::ostringstream& output, const imported_package& package) -> void {
    for (const auto& source : package.sources) {
        output << "source " << config_quote(source.name) << " {\n"
               << "  url: " << config_quote(source.url) << "\n"
               << "  digest: " << to_string(source.content) << "\n"
               << "  kind: archive\n"
               << "}\n\n";
    }
}

auto write_package_variants(std::ostringstream& output, const imported_package& package) -> void {
    for (const auto& variant : package.record.variants) {
        const auto write_variant = [&] {
            output << "variant " << config_quote(variant.name) << " { values: [";
            for (std::size_t index = 0; index < variant.values.size(); ++index) {
                if (index != 0) {
                    output << ", ";
                }
                output << value_token(variant.values[index]);
            }
            output << "] default: " << value_token(variant.default_value) << " }\n";
        };
        if (variant.when) {
            output << "when " << config_quote(*variant.when) << " {\n  ";
            write_variant();
            output << "}\n";
        } else {
            write_variant();
        }
    }
}

auto write_package_dependencies(std::ostringstream& output, const imported_package& package)
    -> void {
    for (const auto& dependency : package.record.dependencies) {
        if (dependency.when) {
            output << "when " << config_quote(*dependency.when) << " {\n  "
                   << dependency_document(dependency) << "\n}\n";
        } else {
            output << dependency_document(dependency) << "\n";
        }
    }
}

auto write_package_constraints(std::ostringstream& output, const imported_package& package)
    -> void {
    for (const auto& provided : package.record.provides) {
        output << "provides " << config_quote(provided.name) << " {";
        if (provided.version) {
            output << " version: " << config_quote(*provided.version);
        }
        output << " }\n";
    }
    for (const auto& conflict : package.record.conflicts) {
        const auto write_conflict = [&] {
            output << "conflicts { constraint: spec "
                   << config_quote(to_string(conflict.constraint));
            if (!conflict.message.empty()) {
                output << " message: " << config_quote(conflict.message);
            }
            output << " }\n";
        };
        if (conflict.when) {
            output << "when " << config_quote(*conflict.when) << " {\n  ";
            write_conflict();
            output << "}\n";
        } else {
            write_conflict();
        }
    }
    for (const auto& patch : package.record.patches) {
        if (patch.when) {
            output << "when " << config_quote(*patch.when)
                   << " {\n  patch { digest: " << to_string(patch.value) << " }\n}\n";
        } else {
            output << "patch { digest: " << to_string(patch.value) << " }\n";
        }
    }
}

auto build_command(const imported_package& package) -> std::string {
    const auto& source = package.sources.back();
    auto command =
        std::string{"set -eu; rm -rf source build out/prefix; mkdir -p source build out/prefix; "
                    "tar -xf deps/__liquid_sources/"};
    auto source_path = to_string(source.content);
    std::ranges::replace(source_path, ':', '/');
    command += source_path + " -C source --strip-components=1; ";
    if (package.build->system.contains("cmake")) {
        command += "cmake -S source -B build -DCMAKE_INSTALL_PREFIX=/";
        for (const auto& argument : package.build->arguments) {
            command += " " + shell_quote(argument);
        }
        return command + "; cmake --build build; DESTDIR=$PWD/out/prefix cmake --install build";
    }
    if (package.build->system.contains("autotools")) {
        command += "cd source; ./configure --prefix=/";
        for (const auto& argument : package.build->arguments) {
            command += " " + shell_quote(argument);
        }
        return command + "; make; make DESTDIR=$OLDPWD/out/prefix install";
    }
    command += "cd source; make";
    for (const auto& argument : package.build->arguments) {
        command += " " + shell_quote(argument);
    }
    return command + "; make DESTDIR=$OLDPWD/out/prefix PREFIX=/ install";
}

auto write_package_build(std::ostringstream& output, const imported_package& package) -> void {
    if (!package.build || package.sources.empty()) {
        return;
    }
    const auto& source = package.sources.back();
    output << "\nbuild " << config_quote(package.record.name) << " {\n"
           << "  command: [\"sh\", \"-c\", " << config_quote(build_command(package)) << "]\n"
           << "  sources: [" << config_quote(source.name) << "]\n"
           << "  outputs: [path \"out/prefix\"]\n"
           << "  install { layout: prefix }\n";
    if (package.build->toolchain) {
        output << "  toolchain: " << to_string(*package.build->toolchain) << "\n";
    }
    output << "}\n";
}

auto package_document(const imported_package& package) -> result<std::string> {
    auto output = std::ostringstream{};
    write_package_sources(output, package);
    output << "package " << config_quote(package.record.name) << " {\n  versions: [";
    for (std::size_t index = 0; index < package.record.versions.size(); ++index) {
        if (index != 0) {
            output << ", ";
        }
        output << package.record.versions[index].version;
    }
    output << "]\n";
    write_package_variants(output, package);
    write_package_dependencies(output, package);
    write_package_constraints(output, package);
    output << "}\n";
    write_package_build(output, package);
    auto parsed = parse_configuration(output.str(), "packages/" + package.record.name + ".liquid");
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return format_configuration(*parsed);
}

auto report_value(const spack_import_report& report) -> json::value {
    auto roots = json::value::array{};
    for (const auto& root : report.roots) {
        roots.emplace_back(root);
    }
    auto packages = json::value::array{};
    for (const auto& package : report.package_reports) {
        auto translated = json::value::array{};
        for (const auto& field : package.translated_fields) {
            translated.emplace_back(field);
        }
        auto losses = json::value::array{};
        for (const auto& loss : package.losses) {
            losses.emplace_back(json::value::object{{"kind", std::string{loss_name(loss.kind)}},
                                                    {"field", loss.field},
                                                    {"reason", loss.reason}});
        }
        packages.emplace_back(json::value::object{{"name", package.name},
                                                  {"translated", std::move(translated)},
                                                  {"losses", std::move(losses)}});
    }
    return json::value::object{
        {"v", json::number{std::to_string(report.v)}},
        {"kind", std::string{"import-report"}},
        {"source", std::string{"spack"}},
        {"packages", json::number{std::to_string(report.packages)}},
        {"translated_fields", json::number{std::to_string(report.translated_fields)}},
        {"losses", json::number{std::to_string(report.losses)}},
        {"pinned_patches", json::number{std::to_string(report.pinned_patches)}},
        {"roots", std::move(roots)},
        {"package_reports", std::move(packages)}};
}

auto unsigned_number(const json::value* value, std::string_view context) -> result<std::uint64_t> {
    const auto* number = value ? std::get_if<json::number>(&value->data) : nullptr;
    if (!number) {
        return std::unexpected(
            import_error(errc::invalid_argument, "expected JSON number", std::string{context}));
    }
    auto output = std::uint64_t{};
    const auto [end, code] =
        std::from_chars(number->text.data(), number->text.data() + number->text.size(), output);
    if (code != std::errc{} || end != number->text.data() + number->text.size()) {
        return std::unexpected(
            import_error(errc::invalid_argument, "expected unsigned number", std::string{context}));
    }
    return output;
}

auto fetch_patch(std::string url) -> runtime::task<std::vector<std::byte>> {
    auto parsed = net_detail::parse_url(url, "Spack patch URL");
    if (!parsed) {
        co_return std::unexpected(parsed.error());
    }
    if (!parsed->tls) {
        co_return std::unexpected(
            import_error(errc::invalid_argument, "patch URL must use HTTPS", url));
    }
    auto response = co_await net_detail::perform_request(
        {.method = net_detail::http_method::get, .url = *parsed, .target = parsed->path});
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status < 200 || response->status >= 300) {
        co_return std::unexpected(import_error(
            errc::io, std::format("patch server returned HTTP {}", response->status), url));
    }
    co_return std::move(response->body);
}

auto read_file(std::filesystem::path path) -> result<std::string> {
    auto input = std::ifstream{path, std::ios::binary};
    if (!input) {
        return std::unexpected(
            import_error(errc::io, "cannot read Spack export", path.generic_string()));
    }
    return std::string{std::istreambuf_iterator<char>{input}, {}};
}

auto write_files(std::filesystem::path root, std::vector<output_file> files) -> result<void> {
    auto code = std::error_code{};
    std::filesystem::create_directories(root / "packages", code);
    if (code) {
        return std::unexpected(import_error(errc::io, code.message(), root.generic_string()));
    }
    for (const auto& file : files) {
        const auto path = root / file.path;
        std::filesystem::create_directories(path.parent_path(), code);
        if (code) {
            return std::unexpected(import_error(errc::io, code.message(), path.generic_string()));
        }
        auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
        if (!output) {
            return std::unexpected(
                import_error(errc::io, "cannot write imported package", path.generic_string()));
        }
        output.write(reinterpret_cast<const char*>(file.contents.data()),
                     static_cast<std::streamsize>(file.contents.size()));
        if (!output) {
            return std::unexpected(
                import_error(errc::io, "cannot write imported package", path.generic_string()));
        }
    }
    return {};
}

} // namespace

result<std::string> spack_import_report_json(const spack_import_report& report) {
    return json::write(report_value(report));
}

std::string spack_import_report_text(const spack_import_report& report) {
    auto output = std::ostringstream{};
    output << "Spack import: packages=" << report.packages
           << " translated_fields=" << report.translated_fields << " losses=" << report.losses
           << " pinned_patches=" << report.pinned_patches << '\n';
    for (const auto& package : report.package_reports) {
        if (package.losses.empty()) {
            continue;
        }
        output << package.name << ":\n";
        for (const auto& loss : package.losses) {
            output << "  " << loss_name(loss.kind) << " " << loss.field << ": " << loss.reason
                   << '\n';
        }
    }
    return output.str();
}

result<spack_import_report> decode_spack_import_report(std::string_view document) {
    auto decoded = json::parse(document);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    auto kind = text(member(*decoded, "kind"), "report kind");
    auto source = text(member(*decoded, "source"), "report source");
    auto version = unsigned_number(member(*decoded, "v"), "report version");
    auto packages = unsigned_number(member(*decoded, "packages"), "report packages");
    auto translated =
        unsigned_number(member(*decoded, "translated_fields"), "report translated fields");
    auto losses = unsigned_number(member(*decoded, "losses"), "report losses");
    auto pinned = unsigned_number(member(*decoded, "pinned_patches"), "report pinned patches");
    if (!kind || !source || !version || !packages || !translated || !losses || !pinned ||
        *kind != "import-report" || *source != "spack" || *version != 1) {
        return std::unexpected(import_error(
            errc::invalid_argument, "unsupported Spack import report", "import-report/1"));
    }
    auto output = spack_import_report{.packages = *packages,
                                      .translated_fields = *translated,
                                      .losses = *losses,
                                      .pinned_patches = *pinned};
    auto roots = array(member(*decoded, "roots"), "report roots");
    auto package_reports = array(member(*decoded, "package_reports"), "report packages");
    if (!roots || !package_reports) {
        return std::unexpected(!roots ? roots.error() : package_reports.error());
    }
    for (const auto& root : **roots) {
        auto root_text = text(&root, "report root");
        if (!root_text) {
            return std::unexpected(root_text.error());
        }
        output.roots.push_back(std::move(*root_text));
    }
    for (const auto& package_value : **package_reports) {
        auto name = text(member(package_value, "name"), "report package name");
        auto translated_values = array(member(package_value, "translated"), "translated fields");
        auto loss_values = array(member(package_value, "losses"), "package losses");
        if (!name || !translated_values || !loss_values) {
            return std::unexpected(!name                ? name.error()
                                   : !translated_values ? translated_values.error()
                                                        : loss_values.error());
        }
        auto package = spack_package_import{.name = std::move(*name)};
        for (const auto& field_value : **translated_values) {
            auto field = text(&field_value, "translated field");
            if (!field) {
                return std::unexpected(field.error());
            }
            package.translated_fields.push_back(std::move(*field));
        }
        for (const auto& loss_value : **loss_values) {
            auto kind_text = text(member(loss_value, "kind"), "loss kind");
            auto field = text(member(loss_value, "field"), "loss field");
            auto reason = text(member(loss_value, "reason"), "loss reason");
            if (!kind_text || !field || !reason) {
                return std::unexpected(!kind_text ? kind_text.error()
                                       : !field   ? field.error()
                                                  : reason.error());
            }
            auto loss_kind_value = parse_loss_kind(*kind_text);
            if (!loss_kind_value) {
                return std::unexpected(loss_kind_value.error());
            }
            package.losses.push_back({*loss_kind_value, std::move(*field), std::move(*reason)});
        }
        output.package_reports.push_back(std::move(package));
    }
    return output;
}

runtime::task<spack_import_report> import_spack(std::filesystem::path export_document,
                                                std::filesystem::path packages_repository,
                                                spack_import_options options) {
    auto loaded =
        co_await runtime::offload([path = std::move(export_document)] { return read_file(path); });
    if (!loaded) {
        co_return std::unexpected(loaded.error());
    }
    if (!*loaded) {
        co_return std::unexpected(loaded->error());
    }
    auto parsed = parse_export(**loaded);
    if (!parsed) {
        co_return std::unexpected(parsed.error());
    }
    const auto imported_count = std::ranges::count_if(
        parsed->packages, [](const auto& package) { return !package.record.versions.empty(); });
    auto report = spack_import_report{.packages = static_cast<std::uint64_t>(imported_count),
                                      .roots = parsed->roots};
    for (auto& package : parsed->packages) {
        if (package.build) {
            package.build->toolchain = options.toolchain;
        }
        for (auto& patch : package.patches) {
            if (!patch.content) {
                auto contents = options.patch_contents.find(patch.url);
                auto fetched = std::vector<std::byte>{};
                if (contents != options.patch_contents.end()) {
                    fetched = contents->second;
                } else {
                    auto response = co_await fetch_patch(patch.url);
                    if (!response) {
                        co_return std::unexpected(response.error());
                    }
                    fetched = std::move(*response);
                }
                auto content = hash(algorithm::sha256, fetched);
                if (!content) {
                    co_return std::unexpected(content.error());
                }
                patch.content = *content;
                ++report.pinned_patches;
            }
            package.record.patches.push_back({*patch.content, patch.when});
        }
        if (!package.record.patches.empty()) {
            add_translated(package, "patches");
        }
        std::ranges::sort(package.report.translated_fields);
        report.translated_fields += package.report.translated_fields.size();
        report.losses += package.report.losses.size();
        report.package_reports.push_back(package.report);
    }
    auto files = std::vector<output_file>{};
    for (const auto& package : parsed->packages) {
        if (package.record.versions.empty()) {
            continue;
        }
        auto document = package_document(package);
        auto encoded = encode_package(package.record);
        if (!document || !encoded) {
            co_return std::unexpected(!document ? document.error() : encoded.error());
        }
        auto shard = std::string{package.record.name.front()};
        files.push_back(
            {std::filesystem::path{"packages"} / shard / (package.record.name + ".liquid"),
             bytes(*document)});
        files.push_back(
            {std::filesystem::path{"packages"} / shard / (package.record.name + ".cbor"),
             std::move(*encoded)});
    }
    auto report_json = spack_import_report_json(report);
    if (!report_json) {
        co_return std::unexpected(report_json.error());
    }
    files.push_back({"import-report.json", bytes(*report_json)});
    auto written = co_await runtime::offload(
        [root = std::move(packages_repository), files = std::move(files)]() mutable {
            return write_files(std::move(root), std::move(files));
        });
    if (!written) {
        co_return std::unexpected(written.error());
    }
    if (!*written) {
        co_return std::unexpected(written->error());
    }
    co_return report;
}

} // namespace liquid
