#include "liquid/configuration.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

namespace liquid {
namespace {

auto kind_name(configuration_value_kind kind) -> std::string_view {
    switch (kind) {
    case configuration_value_kind::string:
        return "string";
    case configuration_value_kind::integer:
        return "integer";
    case configuration_value_kind::boolean:
        return "boolean";
    case configuration_value_kind::version:
        return "version";
    case configuration_value_kind::spec:
        return "spec";
    case configuration_value_kind::digest:
        return "digest";
    case configuration_value_kind::path:
        return "path";
    case configuration_value_kind::identifier:
        return "identifier";
    case configuration_value_kind::list:
        return "list";
    case configuration_value_kind::map:
        return "map";
    }
    return "value";
}

auto found(const configuration_value& value) -> std::string {
    switch (value.kind) {
    case configuration_value_kind::string:
        return "string '" + value.text + "'";
    case configuration_value_kind::integer:
        return std::format("integer {}", value.integer);
    case configuration_value_kind::boolean:
        return value.boolean ? "boolean true" : "boolean false";
    case configuration_value_kind::version:
    case configuration_value_kind::spec:
    case configuration_value_kind::digest:
    case configuration_value_kind::path:
    case configuration_value_kind::identifier:
        return std::string{kind_name(value.kind)} + " '" + value.text + "'";
    case configuration_value_kind::list:
        return "list";
    case configuration_value_kind::map:
        return "map";
    }
    return "value";
}

auto distance(std::string_view left, std::string_view right) -> std::size_t {
    auto prior = std::vector<std::size_t>(right.size() + 1);
    auto current = prior;
    for (std::size_t column = 0; column <= right.size(); ++column) {
        prior[column] = column;
    }
    for (std::size_t row = 1; row <= left.size(); ++row) {
        current[0] = row;
        for (std::size_t column = 1; column <= right.size(); ++column) {
            current[column] =
                std::min({current[column - 1] + 1,
                          prior[column] + 1,
                          prior[column - 1] + (left[row - 1] == right[column - 1] ? 0 : 1)});
        }
        std::swap(prior, current);
    }
    return prior.back();
}

auto nearest(std::string_view name, std::span<const std::string_view> candidates)
    -> std::string_view {
    auto output = candidates.front();
    auto best = std::numeric_limits<std::size_t>::max();
    for (const auto candidate : candidates) {
        const auto score = distance(name, candidate);
        if (score < best) {
            best = score;
            output = candidate;
        }
    }
    return output;
}

auto field(const configuration_record& record, std::string_view name)
    -> const configuration_field* {
    const auto found_field = std::ranges::find(record.fields, name, &configuration_field::name);
    return found_field == record.fields.end() ? nullptr : &*found_field;
}

auto diagnostic(configuration_location location,
                std::string expected,
                std::string found_value,
                std::string hint) -> configuration_diagnostic {
    return {std::move(location), std::move(expected), std::move(found_value), std::move(hint)};
}

auto type_hint(configuration_value_kind expected) -> std::string {
    switch (expected) {
    case configuration_value_kind::version:
        return "versions are unquoted: 1.14.3";
    case configuration_value_kind::digest:
        return "digests include the algorithm: blake3:<64 hexadecimal digits>";
    case configuration_value_kind::path:
        return "paths use the path literal: path \"relative/path\"";
    case configuration_value_kind::spec:
        return "specs use the spec literal: spec \"hdf5 @1.14 +mpi\"";
    case configuration_value_kind::boolean:
        return "booleans are unquoted: true or false";
    case configuration_value_kind::integer:
        return "integers are unquoted: 3";
    case configuration_value_kind::string:
        return "strings are quoted: \"value\"";
    case configuration_value_kind::list:
        return "lists use brackets: [\"one\", \"two\"]";
    case configuration_value_kind::map:
        return "maps use braces: { key: \"value\" }";
    case configuration_value_kind::identifier:
        return "enum values are unquoted";
    }
    return {};
}

auto require_type(const configuration_field* selected,
                  configuration_value_kind expected,
                  std::vector<configuration_diagnostic>& output) -> bool {
    if (!selected) {
        return false;
    }
    if (selected->value.kind == expected) {
        return true;
    }
    output.push_back(diagnostic(selected->value.location,
                                std::string{kind_name(expected)},
                                found(selected->value),
                                type_hint(expected)));
    return false;
}

auto require_one_of(const configuration_field* selected,
                    std::span<const configuration_value_kind> expected,
                    std::string type,
                    std::string hint,
                    std::vector<configuration_diagnostic>& output) -> bool {
    if (!selected) {
        return false;
    }
    if (std::ranges::find(expected, selected->value.kind) != expected.end()) {
        return true;
    }
    output.push_back(diagnostic(
        selected->value.location, std::move(type), found(selected->value), std::move(hint)));
    return false;
}

auto require_field(const configuration_record& record,
                   std::string_view name,
                   std::string expected,
                   std::string hint,
                   std::vector<configuration_diagnostic>& output) -> const configuration_field* {
    const auto* selected = field(record, name);
    if (!selected) {
        output.push_back(diagnostic(record.location,
                                    std::move(expected),
                                    "missing field '" + std::string{name} + "'",
                                    std::move(hint)));
    }
    return selected;
}

auto check_fields(const configuration_record& record,
                  std::span<const std::string_view> valid,
                  std::vector<configuration_diagnostic>& output) -> void {
    auto seen = std::set<std::string>{};
    for (const auto& item : record.fields) {
        if (!seen.insert(item.name).second) {
            output.push_back(diagnostic(item.location,
                                        "unique field",
                                        "duplicate '" + item.name + "'",
                                        "remove the duplicate field"));
            continue;
        }
        if (std::ranges::find(valid, item.name) == valid.end()) {
            const auto suggestion = valid.empty() ? std::string_view{} : nearest(item.name, valid);
            output.push_back(diagnostic(item.location,
                                        "known " + record.kind + " field",
                                        "'" + item.name + "'",
                                        suggestion.empty()
                                            ? "this record does not accept fields"
                                            : "did you mean '" + std::string{suggestion} + "'?"));
        }
    }
}

auto check_string_list(const configuration_field* selected,
                       std::string_view element_type,
                       std::vector<configuration_diagnostic>& output,
                       bool paths = false) -> void {
    if (!require_type(selected, configuration_value_kind::list, output)) {
        return;
    }
    for (const auto& item : selected->value.list) {
        const auto valid = paths ? item.kind == configuration_value_kind::path ||
                                       item.kind == configuration_value_kind::string
                                 : item.kind == configuration_value_kind::string;
        if (!valid) {
            output.push_back(diagnostic(item.location,
                                        std::string{element_type},
                                        found(item),
                                        paths ? type_hint(configuration_value_kind::path)
                                              : type_hint(configuration_value_kind::string)));
        }
    }
}

auto intent_name_is_valid(std::string_view value) -> bool {
    if (value.empty() || (value.front() != '_' && (value.front() < 'a' || value.front() > 'z'))) {
        return false;
    }
    return std::ranges::all_of(value.substr(1), [](const char item) {
        return (item >= 'a' && item <= 'z') || (item >= '0' && item <= '9') || item == '_' ||
               item == '-';
    });
}

auto canonical_absolute_path(std::string_view value) -> bool {
    const auto path = std::filesystem::path{value};
    if (value.empty() || value.front() != '/' || value == "/" ||
        value.find("//") != std::string::npos || path.lexically_normal() != path) {
        return false;
    }
    return std::ranges::all_of(value, [](const char item) {
        return std::isalnum(static_cast<unsigned char>(item)) != 0 || item == '/' || item == '.' ||
               item == '_' || item == '+' || item == '-';
    });
}

auto check_intent_name(const configuration_field* selected,
                       std::string_view field_name,
                       std::vector<configuration_diagnostic>& output) -> void {
    if (!require_type(selected, configuration_value_kind::identifier, output)) {
        return;
    }
    if (!intent_name_is_valid(selected->value.text)) {
        output.push_back(diagnostic(selected->value.location,
                                    "name matching [a-z_][a-z0-9_-]*",
                                    found(selected->value),
                                    std::string{field_name} + " cannot contain metacharacters"));
    }
}

auto check_intent_path(const configuration_field* selected,
                       std::string_view field_name,
                       std::vector<configuration_diagnostic>& output) -> void {
    if (!require_type(selected, configuration_value_kind::path, output)) {
        return;
    }
    if (!canonical_absolute_path(selected->value.text)) {
        output.push_back(diagnostic(selected->value.location,
                                    "canonical absolute path",
                                    found(selected->value),
                                    std::string{field_name} +
                                        " begins with / and has no metacharacters, . or .."));
    }
}

auto require_install_field(const configuration_record& record,
                           std::string_view name,
                           std::vector<configuration_diagnostic>& output)
    -> const configuration_field* {
    return require_field(record,
                         name,
                         "install intent field '" + std::string{name} + "'",
                         "install intent records require every listed field",
                         output);
}

auto check_install_child(const configuration_record& record,
                         std::vector<configuration_diagnostic>& output) -> void {
    if (!record.records.empty()) {
        output.push_back(diagnostic(record.records.front().location,
                                    "install intent field",
                                    "nested record '" + record.records.front().kind + "'",
                                    "install intent records contain fields only"));
    }
    if (record.kind == "service") {
        static constexpr auto valid =
            std::array<std::string_view, 5>{"name", "exec", "user", "after", "restart"};
        check_fields(record, valid, output);
        check_intent_name(require_install_field(record, "name", output), "service name", output);
        check_intent_path(
            require_install_field(record, "exec", output), "service executable", output);
        check_intent_name(require_install_field(record, "user", output), "service user", output);
        const auto* after = require_install_field(record, "after", output);
        if (require_type(after, configuration_value_kind::list, output)) {
            for (const auto& value : after->value.list) {
                auto item = configuration_field{"after", value.location, value};
                check_intent_name(&item, "service dependency", output);
            }
        }
        const auto* restart = require_install_field(record, "restart", output);
        if (require_type(restart, configuration_value_kind::identifier, output) &&
            restart->value.text != "no" && restart->value.text != "on-failure" &&
            restart->value.text != "always") {
            output.push_back(diagnostic(restart->value.location,
                                        "no, on-failure, or always",
                                        found(restart->value),
                                        "service restart policy is a closed enum"));
        }
        return;
    }
    if (record.kind == "user") {
        static constexpr auto valid = std::array<std::string_view, 3>{"name", "system", "home"};
        check_fields(record, valid, output);
        check_intent_name(require_install_field(record, "name", output), "user name", output);
        require_type(require_install_field(record, "system", output),
                     configuration_value_kind::boolean,
                     output);
        check_intent_path(require_install_field(record, "home", output), "user home", output);
        return;
    }
    if (record.kind == "group") {
        static constexpr auto valid = std::array<std::string_view, 2>{"name", "system"};
        check_fields(record, valid, output);
        check_intent_name(require_install_field(record, "name", output), "group name", output);
        require_type(require_install_field(record, "system", output),
                     configuration_value_kind::boolean,
                     output);
        return;
    }
    if (record.kind == "directory") {
        static constexpr auto valid = std::array<std::string_view, 3>{"path", "mode", "owner"};
        check_fields(record, valid, output);
        check_intent_path(require_install_field(record, "path", output), "directory path", output);
        const auto* mode = require_install_field(record, "mode", output);
        if (require_type(mode, configuration_value_kind::integer, output) &&
            (mode->value.integer < 0 || mode->value.integer > 777 ||
             std::to_string(mode->value.integer).find_first_of("89") != std::string::npos)) {
            output.push_back(diagnostic(mode->value.location,
                                        "octal mode from 0000 through 0777",
                                        found(mode->value),
                                        "write modes with octal digits, for example 755"));
        }
        check_intent_name(
            require_install_field(record, "owner", output), "directory owner", output);
        return;
    }
    if (record.kind == "symlink") {
        static constexpr auto valid = std::array<std::string_view, 2>{"path", "target"};
        check_fields(record, valid, output);
        check_intent_path(require_install_field(record, "path", output), "symlink path", output);
        check_intent_path(
            require_install_field(record, "target", output), "symlink target", output);
        return;
    }
    if (record.kind == "cache") {
        static constexpr auto caches = std::array<std::string_view, 7>{"ldconfig",
                                                                       "desktop-database",
                                                                       "mime-database",
                                                                       "man-db",
                                                                       "icon-cache",
                                                                       "systemd-daemon-reload",
                                                                       "fonts"};
        static constexpr auto no_fields = std::array<std::string_view, 0>{};
        check_fields(record, no_fields, output);
        if (!record.name || std::ranges::find(caches, *record.name) == caches.end()) {
            output.push_back(
                diagnostic(record.location,
                           "known cache name",
                           record.name ? "'" + *record.name + "'" : "missing name",
                           "cache names are ldconfig, desktop-database, mime-database, "
                           "man-db, icon-cache, systemd-daemon-reload, or fonts"));
        }
        return;
    }
    if (record.kind == "alternatives") {
        static constexpr auto valid = std::array<std::string_view, 3>{"name", "path", "priority"};
        check_fields(record, valid, output);
        check_intent_name(
            require_install_field(record, "name", output), "alternative name", output);
        check_intent_path(
            require_install_field(record, "path", output), "alternative path", output);
        const auto* priority = require_install_field(record, "priority", output);
        if (require_type(priority, configuration_value_kind::integer, output) &&
            (priority->value.integer < std::numeric_limits<std::int32_t>::min() ||
             priority->value.integer > std::numeric_limits<std::int32_t>::max())) {
            output.push_back(diagnostic(priority->value.location,
                                        "32-bit signed integer",
                                        found(priority->value),
                                        "alternative priorities must fit the package formats"));
        }
        return;
    }
    static constexpr auto kinds = std::array<std::string_view, 7>{
        "service", "user", "group", "directory", "symlink", "cache", "alternatives"};
    output.push_back(
        diagnostic(record.location,
                   "install intent record",
                   "record '" + record.kind + "'",
                   "did you mean '" + std::string{nearest(record.kind, kinds)} + "'?"));
}

auto check_install(const configuration_record& record,
                   std::vector<configuration_diagnostic>& output) -> void {
    static constexpr auto install_fields = std::array<std::string_view, 2>{"layout", "prefix"};
    check_fields(record, install_fields, output);
    if (const auto* layout = field(record, "layout")) {
        if (require_type(layout, configuration_value_kind::identifier, output) &&
            layout->value.text != "prefix") {
            output.push_back(diagnostic(layout->value.location,
                                        "prefix layout",
                                        "identifier '" + layout->value.text + "'",
                                        "install trees contain bin, lib, include, and share"));
        }
    }
    if (const auto* prefix = field(record, "prefix")) {
        static constexpr auto accepted =
            std::array{configuration_value_kind::path, configuration_value_kind::string};
        require_one_of(prefix, accepted, "path", type_hint(configuration_value_kind::path), output);
    }
    for (const auto& child : record.records) {
        check_install_child(child, output);
    }
}

auto check_named(const configuration_record& record,
                 std::string_view kind,
                 std::vector<configuration_diagnostic>& output) -> void {
    if (!record.name || record.name->empty()) {
        output.push_back(diagnostic(record.location,
                                    std::string{kind} + " name",
                                    "missing name",
                                    "write " + std::string{kind} + " <name> { ... }"));
    }
}

auto scalar_text(const configuration_value& value) -> std::string {
    if (value.kind == configuration_value_kind::boolean) {
        return value.boolean ? "true" : "false";
    }
    if (value.kind == configuration_value_kind::integer) {
        return std::to_string(value.integer);
    }
    return value.text;
}

auto check_variant(const configuration_record& record,
                   std::vector<configuration_diagnostic>& output) -> void {
    static constexpr auto valid = std::array<std::string_view, 2>{"values", "default"};
    check_named(record, "variant", output);
    check_fields(record, valid, output);
    const auto* values =
        require_field(record, "values", "list", "variants declare values: [true, false]", output);
    if (require_type(values, configuration_value_kind::list, output)) {
        for (const auto& item : values->value.list) {
            if (item.kind != configuration_value_kind::boolean &&
                item.kind != configuration_value_kind::string) {
                output.push_back(diagnostic(item.location,
                                            "boolean or string",
                                            found(item),
                                            "variant values are scalar choices"));
            }
        }
    }
    const auto* default_value =
        require_field(record, "default", "variant value", "choose one value from values", output);
    if (default_value && default_value->value.kind != configuration_value_kind::boolean &&
        default_value->value.kind != configuration_value_kind::string) {
        output.push_back(diagnostic(default_value->value.location,
                                    "boolean or string",
                                    found(default_value->value),
                                    "the default must be one of the declared values"));
    } else if (default_value && values && values->value.kind == configuration_value_kind::list &&
               std::ranges::none_of(values->value.list, [&](const auto& value) {
                   return value.kind == default_value->value.kind &&
                          scalar_text(value) == scalar_text(default_value->value);
               })) {
        output.push_back(diagnostic(default_value->value.location,
                                    "declared variant value",
                                    found(default_value->value),
                                    "the default must be one of the declared values"));
    }
}

auto check_dependency(const configuration_record& record,
                      std::vector<configuration_diagnostic>& output) -> void {
    static constexpr auto valid = std::array<std::string_view, 3>{"virtual", "version", "type"};
    check_named(record, "depends_on", output);
    check_fields(record, valid, output);
    if (const auto* value = field(record, "virtual")) {
        require_type(value, configuration_value_kind::boolean, output);
    }
    if (const auto* value = field(record, "version")) {
        static constexpr auto accepted =
            std::array{configuration_value_kind::version, configuration_value_kind::string};
        require_one_of(value,
                       accepted,
                       "version constraint",
                       "exact versions are unquoted; ranges may be quoted",
                       output);
    }
    if (const auto* value = field(record, "type")) {
        if (require_type(value, configuration_value_kind::identifier, output) &&
            value->value.text != "build" && value->value.text != "link" &&
            value->value.text != "run") {
            output.push_back(diagnostic(value->value.location,
                                        "build, link, or run",
                                        "identifier '" + value->value.text + "'",
                                        "dependency types are build, link, and run"));
        }
    }
}

auto check_package_record(const configuration_record& record,
                          std::vector<configuration_diagnostic>& output,
                          bool conditional) -> void;

auto check_when(const configuration_record& record,
                std::vector<configuration_diagnostic>& output,
                bool package_context) -> void {
    if (!record.condition || !record.condition_text) {
        output.push_back(diagnostic(record.location,
                                    "spec constraint",
                                    "missing condition",
                                    "when blocks require one spec constraint"));
    }
    if (record.records.empty()) {
        output.push_back(diagnostic(record.location,
                                    "declaration inside when block",
                                    "empty block",
                                    "place conditional records inside the block"));
    }
    for (const auto& child : record.records) {
        if (package_context) {
            check_package_record(child, output, true);
        } else {
            output.push_back(diagnostic(child.location,
                                        "package member",
                                        "'" + child.kind + "'",
                                        "top-level when blocks are not supported"));
        }
    }
}

auto check_package_record(const configuration_record& record,
                          std::vector<configuration_diagnostic>& output,
                          bool conditional) -> void {
    if (record.kind == "variant") {
        check_variant(record, output);
        return;
    }
    if (record.kind == "depends_on") {
        check_dependency(record, output);
        return;
    }
    if (record.kind == "provides") {
        static constexpr auto valid = std::array<std::string_view, 1>{"version"};
        check_named(record, "provides", output);
        check_fields(record, valid, output);
        if (const auto* version = field(record, "version")) {
            static constexpr auto accepted =
                std::array{configuration_value_kind::version, configuration_value_kind::string};
            require_one_of(version,
                           accepted,
                           "version constraint",
                           "exact versions are unquoted; ranges may be quoted",
                           output);
        }
        return;
    }
    if (record.kind == "conflicts") {
        static constexpr auto valid = std::array<std::string_view, 2>{"constraint", "message"};
        check_fields(record, valid, output);
        const auto* constraint = require_field(
            record, "constraint", "spec", "use constraint: spec \"package @version\"", output);
        require_type(constraint, configuration_value_kind::spec, output);
        if (const auto* message = field(record, "message")) {
            require_type(message, configuration_value_kind::string, output);
        }
        return;
    }
    if (record.kind == "patch") {
        static constexpr auto valid = std::array<std::string_view, 1>{"digest"};
        check_fields(record, valid, output);
        const auto* value = require_field(
            record, "digest", "digest", type_hint(configuration_value_kind::digest), output);
        require_type(value, configuration_value_kind::digest, output);
        return;
    }
    if (record.kind == "when") {
        check_when(record, output, true);
        return;
    }
    output.push_back(diagnostic(record.location,
                                "variant, depends_on, provides, conflicts, patch, or when",
                                "record '" + record.kind + "'",
                                conditional
                                    ? "conditional package declarations use the same record names"
                                    : "move build and source records to the top level"));
}

auto check_package(const configuration_record& record,
                   std::vector<configuration_diagnostic>& output) -> void {
    static constexpr auto valid =
        std::array<std::string_view, 3>{"versions", "compilers", "targets"};
    check_named(record, "package", output);
    check_fields(record, valid, output);
    const auto* versions = require_field(
        record, "versions", "list of versions", "packages require versions: [1.14.3]", output);
    if (require_type(versions, configuration_value_kind::list, output)) {
        for (const auto& value : versions->value.list) {
            if (value.kind != configuration_value_kind::version) {
                output.push_back(diagnostic(value.location,
                                            "version",
                                            found(value),
                                            type_hint(configuration_value_kind::version)));
            }
        }
    }
    if (const auto* compilers = field(record, "compilers")) {
        check_string_list(compilers, "string compiler constraint", output);
    }
    if (const auto* targets = field(record, "targets")) {
        check_string_list(targets, "string target constraint", output);
    }
    for (const auto& child : record.records) {
        check_package_record(child, output, false);
    }
}

auto check_build(const configuration_record& record,
                 std::vector<configuration_diagnostic>& output,
                 bool generator) -> void {
    static constexpr auto build_fields = std::array<std::string_view, 8>{"command",
                                                                         "environment",
                                                                         "sources",
                                                                         "dependencies",
                                                                         "outputs",
                                                                         "cwd",
                                                                         "inputs",
                                                                         "toolchain"};
    static constexpr auto generator_fields = std::array<std::string_view, 8>{
        "command", "environment", "outputs", "cwd", "inputs", "toolchain", "output", "format"};
    if (!generator) {
        check_named(record, "build", output);
    }
    if (generator) {
        check_fields(record, generator_fields, output);
    } else {
        check_fields(record, build_fields, output);
    }
    const auto* command = require_field(record,
                                        "command",
                                        "list of strings",
                                        "commands are argv lists: [\"cc\", \"main.c\"]",
                                        output);
    check_string_list(command, "string command argument", output);
    if (const auto* environment = field(record, "environment")) {
        if (require_type(environment, configuration_value_kind::map, output)) {
            for (const auto& [name, value] : environment->value.map) {
                static_cast<void>(name);
                if (value.kind != configuration_value_kind::string) {
                    output.push_back(diagnostic(value.location,
                                                "string environment value",
                                                found(value),
                                                type_hint(configuration_value_kind::string)));
                }
            }
        }
    }
    if (const auto* sources = field(record, "sources")) {
        if (require_type(sources, configuration_value_kind::list, output)) {
            for (const auto& value : sources->value.list) {
                if (value.kind != configuration_value_kind::string &&
                    value.kind != configuration_value_kind::digest) {
                    output.push_back(diagnostic(value.location,
                                                "source name or digest",
                                                found(value),
                                                "sources are quoted names or declared digests"));
                }
            }
        }
    }
    if (const auto* dependencies = field(record, "dependencies")) {
        check_string_list(dependencies, "string dependency name", output);
    }
    if (const auto* outputs = field(record, "outputs")) {
        check_string_list(outputs, "path output", output, true);
    }
    if (const auto* cwd = field(record, "cwd")) {
        static constexpr auto accepted =
            std::array{configuration_value_kind::path, configuration_value_kind::string};
        require_one_of(cwd, accepted, "path", type_hint(configuration_value_kind::path), output);
    }
    for (const auto name : {"inputs", "toolchain"}) {
        if (const auto* value = field(record, name)) {
            require_type(value, configuration_value_kind::digest, output);
        }
    }
    if (generator) {
        const auto* output_path = require_field(
            record, "output", "path", "generators declare their .liquid or .cbor output", output);
        static constexpr auto accepted =
            std::array{configuration_value_kind::path, configuration_value_kind::string};
        require_one_of(
            output_path, accepted, "path", type_hint(configuration_value_kind::path), output);
        if (output_path && !output_path->value.text.ends_with(".liquid") &&
            !output_path->value.text.ends_with(".cbor")) {
            output.push_back(
                diagnostic(output_path->value.location,
                           ".liquid or .cbor output",
                           "path '" + output_path->value.text + "'",
                           "generators emit a .liquid file or canonical CBOR record set"));
        }
        for (const auto name : {"inputs", "toolchain"}) {
            const auto* value =
                require_field(record,
                              name,
                              "digest",
                              "generator actions require explicit input and toolchain trees",
                              output);
            require_type(value, configuration_value_kind::digest, output);
        }
        if (const auto* format = field(record, "format")) {
            if (require_type(format, configuration_value_kind::identifier, output) &&
                format->value.text != "liquid" && format->value.text != "cbor") {
                output.push_back(
                    diagnostic(format->value.location,
                               "liquid or cbor",
                               "identifier '" + format->value.text + "'",
                               "generators emit a .liquid file or canonical CBOR record set"));
            }
        }
    }
    for (const auto& child : record.records) {
        if (generator) {
            output.push_back(diagnostic(child.location,
                                        "generator field",
                                        "record '" + child.kind + "'",
                                        "generator inputs are declared by their input tree"));
            continue;
        }
        if (child.kind == "depends_on") {
            check_dependency(child, output);
        } else if (child.kind == "when") {
            for (const auto& conditional : child.records) {
                if (conditional.kind == "depends_on") {
                    check_dependency(conditional, output);
                } else {
                    output.push_back(diagnostic(conditional.location,
                                                "depends_on",
                                                "record '" + conditional.kind + "'",
                                                "build conditions may select dependencies"));
                }
            }
        } else if (child.kind == "install") {
            check_install(child, output);
        } else {
            output.push_back(diagnostic(child.location,
                                        "depends_on, when, or install",
                                        "record '" + child.kind + "'",
                                        "build records contain dependencies and install layout"));
        }
    }
}

struct source_declarations {
    std::set<std::string> names;
    std::set<std::string> digests;
};

auto collect_sources(const std::vector<configuration_record>& records, source_declarations& sources)
    -> void {
    for (const auto& record : records) {
        if (record.kind == "include") {
            collect_sources(record.records, sources);
            continue;
        }
        if (record.kind != "source") {
            continue;
        }
        if (record.name) {
            sources.names.insert(*record.name);
        }
        if (const auto* value = field(record, "digest");
            value && value->value.kind == configuration_value_kind::digest) {
            sources.digests.insert(value->value.text);
        }
    }
}

auto check_source_references(const std::vector<configuration_record>& records,
                             const source_declarations& sources,
                             std::vector<configuration_diagnostic>& output) -> void {
    for (const auto& record : records) {
        if (record.kind == "include") {
            check_source_references(record.records, sources, output);
            continue;
        }
        if (record.kind != "build") {
            continue;
        }
        const auto* selected = field(record, "sources");
        if (!selected || selected->value.kind != configuration_value_kind::list) {
            continue;
        }
        for (const auto& value : selected->value.list) {
            const auto declared = value.kind == configuration_value_kind::string
                                      ? sources.names.contains(value.text)
                                  : value.kind == configuration_value_kind::digest
                                      ? sources.digests.contains(value.text)
                                      : true;
            if (!declared) {
                output.push_back(diagnostic(value.location,
                                            "declared source",
                                            found(value),
                                            "declare its HTTPS URL and digest in a source record"));
            }
        }
    }
}

auto check_source(const configuration_record& record, std::vector<configuration_diagnostic>& output)
    -> void {
    static constexpr auto valid = std::array<std::string_view, 3>{"url", "digest", "kind"};
    check_named(record, "source", output);
    check_fields(record, valid, output);
    const auto* url =
        require_field(record, "url", "string URL", "sources require an https URL", output);
    if (require_type(url, configuration_value_kind::string, output) &&
        !url->value.text.starts_with("https://")) {
        output.push_back(diagnostic(url->value.location,
                                    "HTTPS URL",
                                    "string '" + url->value.text + "'",
                                    "source URLs begin with https://"));
    }
    const auto* content = require_field(
        record, "digest", "digest", "undeclared-digest fetches are forbidden", output);
    if (require_type(content, configuration_value_kind::digest, output)) {
        auto parsed = parse_digest(content->value.text);
        if (parsed && parsed->alg == algorithm::blake3_keyed) {
            output.push_back(diagnostic(content->value.location,
                                        "unkeyed content digest",
                                        found(content->value),
                                        "source digests use blake3 or sha256"));
        }
    }
    const auto* kind =
        require_field(record, "kind", "archive or file", "source kind is archive or file", output);
    if (require_type(kind, configuration_value_kind::identifier, output) &&
        kind->value.text != "archive" && kind->value.text != "file") {
        output.push_back(diagnostic(kind->value.location,
                                    "archive or file",
                                    "identifier '" + kind->value.text + "'",
                                    "source kind is archive or file"));
    }
}

auto check_lock(const configuration_record& record, std::vector<configuration_diagnostic>& output)
    -> void {
    static constexpr auto valid = std::array<std::string_view, 1>{"paths"};
    check_named(record, "lock", output);
    check_fields(record, valid, output);
    const auto* paths = require_field(
        record, "paths", "list of paths", "lock rules require paths: [path \"assets/**\"]", output);
    if (require_type(paths, configuration_value_kind::list, output)) {
        if (paths->value.list.empty()) {
            output.push_back(diagnostic(paths->value.location,
                                        "non-empty list of paths",
                                        "empty list",
                                        "declare at least one lock path pattern"));
        }
        for (const auto& value : paths->value.list) {
            if (value.kind != configuration_value_kind::path) {
                output.push_back(diagnostic(value.location,
                                            "path pattern",
                                            found(value),
                                            type_hint(configuration_value_kind::path)));
            }
        }
    }
    for (const auto& child : record.records) {
        output.push_back(diagnostic(child.location,
                                    "lock field",
                                    "record '" + child.kind + "'",
                                    "lock rules contain only path fields"));
    }
}

auto check_records(const std::vector<configuration_record>& records,
                   std::vector<configuration_diagnostic>& output,
                   std::set<std::pair<std::string, std::string>>& names) -> void {
    static constexpr auto top = std::array<std::string_view, 6>{
        "package", "build", "source", "lock", "generate", "include"};
    for (const auto& record : records) {
        if (record.kind == "include") {
            check_records(record.records, output, names);
            continue;
        }
        if (std::ranges::find(top, record.kind) == top.end()) {
            const auto suggestion = nearest(record.kind, top);
            output.push_back(diagnostic(record.location,
                                        "top-level record",
                                        "'" + record.kind + "'",
                                        "did you mean '" + std::string{suggestion} + "'?"));
            continue;
        }
        if (record.name && !names.emplace(record.kind, *record.name).second) {
            output.push_back(diagnostic(record.location,
                                        "unique " + record.kind + " name",
                                        "duplicate '" + *record.name + "'",
                                        "rename or combine the duplicate declarations"));
        }
        if (record.kind == "package") {
            check_package(record, output);
        } else if (record.kind == "build") {
            check_build(record, output, false);
        } else if (record.kind == "source") {
            check_source(record, output);
        } else if (record.kind == "lock") {
            check_lock(record, output);
        } else if (record.kind == "generate") {
            check_build(record, output, true);
        }
    }
}

auto parse_content(const configuration_value& value) -> result<digest> {
    auto parsed = parse_digest(value.text);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return *parsed;
}

auto strings(const configuration_field* value) -> std::vector<std::string> {
    auto output = std::vector<std::string>{};
    if (!value) {
        return output;
    }
    for (const auto& item : value->value.list) {
        output.push_back(scalar_text(item));
    }
    return output;
}

auto dependency_kind(const configuration_record& record) -> dependency_type {
    const auto* type = field(record, "type");
    if (!type || type->value.text == "link") {
        return dependency_type::link;
    }
    return type->value.text == "build" ? dependency_type::build : dependency_type::run;
}

auto lower_dependency(const configuration_record& record, std::optional<std::string> when)
    -> dependency_definition {
    const auto* version = field(record, "version");
    const auto* virtual_value = field(record, "virtual");
    return {.name = record.name.value_or(""),
            .virtual_dependency = virtual_value && virtual_value->value.boolean,
            .version =
                version ? std::optional<std::string>{scalar_text(version->value)} : std::nullopt,
            .when = std::move(when),
            .type = dependency_kind(record)};
}

auto lower_package(const configuration_record& record) -> result<package_record> {
    auto output = package_record{.name = *record.name};
    for (const auto& version : field(record, "versions")->value.list) {
        output.versions.push_back({version.text, std::nullopt});
    }
    output.compiler_constraints = strings(field(record, "compilers"));
    output.target_constraints = strings(field(record, "targets"));
    auto visit = [&](auto&& self,
                     const configuration_record& child,
                     std::optional<std::string> when) -> result<void> {
        if (child.kind == "when") {
            for (const auto& nested : child.records) {
                auto lowered = self(self, nested, child.condition_text);
                if (!lowered) {
                    return lowered;
                }
            }
            return {};
        }
        if (child.kind == "variant") {
            output.variants.push_back({.name = *child.name,
                                       .values = strings(field(child, "values")),
                                       .default_value = scalar_text(field(child, "default")->value),
                                       .when = std::move(when)});
        } else if (child.kind == "depends_on") {
            output.dependencies.push_back(lower_dependency(child, std::move(when)));
        } else if (child.kind == "provides") {
            const auto* version = field(child, "version");
            output.provides.push_back(
                {.name = *child.name,
                 .version = version ? std::optional<std::string>{scalar_text(version->value)}
                                    : std::nullopt});
        } else if (child.kind == "conflicts") {
            const auto* constraint = field(child, "constraint");
            auto parsed = parse_spec(constraint->value.text);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            const auto* message = field(child, "message");
            output.conflicts.push_back({.constraint = std::move(*parsed),
                                        .when = std::move(when),
                                        .message = message ? message->value.text : std::string{}});
        } else if (child.kind == "patch") {
            auto content = parse_content(field(child, "digest")->value);
            if (!content) {
                return std::unexpected(content.error());
            }
            output.patches.push_back({*content, std::move(when)});
        }
        return {};
    };
    for (const auto& child : record.records) {
        auto lowered = visit(visit, child, std::nullopt);
        if (!lowered) {
            return std::unexpected(lowered.error());
        }
    }
    return output;
}

auto origin_of(const configuration_record& record) -> configuration_record_origin {
    return {.kind = configuration_origin_kind::source, .file = record.location.file};
}

auto lower_source(const configuration_record& record) -> result<source> {
    auto content = parse_content(field(record, "digest")->value);
    if (!content) {
        return std::unexpected(content.error());
    }
    return source{.name = *record.name,
                  .url = field(record, "url")->value.text,
                  .content = *content,
                  .kind = field(record, "kind")->value.text == "file" ? source_kind::file
                                                                      : source_kind::archive,
                  .origin = origin_of(record)};
}

auto append_target_dependency(target& output,
                              const configuration_record& record,
                              std::optional<std::string> when) -> void {
    output.dependencies.push_back({record.name.value_or(""), std::move(when)});
}

auto lower_mode(std::int64_t value) -> std::uint32_t {
    auto output = std::uint32_t{};
    auto place = std::uint32_t{1};
    do {
        output += static_cast<std::uint32_t>(value % 10) * place;
        value /= 10;
        place *= 8;
    } while (value != 0);
    return output;
}

auto lower_restart(std::string_view value) -> service_restart {
    if (value == "no") {
        return service_restart::no;
    }
    if (value == "always") {
        return service_restart::always;
    }
    return service_restart::on_failure;
}

auto lower_cache(std::string_view value) -> install_cache {
    if (value == "desktop-database") {
        return install_cache::desktop_database;
    }
    if (value == "mime-database") {
        return install_cache::mime_database;
    }
    if (value == "man-db") {
        return install_cache::man_db;
    }
    if (value == "icon-cache") {
        return install_cache::icon_cache;
    }
    if (value == "systemd-daemon-reload") {
        return install_cache::systemd_daemon_reload;
    }
    if (value == "fonts") {
        return install_cache::fonts;
    }
    return install_cache::ldconfig;
}

auto lower_install(const configuration_record& record) -> install_intent {
    auto output = install_intent{};
    for (const auto& child : record.records) {
        if (child.kind == "service") {
            auto after = std::vector<std::string>{};
            for (const auto& item : field(child, "after")->value.list) {
                after.push_back(item.text);
            }
            output.services.push_back(
                {.name = field(child, "name")->value.text,
                 .executable = field(child, "exec")->value.text,
                 .user = field(child, "user")->value.text,
                 .after = std::move(after),
                 .restart = lower_restart(field(child, "restart")->value.text)});
        } else if (child.kind == "user") {
            output.users.push_back({.name = field(child, "name")->value.text,
                                    .system = field(child, "system")->value.boolean,
                                    .home = field(child, "home")->value.text});
        } else if (child.kind == "group") {
            output.groups.push_back({.name = field(child, "name")->value.text,
                                     .system = field(child, "system")->value.boolean});
        } else if (child.kind == "directory") {
            output.directories.push_back({.path = field(child, "path")->value.text,
                                          .mode = lower_mode(field(child, "mode")->value.integer),
                                          .owner = field(child, "owner")->value.text});
        } else if (child.kind == "symlink") {
            output.symlinks.push_back({.path = field(child, "path")->value.text,
                                       .target = field(child, "target")->value.text});
        } else if (child.kind == "cache") {
            output.caches.push_back(lower_cache(*child.name));
        } else if (child.kind == "alternatives") {
            output.alternatives.push_back(
                {.name = field(child, "name")->value.text,
                 .path = field(child, "path")->value.text,
                 .priority = static_cast<std::int32_t>(field(child, "priority")->value.integer)});
        }
    }
    return output;
}

auto lower_target(const configuration_record& record,
                  const std::map<std::string, digest>& declared_sources) -> result<target> {
    auto output = target{.name = *record.name,
                         .command = strings(field(record, "command")),
                         .origin = origin_of(record)};
    if (const auto* cwd = field(record, "cwd")) {
        output.cwd = cwd->value.text;
    }
    if (const auto* environment = field(record, "environment")) {
        for (const auto& [name, value] : environment->value.map) {
            output.environment.emplace(name, value.text);
        }
    }
    if (const auto* source_values = field(record, "sources")) {
        for (const auto& value : source_values->value.list) {
            if (value.kind == configuration_value_kind::digest) {
                auto content = parse_content(value);
                if (!content) {
                    return std::unexpected(content.error());
                }
                output.sources.push_back(*content);
            } else {
                const auto declared = declared_sources.find(value.text);
                if (declared == declared_sources.end()) {
                    return std::unexpected(
                        error{errc::not_found, "target source is not declared", value.text});
                }
                output.sources.push_back(declared->second);
            }
        }
    }
    for (const auto& name : strings(field(record, "dependencies"))) {
        output.dependencies.push_back({name, std::nullopt});
    }
    output.outputs = strings(field(record, "outputs"));
    for (const auto& child : record.records) {
        if (child.kind == "depends_on") {
            append_target_dependency(output, child, std::nullopt);
        } else if (child.kind == "when") {
            for (const auto& nested : child.records) {
                append_target_dependency(output, nested, child.condition_text);
            }
        } else if (child.kind == "install") {
            if (const auto* layout = field(child, "layout")) {
                output.install_layout = layout->value.text;
            }
            output.install = lower_install(child);
        }
    }
    for (const auto name : {"inputs", "toolchain"}) {
        if (const auto* value = field(record, name)) {
            auto content = parse_content(value->value);
            if (!content) {
                return std::unexpected(content.error());
            }
            if (name == std::string_view{"inputs"}) {
                output.inputs = *content;
            } else {
                output.toolchain = *content;
            }
        }
    }
    return output;
}

auto lower_generator(const configuration_record& record) -> result<configuration_generator> {
    auto inputs = parse_content(field(record, "inputs")->value);
    auto toolchain = parse_content(field(record, "toolchain")->value);
    if (!inputs || !toolchain) {
        return std::unexpected(!inputs ? inputs.error() : toolchain.error());
    }
    auto output = configuration_generator{
        .command = strings(field(record, "command")), .inputs = *inputs, .toolchain = *toolchain};
    if (const auto* cwd = field(record, "cwd")) {
        output.cwd = cwd->value.text;
    }
    if (const auto* environment = field(record, "environment")) {
        for (const auto& [name, value] : environment->value.map) {
            output.environment.emplace(name, value.text);
        }
    }
    output.output = field(record, "output")->value.text;
    output.declared_outputs = strings(field(record, "outputs"));
    if (std::ranges::find(output.declared_outputs, output.output) ==
        output.declared_outputs.end()) {
        output.declared_outputs.push_back(output.output);
    }
    if (const auto* format = field(record, "format")) {
        output.format = format->value.text;
    } else if (output.output.ends_with(".cbor")) {
        output.format = "cbor";
    }
    return output;
}

auto lower_records(const std::vector<configuration_record>& records,
                   lowered_configuration& output,
                   std::map<std::string, digest>& declared_sources) -> result<void> {
    for (const auto& record : records) {
        if (record.kind == "include") {
            auto nested = lower_records(record.records, output, declared_sources);
            if (!nested) {
                return nested;
            }
        } else if (record.kind == "source") {
            auto lowered = lower_source(record);
            if (!lowered) {
                return std::unexpected(lowered.error());
            }
            declared_sources.emplace(lowered->name, lowered->content);
            output.sources.push_back(std::move(*lowered));
        }
    }
    for (const auto& record : records) {
        if (record.kind == "package") {
            auto lowered = lower_package(record);
            if (!lowered) {
                return std::unexpected(lowered.error());
            }
            output.packages.push_back(std::move(*lowered));
            output.package_origins.push_back(origin_of(record));
        } else if (record.kind == "build") {
            auto lowered = lower_target(record, declared_sources);
            if (!lowered) {
                return std::unexpected(lowered.error());
            }
            output.targets.push_back(std::move(*lowered));
        } else if (record.kind == "generate") {
            auto lowered = lower_generator(record);
            if (!lowered) {
                return std::unexpected(lowered.error());
            }
            output.generators.push_back(std::move(*lowered));
        } else if (record.kind == "lock") {
            output.locks.rules.push_back(
                {.name = *record.name, .paths = strings(field(record, "paths"))});
        }
    }
    return {};
}

auto escape(std::string_view value) -> std::string {
    auto output = std::string{};
    for (const auto item : value) {
        switch (item) {
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
            output.push_back(item);
            break;
        }
    }
    return output;
}

auto format_name(std::string_view value) -> std::string {
    const auto identifier =
        !value.empty() &&
        (std::isalpha(static_cast<unsigned char>(value.front())) != 0 || value.front() == '_') &&
        std::ranges::all_of(value, [](char item) {
            return std::isalnum(static_cast<unsigned char>(item)) != 0 || item == '_' ||
                   item == '-' || item == '.';
        });
    return identifier ? std::string{value} : "\"" + escape(value) + "\"";
}

auto format_value(const configuration_value& value) -> std::string {
    switch (value.kind) {
    case configuration_value_kind::string:
        return "\"" + escape(value.text) + "\"";
    case configuration_value_kind::integer:
        return std::to_string(value.integer);
    case configuration_value_kind::boolean:
        return value.boolean ? "true" : "false";
    case configuration_value_kind::version:
    case configuration_value_kind::digest:
    case configuration_value_kind::identifier:
        return value.text;
    case configuration_value_kind::spec:
        return "spec \"" + escape(value.text) + "\"";
    case configuration_value_kind::path:
        return "path \"" + escape(value.text) + "\"";
    case configuration_value_kind::list: {
        auto output = std::string{"["};
        for (std::size_t index = 0; index < value.list.size(); ++index) {
            if (index != 0) {
                output += ", ";
            }
            output += format_value(value.list[index]);
        }
        return output + "]";
    }
    case configuration_value_kind::map: {
        auto output = std::string{"{"};
        auto first = true;
        for (const auto& [name, item] : value.map) {
            output += first ? " " : ", ";
            output += format_name(name) + ": " + format_value(item);
            first = false;
        }
        return output + (value.map.empty() ? "}" : " }");
    }
    }
    return {};
}

auto indent(std::uint64_t depth) -> std::string {
    return std::string(depth * 4, ' ');
}

auto format_record(const configuration_record& record,
                   std::uint64_t depth,
                   std::ostringstream& output) -> void {
    if (record.kind == "include") {
        output << indent(depth) << "include \"" << escape(record.fields.front().value.text)
               << "\"\n";
        return;
    }
    if (record.kind == "when") {
        output << indent(depth) << "when \"" << escape(*record.condition_text) << "\" {\n";
    } else if (record.kind == "generate") {
        output << indent(depth) << "generate = build {\n";
    } else {
        output << indent(depth) << record.kind;
        if (record.name) {
            output << ' ' << format_name(*record.name);
        }
        output << " {\n";
    }
    auto fields = std::vector<const configuration_field*>{};
    for (const auto& field_value : record.fields) {
        fields.push_back(&field_value);
    }
    std::ranges::sort(fields, {}, [](const auto* value) { return value->name; });
    for (const auto* field_value : fields) {
        output << indent(depth + 1) << field_value->name << ": " << format_value(field_value->value)
               << "\n";
    }
    for (const auto& child : record.records) {
        format_record(child, depth + 1, output);
    }
    output << indent(depth) << "}\n";
}

} // namespace

std::vector<configuration_diagnostic> check(const configuration_document& document) {
    auto output = std::vector<configuration_diagnostic>{};
    auto names = std::set<std::pair<std::string, std::string>>{};
    check_records(document.records, output, names);
    auto sources = source_declarations{};
    collect_sources(document.records, sources);
    check_source_references(document.records, sources, output);
    return output;
}

result<lowered_configuration> lower(const configuration_document& document) {
    const auto diagnostics = check(document);
    if (!diagnostics.empty()) {
        return std::unexpected(
            error{errc::invalid_argument, to_string(diagnostics.front()), "configuration checker"});
    }
    auto output = lowered_configuration{};
    auto declared_sources = std::map<std::string, digest>{};
    auto lowered = lower_records(document.records, output, declared_sources);
    if (!lowered) {
        return std::unexpected(lowered.error());
    }
    return output;
}

std::string to_string(const configuration_diagnostic& value) {
    return std::format("{}:{}:{}: expected {}, found {}\n  hint: {}",
                       value.location.file.generic_string(),
                       value.location.line,
                       value.location.column,
                       value.expected,
                       value.found,
                       value.hint);
}

result<std::string> format_configuration(const configuration_document& document) {
    const auto diagnostics = check(document);
    if (!diagnostics.empty()) {
        return std::unexpected(error{
            errc::invalid_argument, to_string(diagnostics.front()), "configuration formatter"});
    }
    auto output = std::ostringstream{};
    auto first = true;
    for (const auto& record : document.records) {
        if (!first) {
            output << '\n';
        }
        format_record(record, 0, output);
        first = false;
    }
    return output.str();
}

} // namespace liquid
