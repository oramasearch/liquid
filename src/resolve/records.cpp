#include "liquid/resolve.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>
#include <utility>

#include "encoding/cbor.hpp"
#include "liquid/repository.hpp"

namespace liquid {
namespace {

namespace cbor = encoding::cbor;

auto corrupt(std::string message) -> error {
    return {errc::corrupt, std::move(message), "resolution record"};
}

auto invalid(std::string message) -> error {
    return {errc::invalid_argument, std::move(message), "resolution record"};
}

auto map(std::initializer_list<std::pair<cbor::value, cbor::value>> fields) -> cbor::value {
    return cbor::value{cbor::value::map{fields}};
}

auto find(const cbor::value& source, std::string_view name) -> const cbor::value* {
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

auto text(const cbor::value* value, std::string_view field) -> result<std::string> {
    const auto* output = value ? std::get_if<std::string>(&value->data) : nullptr;
    if (!output) {
        return std::unexpected(corrupt(std::string{field} + " is not text"));
    }
    return *output;
}

auto optional_text(const cbor::value* value, std::string_view field)
    -> result<std::optional<std::string>> {
    if (!value) {
        return std::optional<std::string>{};
    }
    auto output = text(value, field);
    if (!output) {
        return std::unexpected(output.error());
    }
    return std::optional<std::string>{std::move(*output)};
}

auto boolean(const cbor::value* value, std::string_view field) -> result<bool> {
    const auto* output = value ? std::get_if<bool>(&value->data) : nullptr;
    if (!output) {
        return std::unexpected(corrupt(std::string{field} + " is not boolean"));
    }
    return *output;
}

auto array(const cbor::value* value, std::string_view field) -> result<const cbor::value::array*> {
    const auto* output = value ? std::get_if<cbor::value::array>(&value->data) : nullptr;
    if (!output) {
        return std::unexpected(corrupt(std::string{field} + " is not an array"));
    }
    return output;
}

auto bytes(std::span<const std::byte> input) -> cbor::value {
    return cbor::value{cbor::value::bytes{input.begin(), input.end()}};
}

auto encode_digest(const digest& value) -> cbor::value {
    return map({{"alg", static_cast<std::uint64_t>(value.alg)}, {"bytes", bytes(value.bytes)}});
}

auto decode_digest(const cbor::value* value, std::string_view field) -> result<digest> {
    if (!value) {
        return std::unexpected(corrupt(std::string{field} + " is missing"));
    }
    const auto* algorithm_value = find(*value, "alg");
    const auto* bytes_value = find(*value, "bytes");
    const auto* algorithm_number =
        algorithm_value ? std::get_if<std::uint64_t>(&algorithm_value->data) : nullptr;
    const auto* raw = bytes_value ? std::get_if<cbor::value::bytes>(&bytes_value->data) : nullptr;
    if (!algorithm_number || *algorithm_number < 1 || *algorithm_number > 3 || !raw ||
        raw->size() != digest{}.bytes.size()) {
        return std::unexpected(corrupt(std::string{field} + " is not a digest"));
    }
    auto output = digest{static_cast<algorithm>(*algorithm_number), {}};
    std::ranges::copy(*raw, output.bytes.begin());
    return output;
}

auto encode_text_array(std::span<const std::string> values) -> cbor::value {
    auto output = cbor::value::array{};
    for (const auto& value : values) {
        output.emplace_back(value);
    }
    return cbor::value{std::move(output)};
}

auto decode_text_array(const cbor::value* value, std::string_view field)
    -> result<std::vector<std::string>> {
    auto values = array(value, field);
    if (!values) {
        return std::unexpected(values.error());
    }
    auto output = std::vector<std::string>{};
    for (const auto& item : **values) {
        auto decoded = text(&item, field);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        output.push_back(std::move(*decoded));
    }
    return output;
}

auto encode_text_map(const std::map<std::string, std::string>& values) -> cbor::value {
    auto output = cbor::value::map{};
    for (const auto& [name, value] : values) {
        output.emplace_back(name, value);
    }
    return cbor::value{std::move(output)};
}

auto decode_text_map(const cbor::value* value, std::string_view field)
    -> result<std::map<std::string, std::string>> {
    const auto* values = value ? std::get_if<cbor::value::map>(&value->data) : nullptr;
    if (!values) {
        return std::unexpected(corrupt(std::string{field} + " is not a map"));
    }
    auto output = std::map<std::string, std::string>{};
    for (const auto& [key, item] : *values) {
        auto name = text(&key, field);
        auto selected = text(&item, field);
        if (!name || !selected) {
            return std::unexpected(!name ? name.error() : selected.error());
        }
        output.emplace(std::move(*name), std::move(*selected));
    }
    return output;
}

auto encode_spec_value(const spec& value) -> cbor::value {
    auto dependencies = cbor::value::array{};
    for (const auto& dependency : value.dependencies) {
        dependencies.push_back(encode_spec_value(dependency));
    }
    auto fields = cbor::value::map{{"name", value.name},
                                   {"variants", encode_text_map(value.variants)},
                                   {"dependencies", cbor::value{std::move(dependencies)}}};
    if (value.version) {
        fields.emplace_back("version", *value.version);
    }
    if (value.compiler) {
        auto compiler = cbor::value::map{{"name", value.compiler->name}};
        if (value.compiler->version) {
            compiler.emplace_back("version", *value.compiler->version);
        }
        fields.emplace_back("compiler", cbor::value{std::move(compiler)});
    }
    if (value.target) {
        fields.emplace_back("target", *value.target);
    }
    return cbor::value{std::move(fields)};
}

auto decode_spec_value(const cbor::value& value) -> result<spec> {
    auto name = text(find(value, "name"), "spec name");
    auto version = optional_text(find(value, "version"), "spec version");
    auto variants = decode_text_map(find(value, "variants"), "spec variants");
    auto dependencies = array(find(value, "dependencies"), "spec dependencies");
    auto target = optional_text(find(value, "target"), "spec target");
    if (!name || !version || !variants || !dependencies || !target) {
        if (!name) {
            return std::unexpected(name.error());
        }
        if (!version) {
            return std::unexpected(version.error());
        }
        if (!variants) {
            return std::unexpected(variants.error());
        }
        if (!dependencies) {
            return std::unexpected(dependencies.error());
        }
        return std::unexpected(target.error());
    }
    auto output = spec{.name = std::move(*name),
                       .version = std::move(*version),
                       .variants = std::move(*variants),
                       .target = std::move(*target)};
    if (const auto* compiler_value = find(value, "compiler")) {
        auto compiler_name = text(find(*compiler_value, "name"), "compiler name");
        auto compiler_version = optional_text(find(*compiler_value, "version"), "compiler version");
        if (!compiler_name || !compiler_version) {
            return std::unexpected(!compiler_name ? compiler_name.error()
                                                  : compiler_version.error());
        }
        output.compiler = compiler_spec{std::move(*compiler_name), std::move(*compiler_version)};
    }
    for (const auto& dependency : **dependencies) {
        auto decoded = decode_spec_value(dependency);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        output.dependencies.push_back(std::move(*decoded));
    }
    return output;
}

auto type_name(dependency_type value) -> std::string_view {
    switch (value) {
    case dependency_type::build:
        return "build";
    case dependency_type::link:
        return "link";
    case dependency_type::run:
        return "run";
    }
    return "link";
}

auto decode_type(const cbor::value* value) -> result<dependency_type> {
    auto name = text(value, "dependency type");
    if (!name) {
        return std::unexpected(name.error());
    }
    if (*name == "build") {
        return dependency_type::build;
    }
    if (*name == "link") {
        return dependency_type::link;
    }
    if (*name == "run") {
        return dependency_type::run;
    }
    return std::unexpected(corrupt("unknown dependency type"));
}

auto decode_record(std::span<const std::byte> input, std::string_view kind) -> result<cbor::value> {
    auto decoded = cbor::decode(input);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    auto decoded_kind = text(find(*decoded, "kind"), "record kind");
    const auto* version_value = find(*decoded, "v");
    const auto* version =
        version_value ? std::get_if<std::uint64_t>(&version_value->data) : nullptr;
    if (!decoded_kind || *decoded_kind != kind || !version || *version != 1) {
        return std::unexpected(corrupt("unsupported record kind or version"));
    }
    return decoded;
}

auto validate(package_universe universe) -> result<package_universe> {
    std::ranges::sort(universe.packages, {}, &package_record::name);
    auto names = std::set<std::string>{};
    for (auto& package : universe.packages) {
        if (package.name.empty() || package.versions.empty()) {
            return std::unexpected(invalid("packages require a name and at least one version"));
        }
        if (!names.insert(package.name).second) {
            return std::unexpected(invalid("duplicate package '" + package.name + "'"));
        }
        std::ranges::sort(package.versions, {}, &package_version::version);
        std::ranges::sort(package.variants, {}, &variant_definition::name);
        std::ranges::sort(package.dependencies, [](const auto& left, const auto& right) {
            return std::tie(left.name, left.type) < std::tie(right.name, right.type);
        });
    }
    return universe;
}

auto read_directory(std::filesystem::path directory) -> result<package_universe> {
    auto root = directory / "packages";
    if (!std::filesystem::is_directory(root)) {
        root = std::move(directory);
    }
    if (!std::filesystem::is_directory(root)) {
        return std::unexpected(
            error{errc::not_found, "package universe directory does not exist", root.string()});
    }
    auto paths = std::vector<std::filesystem::path>{};
    std::error_code failure_code;
    for (auto iterator = std::filesystem::recursive_directory_iterator{root, failure_code};
         !failure_code && iterator != std::filesystem::recursive_directory_iterator{};
         iterator.increment(failure_code)) {
        if (iterator->is_regular_file() && iterator->path().extension() == ".cbor") {
            paths.push_back(iterator->path());
        }
    }
    if (failure_code) {
        return std::unexpected(error{errc::io, failure_code.message(), root.string()});
    }
    std::ranges::sort(paths);
    auto universe = package_universe{};
    for (const auto& path : paths) {
        auto stream = std::ifstream{path, std::ios::binary};
        if (!stream) {
            return std::unexpected(error{errc::io, "cannot read package record", path.string()});
        }
        const auto chars = std::vector<char>{std::istreambuf_iterator<char>{stream}, {}};
        auto data = std::vector<std::byte>{};
        data.reserve(chars.size());
        for (const auto value : chars) {
            data.push_back(std::byte{static_cast<unsigned char>(value)});
        }
        auto package = decode_package(data);
        if (!package) {
            auto issue = package.error();
            issue.context = path.string();
            return std::unexpected(std::move(issue));
        }
        universe.packages.push_back(std::move(*package));
    }
    return validate(std::move(universe));
}

runtime::task<void> collect_repository_packages(repository* source,
                                                tree directory,
                                                std::string prefix,
                                                std::vector<package_record>* output) {
    for (const auto& entry : directory.entries) {
        auto path = prefix.empty() ? entry.name : prefix + "/" + entry.name;
        if (entry.kind == node_kind::tree) {
            auto nested = co_await source->tree(entry.ref);
            if (!nested) {
                co_return std::unexpected(nested.error());
            }
            auto collected = co_await collect_repository_packages(
                source, std::move(*nested), std::move(path), output);
            if (!collected) {
                co_return std::unexpected(collected.error());
            }
            continue;
        }
        if (entry.kind != node_kind::blob || !path.starts_with("packages/") ||
            !path.ends_with(".cbor")) {
            continue;
        }
        auto data = co_await source->read(path);
        if (!data) {
            co_return std::unexpected(data.error());
        }
        auto package = decode_package(*data);
        if (!package) {
            co_return std::unexpected(package.error());
        }
        output->push_back(std::move(*package));
    }
    co_return {};
}

} // namespace

result<std::vector<std::byte>> encode_package(const package_record& value) {
    if (value.name.empty() || value.versions.empty()) {
        return std::unexpected(invalid("packages require a name and at least one version"));
    }
    auto versions = cbor::value::array{};
    for (const auto& version : value.versions) {
        auto fields = cbor::value::map{{"version", version.version}};
        if (version.source) {
            fields.emplace_back("source", encode_digest(*version.source));
        }
        versions.emplace_back(std::move(fields));
    }
    auto variants = cbor::value::array{};
    for (const auto& variant : value.variants) {
        auto fields = cbor::value::map{{"name", variant.name},
                                       {"values", encode_text_array(variant.values)},
                                       {"default", variant.default_value}};
        if (variant.when) {
            fields.emplace_back("when", *variant.when);
        }
        variants.emplace_back(std::move(fields));
    }
    auto dependencies = cbor::value::array{};
    for (const auto& dependency : value.dependencies) {
        auto fields = cbor::value::map{{"name", dependency.name},
                                       {"virtual", dependency.virtual_dependency},
                                       {"type", std::string{type_name(dependency.type)}}};
        if (dependency.version) {
            fields.emplace_back("version", *dependency.version);
        }
        if (dependency.when) {
            fields.emplace_back("when", *dependency.when);
        }
        dependencies.emplace_back(std::move(fields));
    }
    auto provides = cbor::value::array{};
    for (const auto& provided : value.provides) {
        auto fields = cbor::value::map{{"name", provided.name}};
        if (provided.version) {
            fields.emplace_back("version", *provided.version);
        }
        provides.emplace_back(std::move(fields));
    }
    auto conflicts = cbor::value::array{};
    for (const auto& conflict : value.conflicts) {
        auto fields = cbor::value::map{{"constraint", encode_spec_value(conflict.constraint)},
                                       {"message", conflict.message}};
        if (conflict.when) {
            fields.emplace_back("when", *conflict.when);
        }
        conflicts.emplace_back(std::move(fields));
    }
    auto patches = cbor::value::array{};
    for (const auto& patch : value.patches) {
        auto fields = cbor::value::map{{"digest", encode_digest(patch.value)}};
        if (patch.when) {
            fields.emplace_back("when", *patch.when);
        }
        patches.emplace_back(std::move(fields));
    }
    return cbor::encode(map({{"v", std::uint64_t{1}},
                             {"kind", "package"},
                             {"name", value.name},
                             {"versions", cbor::value{std::move(versions)}},
                             {"variants", cbor::value{std::move(variants)}},
                             {"dependencies", cbor::value{std::move(dependencies)}},
                             {"provides", cbor::value{std::move(provides)}},
                             {"conflicts", cbor::value{std::move(conflicts)}},
                             {"compilers", encode_text_array(value.compiler_constraints)},
                             {"targets", encode_text_array(value.target_constraints)},
                             {"patches", cbor::value{std::move(patches)}}}));
}

result<package_record> decode_package(std::span<const std::byte> bytes_value) {
    auto decoded = decode_record(bytes_value, "package");
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    auto name = text(find(*decoded, "name"), "package name");
    auto versions = array(find(*decoded, "versions"), "package versions");
    auto variants = array(find(*decoded, "variants"), "package variants");
    auto dependencies = array(find(*decoded, "dependencies"), "package dependencies");
    auto provides = array(find(*decoded, "provides"), "package provides");
    auto conflicts = array(find(*decoded, "conflicts"), "package conflicts");
    auto compilers = decode_text_array(find(*decoded, "compilers"), "package compilers");
    auto targets = decode_text_array(find(*decoded, "targets"), "package targets");
    auto patches = array(find(*decoded, "patches"), "package patches");
    if (!name || !versions || !variants || !dependencies || !provides || !conflicts || !compilers ||
        !targets || !patches) {
        if (!name) {
            return std::unexpected(name.error());
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
        if (!compilers) {
            return std::unexpected(compilers.error());
        }
        if (!targets) {
            return std::unexpected(targets.error());
        }
        return std::unexpected(patches.error());
    }
    auto output = package_record{.name = std::move(*name),
                                 .compiler_constraints = std::move(*compilers),
                                 .target_constraints = std::move(*targets)};
    for (const auto& item : **versions) {
        auto version = text(find(item, "version"), "package version");
        if (!version) {
            return std::unexpected(version.error());
        }
        auto source = std::optional<digest>{};
        if (const auto* source_value = find(item, "source")) {
            auto value = decode_digest(source_value, "version source");
            if (!value) {
                return std::unexpected(value.error());
            }
            source = *value;
        }
        output.versions.push_back({std::move(*version), source});
    }
    for (const auto& item : **variants) {
        auto variant_name = text(find(item, "name"), "variant name");
        auto values = decode_text_array(find(item, "values"), "variant values");
        auto default_value = text(find(item, "default"), "variant default");
        auto when = optional_text(find(item, "when"), "variant condition");
        if (!variant_name || !values || !default_value || !when) {
            if (!variant_name) {
                return std::unexpected(variant_name.error());
            }
            if (!values) {
                return std::unexpected(values.error());
            }
            if (!default_value) {
                return std::unexpected(default_value.error());
            }
            return std::unexpected(when.error());
        }
        output.variants.push_back({std::move(*variant_name),
                                   std::move(*values),
                                   std::move(*default_value),
                                   std::move(*when)});
    }
    for (const auto& item : **dependencies) {
        auto dependency_name = text(find(item, "name"), "dependency name");
        auto virtual_value = boolean(find(item, "virtual"), "dependency virtual flag");
        auto version = optional_text(find(item, "version"), "dependency version");
        auto when = optional_text(find(item, "when"), "dependency condition");
        auto type = decode_type(find(item, "type"));
        if (!dependency_name || !virtual_value || !version || !when || !type) {
            if (!dependency_name) {
                return std::unexpected(dependency_name.error());
            }
            if (!virtual_value) {
                return std::unexpected(virtual_value.error());
            }
            if (!version) {
                return std::unexpected(version.error());
            }
            if (!when) {
                return std::unexpected(when.error());
            }
            return std::unexpected(type.error());
        }
        output.dependencies.push_back({std::move(*dependency_name),
                                       *virtual_value,
                                       std::move(*version),
                                       std::move(*when),
                                       *type});
    }
    for (const auto& item : **provides) {
        auto provide_name = text(find(item, "name"), "provided virtual");
        auto version = optional_text(find(item, "version"), "provided version");
        if (!provide_name || !version) {
            return std::unexpected(!provide_name ? provide_name.error() : version.error());
        }
        output.provides.push_back({std::move(*provide_name), std::move(*version)});
    }
    for (const auto& item : **conflicts) {
        const auto* constraint_value = find(item, "constraint");
        if (!constraint_value) {
            return std::unexpected(corrupt("conflict constraint is missing"));
        }
        auto constraint = decode_spec_value(*constraint_value);
        auto when = optional_text(find(item, "when"), "conflict condition");
        auto message = text(find(item, "message"), "conflict message");
        if (!constraint || !when || !message) {
            if (!constraint) {
                return std::unexpected(constraint.error());
            }
            if (!when) {
                return std::unexpected(when.error());
            }
            return std::unexpected(message.error());
        }
        output.conflicts.push_back({std::move(*constraint), std::move(*when), std::move(*message)});
    }
    for (const auto& item : **patches) {
        auto patch = decode_digest(find(item, "digest"), "patch digest");
        auto when = optional_text(find(item, "when"), "patch condition");
        if (!patch || !when) {
            return std::unexpected(!patch ? patch.error() : when.error());
        }
        output.patches.push_back({*patch, std::move(*when)});
    }
    if (output.versions.empty()) {
        return std::unexpected(corrupt("package has no versions"));
    }
    return output;
}

result<std::vector<std::byte>> encode_concretised_spec(const concretised_spec& value) {
    auto dependencies = cbor::value::array{};
    for (const auto& dependency : value.dependencies) {
        dependencies.push_back(map({{"name", dependency.name},
                                    {"type", std::string{type_name(dependency.type)}},
                                    {"target", encode_digest(dependency.target)},
                                    {"virtual", dependency.virtual_dependency}}));
    }
    auto patches = cbor::value::array{};
    for (const auto& patch : value.patches) {
        patches.push_back(encode_digest(patch));
    }
    auto fields = cbor::value::map{{"v", std::uint64_t{1}},
                                   {"kind", "concretised_spec"},
                                   {"name", value.name},
                                   {"version", value.version},
                                   {"variants", encode_text_map(value.variants)},
                                   {"target", value.target},
                                   {"dependencies", cbor::value{std::move(dependencies)}},
                                   {"patches", cbor::value{std::move(patches)}},
                                   {"reasons", encode_text_array(value.reasons)}};
    if (value.compiler) {
        fields.emplace_back("compiler", encode_digest(*value.compiler));
    }
    return cbor::encode(cbor::value{std::move(fields)});
}

result<concretised_spec> decode_concretised_spec(std::span<const std::byte> bytes_value) {
    auto decoded = decode_record(bytes_value, "concretised_spec");
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    auto name = text(find(*decoded, "name"), "concretised name");
    auto version = text(find(*decoded, "version"), "concretised version");
    auto variants = decode_text_map(find(*decoded, "variants"), "concretised variants");
    auto target = text(find(*decoded, "target"), "concretised target");
    auto dependencies = array(find(*decoded, "dependencies"), "concretised dependencies");
    auto patches = array(find(*decoded, "patches"), "concretised patches");
    auto reasons = decode_text_array(find(*decoded, "reasons"), "concretised reasons");
    if (!name || !version || !variants || !target || !dependencies || !patches || !reasons) {
        if (!name) {
            return std::unexpected(name.error());
        }
        if (!version) {
            return std::unexpected(version.error());
        }
        if (!variants) {
            return std::unexpected(variants.error());
        }
        if (!target) {
            return std::unexpected(target.error());
        }
        if (!dependencies) {
            return std::unexpected(dependencies.error());
        }
        if (!patches) {
            return std::unexpected(patches.error());
        }
        return std::unexpected(reasons.error());
    }
    auto output = concretised_spec{.name = std::move(*name),
                                   .version = std::move(*version),
                                   .variants = std::move(*variants),
                                   .target = std::move(*target),
                                   .reasons = std::move(*reasons)};
    if (const auto* compiler_value = find(*decoded, "compiler")) {
        auto compiler = decode_digest(compiler_value, "concretised compiler");
        if (!compiler) {
            return std::unexpected(compiler.error());
        }
        output.compiler = *compiler;
    }
    for (const auto& item : **dependencies) {
        auto dependency_name = text(find(item, "name"), "edge name");
        auto type = decode_type(find(item, "type"));
        auto edge_target = decode_digest(find(item, "target"), "edge target");
        auto virtual_value = boolean(find(item, "virtual"), "edge virtual flag");
        if (!dependency_name || !type || !edge_target || !virtual_value) {
            if (!dependency_name) {
                return std::unexpected(dependency_name.error());
            }
            if (!type) {
                return std::unexpected(type.error());
            }
            if (!edge_target) {
                return std::unexpected(edge_target.error());
            }
            return std::unexpected(virtual_value.error());
        }
        output.dependencies.push_back(
            {std::move(*dependency_name), *type, *edge_target, *virtual_value});
    }
    for (const auto& item : **patches) {
        auto patch = decode_digest(&item, "concretised patch");
        if (!patch) {
            return std::unexpected(patch.error());
        }
        output.patches.push_back(*patch);
    }
    auto identity = hash(algorithm::blake3, bytes_value);
    if (!identity) {
        return std::unexpected(identity.error());
    }
    output.id = *identity;
    return output;
}

runtime::task<package_universe> load_package_universe(std::filesystem::path directory) {
    auto loaded = co_await runtime::offload(
        [directory = std::move(directory)] { return read_directory(directory); });
    if (!loaded) {
        co_return std::unexpected(loaded.error());
    }
    if (!*loaded) {
        co_return std::unexpected(loaded->error());
    }
    co_return std::move(**loaded);
}

runtime::task<package_universe> load_package_universe(repository& source) {
    auto root = co_await source.root();
    if (!root) {
        co_return std::unexpected(root.error());
    }
    auto packages = std::vector<package_record>{};
    auto collected = co_await collect_repository_packages(&source, std::move(*root), {}, &packages);
    if (!collected) {
        co_return std::unexpected(collected.error());
    }
    auto checked = validate(package_universe{std::move(packages)});
    if (!checked) {
        co_return std::unexpected(checked.error());
    }
    co_return std::move(*checked);
}

} // namespace liquid
