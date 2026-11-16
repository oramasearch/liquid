// SPDX-License-Identifier: MIT

#include "liquid/configuration.hpp"

#include <algorithm>
#include <utility>

#include "encoding/cbor.hpp"

namespace liquid {
namespace {

namespace cbor = encoding::cbor;

auto failure(std::string message) -> error {
    return {errc::corrupt, std::move(message), "configuration record"};
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

auto text(const cbor::value* value, std::string_view name) -> result<std::string> {
    const auto* output = value ? std::get_if<std::string>(&value->data) : nullptr;
    if (!output) {
        return std::unexpected(failure(std::string{name} + " is not text"));
    }
    return *output;
}

auto number(const cbor::value* value, std::string_view name) -> result<std::uint64_t> {
    const auto* output = value ? std::get_if<std::uint64_t>(&value->data) : nullptr;
    if (!output) {
        return std::unexpected(failure(std::string{name} + " is not an unsigned integer"));
    }
    return *output;
}

auto array(const cbor::value* value, std::string_view name) -> result<const cbor::value::array*> {
    const auto* output = value ? std::get_if<cbor::value::array>(&value->data) : nullptr;
    if (!output) {
        return std::unexpected(failure(std::string{name} + " is not an array"));
    }
    return output;
}

auto bytes(std::span<const std::byte> value) -> cbor::value {
    return cbor::value{cbor::value::bytes{value.begin(), value.end()}};
}

auto encode_digest(const digest& value) -> cbor::value {
    return map({{"alg", static_cast<std::uint64_t>(value.alg)}, {"bytes", bytes(value.bytes)}});
}

auto decode_digest(const cbor::value* value, std::string_view name) -> result<digest> {
    if (!value) {
        return std::unexpected(failure(std::string{name} + " is missing"));
    }
    auto algorithm_value = number(find(*value, "alg"), "digest algorithm");
    const auto* raw_value = find(*value, "bytes");
    const auto* raw = raw_value ? std::get_if<cbor::value::bytes>(&raw_value->data) : nullptr;
    if (!algorithm_value || *algorithm_value < 1 || *algorithm_value > 3 || !raw ||
        raw->size() != digest{}.bytes.size()) {
        return std::unexpected(failure(std::string{name} + " is not a digest"));
    }
    auto output = digest{static_cast<algorithm>(*algorithm_value), {}};
    std::ranges::copy(*raw, output.bytes.begin());
    return output;
}

auto encode_strings(std::span<const std::string> values) -> cbor::value {
    auto output = cbor::value::array{};
    for (const auto& value : values) {
        output.emplace_back(value);
    }
    return cbor::value{std::move(output)};
}

auto decode_strings(const cbor::value* value, std::string_view name)
    -> result<std::vector<std::string>> {
    auto values = array(value, name);
    if (!values) {
        return std::unexpected(values.error());
    }
    auto output = std::vector<std::string>{};
    for (const auto& item : **values) {
        auto value_text = text(&item, name);
        if (!value_text) {
            return std::unexpected(value_text.error());
        }
        output.push_back(std::move(*value_text));
    }
    return output;
}

auto encode_origin(const configuration_record_origin& value) -> cbor::value {
    auto fields = cbor::value::map{{"kind",
                                    value.kind == configuration_origin_kind::generated
                                        ? cbor::value{"generated"}
                                        : cbor::value{"source"}},
                                   {"file", value.file.generic_string()}};
    if (value.result) {
        fields.emplace_back("result", encode_digest(*value.result));
    }
    return cbor::value{std::move(fields)};
}

auto decode_origin(const cbor::value* value) -> result<configuration_record_origin> {
    if (!value) {
        return std::unexpected(failure("record origin is missing"));
    }
    auto kind = text(find(*value, "kind"), "origin kind");
    auto file = text(find(*value, "file"), "origin file");
    if (!kind || !file || (*kind != "source" && *kind != "generated")) {
        return std::unexpected(!kind   ? kind.error()
                               : !file ? file.error()
                                       : failure("unknown origin kind"));
    }
    auto output = configuration_record_origin{.kind = *kind == "generated"
                                                          ? configuration_origin_kind::generated
                                                          : configuration_origin_kind::source,
                                              .file = std::move(*file)};
    if (const auto* result_value = find(*value, "result")) {
        auto result_id = decode_digest(result_value, "origin result");
        if (!result_id) {
            return std::unexpected(result_id.error());
        }
        output.result = *result_id;
    }
    if (output.kind == configuration_origin_kind::generated && !output.result) {
        return std::unexpected(failure("generated origin is missing result"));
    }
    return output;
}

auto decode_record(std::span<const std::byte> bytes_value, std::string_view kind)
    -> result<cbor::value> {
    auto decoded = cbor::decode(bytes_value);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    auto canonical = cbor::encode(*decoded);
    if (!canonical || !std::ranges::equal(*canonical, bytes_value)) {
        return std::unexpected(failure(std::string{kind} + " is not canonical CBOR"));
    }
    auto record_kind = text(find(*decoded, "kind"), "record kind");
    auto version = number(find(*decoded, "v"), "record version");
    if (!record_kind || !version || *record_kind != kind || *version != 1) {
        return std::unexpected(failure("unsupported record kind or version"));
    }
    return *decoded;
}

} // namespace

result<std::vector<std::byte>> encode_source(const source& value) {
    if (value.name.empty() || value.url.empty()) {
        return std::unexpected(
            error{errc::invalid_argument, "source requires name and URL", "configuration record"});
    }
    return cbor::encode(
        map({{"v", value.v},
             {"kind", "source"},
             {"name", value.name},
             {"url", value.url},
             {"digest", encode_digest(value.content)},
             {"source_kind", value.kind == source_kind::archive ? "archive" : "file"},
             {"origin", encode_origin(value.origin)}}));
}

result<source> decode_source(std::span<const std::byte> bytes_value) {
    auto decoded = decode_record(bytes_value, "source");
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    auto name = text(find(*decoded, "name"), "source name");
    auto url = text(find(*decoded, "url"), "source URL");
    auto content = decode_digest(find(*decoded, "digest"), "source digest");
    auto kind = text(find(*decoded, "source_kind"), "source kind");
    auto origin = decode_origin(find(*decoded, "origin"));
    if (!name || !url || !content || !kind || !origin || (*kind != "archive" && *kind != "file")) {
        if (!name) {
            return std::unexpected(name.error());
        }
        if (!url) {
            return std::unexpected(url.error());
        }
        if (!content) {
            return std::unexpected(content.error());
        }
        if (!kind) {
            return std::unexpected(kind.error());
        }
        if (!origin) {
            return std::unexpected(origin.error());
        }
        return std::unexpected(failure("unknown source kind"));
    }
    return source{.name = std::move(*name),
                  .url = std::move(*url),
                  .content = *content,
                  .kind = *kind == "archive" ? source_kind::archive : source_kind::file,
                  .origin = std::move(*origin)};
}

result<std::vector<std::byte>> encode_target(const target& value) {
    if (value.name.empty() || value.command.empty()) {
        return std::unexpected(error{
            errc::invalid_argument, "target requires name and command", "configuration record"});
    }
    auto environment = cbor::value::map{};
    for (const auto& [name, selected] : value.environment) {
        environment.emplace_back(name, selected);
    }
    auto sources = cbor::value::array{};
    for (const auto& content : value.sources) {
        sources.push_back(encode_digest(content));
    }
    auto dependencies = cbor::value::array{};
    for (const auto& dependency : value.dependencies) {
        auto fields = cbor::value::map{{"name", dependency.name}};
        if (dependency.when) {
            fields.emplace_back("when", *dependency.when);
        }
        dependencies.emplace_back(std::move(fields));
    }
    auto fields = cbor::value::map{{"v", value.v},
                                   {"kind", "target"},
                                   {"name", value.name},
                                   {"command", encode_strings(value.command)},
                                   {"cwd", value.cwd},
                                   {"environment", cbor::value{std::move(environment)}},
                                   {"sources", cbor::value{std::move(sources)}},
                                   {"dependencies", cbor::value{std::move(dependencies)}},
                                   {"outputs", encode_strings(value.outputs)},
                                   {"install_layout", value.install_layout},
                                   {"origin", encode_origin(value.origin)}};
    if (value.install) {
        auto encoded_install = encode_install_intent(*value.install);
        if (!encoded_install) {
            return std::unexpected(encoded_install.error());
        }
        fields.emplace_back("install", bytes(*encoded_install));
    }
    if (value.inputs) {
        fields.emplace_back("inputs", encode_digest(*value.inputs));
    }
    if (value.toolchain) {
        fields.emplace_back("toolchain", encode_digest(*value.toolchain));
    }
    return cbor::encode(cbor::value{std::move(fields)});
}

result<target> decode_target(std::span<const std::byte> bytes_value) {
    auto decoded = decode_record(bytes_value, "target");
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    auto name = text(find(*decoded, "name"), "target name");
    auto command = decode_strings(find(*decoded, "command"), "target command");
    auto cwd = text(find(*decoded, "cwd"), "target cwd");
    auto sources = array(find(*decoded, "sources"), "target sources");
    auto dependencies = array(find(*decoded, "dependencies"), "target dependencies");
    auto outputs = decode_strings(find(*decoded, "outputs"), "target outputs");
    auto install_layout = text(find(*decoded, "install_layout"), "target install layout");
    auto origin = decode_origin(find(*decoded, "origin"));
    const auto* environment_value = find(*decoded, "environment");
    const auto* environment =
        environment_value ? std::get_if<cbor::value::map>(&environment_value->data) : nullptr;
    if (!name || !command || !cwd || !sources || !dependencies || !outputs || !install_layout ||
        !origin || !environment) {
        if (!name) {
            return std::unexpected(name.error());
        }
        if (!command) {
            return std::unexpected(command.error());
        }
        if (!cwd) {
            return std::unexpected(cwd.error());
        }
        if (!sources) {
            return std::unexpected(sources.error());
        }
        if (!dependencies) {
            return std::unexpected(dependencies.error());
        }
        if (!outputs) {
            return std::unexpected(outputs.error());
        }
        if (!install_layout) {
            return std::unexpected(install_layout.error());
        }
        if (!origin) {
            return std::unexpected(origin.error());
        }
        return std::unexpected(failure("target environment is not a map"));
    }
    auto output = target{.name = std::move(*name),
                         .command = std::move(*command),
                         .cwd = std::move(*cwd),
                         .outputs = std::move(*outputs),
                         .install_layout = std::move(*install_layout),
                         .origin = std::move(*origin)};
    if (const auto* install_value = find(*decoded, "install")) {
        const auto* install_bytes = std::get_if<cbor::value::bytes>(&install_value->data);
        if (!install_bytes) {
            return std::unexpected(failure("target install intent is not bytes"));
        }
        auto install = decode_install_intent(*install_bytes);
        if (!install) {
            return std::unexpected(install.error());
        }
        output.install = std::move(*install);
    }
    for (const auto& [key, selected] : *environment) {
        auto environment_name = text(&key, "environment name");
        auto environment_value_text = text(&selected, "environment value");
        if (!environment_name || !environment_value_text) {
            return std::unexpected(!environment_name ? environment_name.error()
                                                     : environment_value_text.error());
        }
        output.environment.emplace(std::move(*environment_name),
                                   std::move(*environment_value_text));
    }
    for (const auto& item : **sources) {
        auto content = decode_digest(&item, "target source");
        if (!content) {
            return std::unexpected(content.error());
        }
        output.sources.push_back(*content);
    }
    for (const auto& item : **dependencies) {
        auto dependency_name = text(find(item, "name"), "target dependency name");
        if (!dependency_name) {
            return std::unexpected(dependency_name.error());
        }
        auto dependency = target_dependency{.name = std::move(*dependency_name)};
        if (const auto* when_value = find(item, "when")) {
            auto when = text(when_value, "target dependency condition");
            if (!when) {
                return std::unexpected(when.error());
            }
            dependency.when = std::move(*when);
        }
        output.dependencies.push_back(std::move(dependency));
    }
    for (const auto name_value : {"inputs", "toolchain"}) {
        if (const auto* value = find(*decoded, name_value)) {
            auto content = decode_digest(value, name_value);
            if (!content) {
                return std::unexpected(content.error());
            }
            if (name_value == std::string_view{"inputs"}) {
                output.inputs = *content;
            } else {
                output.toolchain = *content;
            }
        }
    }
    return output;
}

} // namespace liquid
