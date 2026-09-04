#include "liquid/build.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string_view>
#include <utility>

#include "build/cache.hpp"
#include "build/repository_access.hpp"
#include "build/sbom.hpp"
#include "build/wrap.hpp"
#include "formats/json.hpp"
#include "identity/taap.hpp"
#include "objects/records.hpp"
#include "vfs/repository_access.hpp"

namespace liquid {
namespace {

namespace json = formats::json;

struct sbom_model {
    digest subject;
    std::string name;
    std::chrono::sys_seconds created;
    bool derived{};
    bool inferred{};
    std::vector<sbom_component> components;
    std::vector<sbom_relationship> relationships;
    std::vector<std::string> evidence;
    std::map<std::string, std::string> component_ids;
    std::set<std::string> relationship_ids;
};

auto same_digest(const digest& left, const digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

auto format_name(sbom_format format) -> std::string_view {
    return format == sbom_format::spdx ? "SPDX-2.3" : "CycloneDX-1.5";
}

auto timestamp(std::chrono::sys_seconds value) -> std::string {
    const auto day = std::chrono::floor<std::chrono::days>(value);
    const auto date = std::chrono::year_month_day{day};
    const auto time = std::chrono::hh_mm_ss{value - day};
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z",
                       static_cast<int>(date.year()),
                       static_cast<unsigned>(date.month()),
                       static_cast<unsigned>(date.day()),
                       time.hours().count(),
                       time.minutes().count(),
                       time.seconds().count());
}

auto json_text(std::string value) -> json::value {
    return json::value{std::move(value)};
}

auto json_number(std::uint64_t value) -> json::value {
    return json::value{json::number{std::to_string(value)}};
}

auto digest_algorithm(algorithm value) -> std::string_view {
    switch (value) {
    case algorithm::blake3:
        return "BLAKE3";
    case algorithm::sha256:
        return "SHA-256";
    case algorithm::blake3_keyed:
        return "BLAKE3-KEYED";
    }
    return "UNKNOWN";
}

auto digest_hex(const digest& value) -> std::string {
    const auto text = to_string(value);
    const auto separator = text.find(':');
    return separator == std::string::npos ? text : text.substr(separator + 1);
}

auto node_text(const node_id& value) -> std::string {
    constexpr auto digits = std::string_view{"0123456789abcdef"};
    auto output = std::string{};
    output.reserve(value.size() * 2);
    for (const auto byte : value) {
        const auto item = std::to_integer<unsigned char>(byte);
        output.push_back(digits[item >> 4U]);
        output.push_back(digits[item & 0x0fU]);
    }
    return output;
}

auto component_claims(const sbom_model& model) -> std::vector<std::string> {
    auto output = std::vector<std::string>{};
    output.reserve(model.components.size());
    for (const auto& component : model.components) {
        auto claim = component.kind + "|" + component.name + "|";
        if (component.content) {
            claim += to_string(*component.content);
        } else if (component.pinned_change) {
            claim += to_string(*component.pinned_change);
        } else {
            claim += "none";
        }
        if (component.repository_id) {
            claim += "|repo=" + *component.repository_id;
        }
        if (component.pinned_change) {
            claim += "|pin=" + to_string(*component.pinned_change);
        }
        claim += component.inferred ? "|inferred" : "|derived";
        output.push_back(std::move(claim));
    }
    return output;
}

auto attribute(const attestation& value, std::string_view name) -> const attestation_value* {
    const auto found = std::ranges::find(value.attributes, name, &attestation_attribute::name);
    return found == value.attributes.end() ? nullptr : &found->value;
}

auto text_attribute(const attestation& value, std::string_view name) -> const std::string* {
    const auto* item = attribute(value, name);
    return item ? std::get_if<std::string>(item) : nullptr;
}

auto bool_attribute(const attestation& value, std::string_view name) -> const bool* {
    const auto* item = attribute(value, name);
    return item ? std::get_if<bool>(item) : nullptr;
}

auto list_attribute(const attestation& value, std::string_view name)
    -> const std::vector<std::string>* {
    const auto* item = attribute(value, name);
    return item ? std::get_if<std::vector<std::string>>(item) : nullptr;
}

auto component_id(std::size_t index) -> std::string {
    return "component-" + std::to_string(index + 1);
}

auto component_key(const sbom_component& component) -> std::string {
    auto output = component.kind + "\n" + component.name + "\n";
    if (component.content) {
        output += to_string(*component.content);
    }
    output += "\n";
    if (component.repository_id) {
        output += *component.repository_id;
    }
    output += "\n";
    if (component.pinned_change) {
        output += to_string(*component.pinned_change);
    }
    return output;
}

auto add_component(sbom_model& model, sbom_component component) -> std::string {
    const auto key = component_key(component);
    if (const auto found = model.component_ids.find(key); found != model.component_ids.end()) {
        return found->second;
    }
    component.id = component_id(model.components.size());
    const auto id = component.id;
    model.components.push_back(std::move(component));
    model.component_ids.emplace(key, id);
    return id;
}

void add_relationship(sbom_model& model, std::string from, std::string to, std::string kind) {
    const auto key = from + "\n" + to + "\n" + kind;
    if (model.relationship_ids.insert(key).second) {
        model.relationships.push_back({std::move(from), std::move(to), std::move(kind)});
    }
}

class graph_builder {
public:
    graph_builder(repository& source, std::vector<objects::action_index_entry> entries)
        : source_(source), entries_(std::move(entries)) {}

    auto build(const objects::action_record& action,
               const action_result& value,
               const action_output& target,
               std::optional<digest> current_result) -> runtime::task<sbom_model> {
        tool_components_.clear();
        repository_components_.clear();
        auto indexed = co_await index_producers();
        if (!indexed) {
            co_return std::unexpected(indexed.error());
        }
        auto model = sbom_model{.subject = target.blob,
                                .name = target.path,
                                .created = value.finished_at.value_or(value.observed_at),
                                .derived = true};
        const auto root = add_component(
            model,
            {.name = target.path, .kind = "artifact", .content = target.blob, .inferred = false});
        auto visited = std::vector<digest>{};
        auto before_sequence = std::numeric_limits<std::uint64_t>::max();
        if (current_result) {
            visited.push_back(*current_result);
            const auto current = std::ranges::find_if(entries_, [&](const auto& entry) {
                return same_digest(entry.result, *current_result);
            });
            if (current != entries_.end()) {
                before_sequence = current->sequence;
            }
        }
        auto expanded = co_await visit_action(model, action, value, root, visited, before_sequence);
        if (!expanded) {
            co_return std::unexpected(expanded.error());
        }
        for (const auto& component : model.components) {
            if (component.id != root) {
                add_relationship(model, root, component.id, "contains");
            }
        }
        co_return model;
    }

private:
    struct producer {
        digest id;
        std::uint64_t sequence;
        action_output output;
    };

    auto load_result(digest id) -> runtime::task<const action_result*> {
        const auto key = to_string(id);
        if (const auto found = results_.find(key); found != results_.end()) {
            co_return &found->second;
        }
        auto bytes = co_await repository_detail::build_access::get(source_, id);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        auto decoded = objects::decode_result(*bytes);
        if (!decoded) {
            co_return std::unexpected(decoded.error());
        }
        const auto [stored, inserted] = results_.emplace(key, std::move(*decoded));
        static_cast<void>(inserted);
        co_return &stored->second;
    }

    auto load_action(digest id) -> runtime::task<const objects::action_record*> {
        const auto key = to_string(id);
        if (const auto found = actions_.find(key); found != actions_.end()) {
            co_return &found->second;
        }
        auto bytes = co_await repository_detail::build_access::get(source_, id);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        auto decoded = objects::decode_action(*bytes);
        if (!decoded) {
            co_return std::unexpected(decoded.error());
        }
        const auto [stored, inserted] = actions_.emplace(key, std::move(*decoded));
        static_cast<void>(inserted);
        co_return &stored->second;
    }

    auto index_producers() -> runtime::task<void> {
        if (producers_indexed_) {
            co_return result<void>{};
        }
        for (const auto& entry : entries_) {
            if (entry.divergent) {
                continue;
            }
            auto value = co_await load_result(entry.result);
            if (!value) {
                if (value.error().code == errc::not_found) {
                    continue;
                }
                co_return std::unexpected(value.error());
            }
            for (const auto& output : (*value)->outputs) {
                producers_[to_string(output.blob)].push_back(
                    producer{entry.result, entry.sequence, output});
            }
        }
        producers_indexed_ = true;
        co_return result<void>{};
    }

    auto find_producer(digest content,
                       std::span<const digest> visited,
                       std::uint64_t before_sequence) const -> std::optional<producer> {
        const auto found = producers_.find(to_string(content));
        if (found == producers_.end()) {
            return std::nullopt;
        }
        const producer* newest = nullptr;
        for (const auto& candidate : found->second) {
            if (candidate.sequence >= before_sequence ||
                std::ranges::any_of(
                    visited, [&](const auto& id) { return same_digest(id, candidate.id); })) {
                continue;
            }
            if (!newest || candidate.sequence > newest->sequence) {
                newest = &candidate;
            }
        }
        if (newest) {
            return *newest;
        }
        return std::nullopt;
    }

    auto collect_toolchain(sbom_model& model,
                           digest root,
                           std::string prefix,
                           std::vector<digest>& visited)
        -> runtime::task<std::vector<std::string>> {
        const auto cache_key = to_string(root) + "\n" + prefix;
        if (const auto found = tool_components_.find(cache_key); found != tool_components_.end()) {
            co_return found->second;
        }
        if (std::ranges::any_of(visited,
                                [&](const auto& value) { return same_digest(value, root); })) {
            co_return std::vector<std::string>{};
        }
        visited.push_back(root);
        auto bytes = co_await repository_detail::build_access::get(source_, root);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        auto tree = objects::decode_tree(*bytes);
        if (!tree) {
            co_return std::unexpected(error{tree.error().code,
                                            "invalid toolchain tree: " + tree.error().message,
                                            prefix.empty() ? to_string(root) : prefix});
        }
        auto output = std::vector<std::string>{};
        for (const auto& entry : tree->entries) {
            const auto path = prefix.empty() ? entry.name : prefix + "/" + entry.name;
            if (entry.kind == node_kind::tree) {
                auto nested = co_await collect_toolchain(model, entry.ref, path, visited);
                if (!nested) {
                    co_return std::unexpected(nested.error());
                }
                output.insert(output.end(), nested->begin(), nested->end());
                continue;
            }
            if (entry.kind != node_kind::blob) {
                co_return std::unexpected(
                    error{errc::corrupt, "toolchain tree contains a non-blob entry", path});
            }
            output.push_back(add_component(
                model, {.name = path, .kind = "tool", .content = entry.ref, .inferred = false}));
        }
        tool_components_.emplace(cache_key, output);
        co_return output;
    }

    auto collect_repositories(sbom_model& model,
                              digest root,
                              std::string prefix,
                              std::vector<digest>& visited)
        -> runtime::task<std::vector<std::string>> {
        const auto cache_key = to_string(root) + "\n" + prefix;
        if (const auto found = repository_components_.find(cache_key);
            found != repository_components_.end()) {
            co_return found->second;
        }
        if (std::ranges::any_of(visited,
                                [&](const auto& value) { return same_digest(value, root); })) {
            co_return std::vector<std::string>{};
        }
        visited.push_back(root);
        auto bytes = co_await repository_detail::build_access::get(source_, root);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        auto tree = objects::decode_tree(*bytes);
        if (!tree) {
            co_return std::unexpected(error{tree.error().code,
                                            "invalid input tree: " + tree.error().message,
                                            prefix.empty() ? to_string(root) : prefix});
        }
        auto output = std::vector<std::string>{};
        for (const auto& entry : tree->entries) {
            const auto path = prefix.empty() ? entry.name : prefix + "/" + entry.name;
            if (entry.kind == node_kind::tree) {
                auto nested = co_await collect_repositories(model, entry.ref, path, visited);
                if (!nested) {
                    co_return std::unexpected(nested.error());
                }
                output.insert(output.end(), nested->begin(), nested->end());
                continue;
            }
            if (entry.kind != node_kind::repo) {
                continue;
            }
            output.push_back(add_component(model,
                                           {.name = path,
                                            .kind = "repository",
                                            .repository_id = node_text(entry.node),
                                            .pinned_change = entry.ref,
                                            .inferred = false}));
        }
        repository_components_.emplace(cache_key, output);
        co_return output;
    }

    auto visit_action(sbom_model& model,
                      const objects::action_record& action,
                      const action_result& value,
                      const std::string& artifact,
                      std::vector<digest>& visited,
                      std::uint64_t before_sequence) -> runtime::task<void> {
        auto tool_trees = std::vector<digest>{};
        auto tools = co_await collect_toolchain(model, action.input.toolchain, {}, tool_trees);
        if (!tools) {
            co_return std::unexpected(tools.error());
        }
        for (const auto& tool : *tools) {
            add_relationship(model, artifact, tool, "depends_on");
        }
        auto input_trees = std::vector<digest>{};
        auto repositories =
            co_await collect_repositories(model, action.input.inputs, {}, input_trees);
        if (!repositories) {
            co_return std::unexpected(repositories.error());
        }
        for (const auto& repository : *repositories) {
            add_relationship(model, artifact, repository, "generated_from");
        }
        for (const auto& input : value.manifest) {
            auto producer = find_producer(input.content, visited, before_sequence);
            if (!producer) {
                const auto component = add_component(model,
                                                     {.name = input.path,
                                                      .kind = "input",
                                                      .content = input.content,
                                                      .inferred = false});
                add_relationship(model, artifact, component, "generated_from");
                continue;
            }
            const auto component = add_component(model,
                                                 {.name = producer->output.path,
                                                  .kind = "artifact",
                                                  .content = producer->output.blob,
                                                  .inferred = false});
            add_relationship(model, artifact, component, "generated_from");
            visited.push_back(producer->id);
            auto producer_value = co_await load_result(producer->id);
            if (!producer_value) {
                co_return std::unexpected(producer_value.error());
            }
            auto action_value = co_await load_action((*producer_value)->action);
            if (!action_value) {
                co_return std::unexpected(action_value.error());
            }
            auto expanded = co_await visit_action(
                model, **action_value, **producer_value, component, visited, producer->sequence);
            if (!expanded) {
                co_return std::unexpected(expanded.error());
            }
        }
        co_return result<void>{};
    }

    repository& source_;
    std::vector<objects::action_index_entry> entries_;
    bool producers_indexed_{};
    std::map<std::string, std::vector<producer>> producers_;
    std::map<std::string, action_result> results_;
    std::map<std::string, objects::action_record> actions_;
    std::map<std::string, std::vector<std::string>> tool_components_;
    std::map<std::string, std::vector<std::string>> repository_components_;
};

void retarget_model(sbom_model& model, const action_output& target) {
    model.subject = target.blob;
    model.name = target.path;
    if (!model.components.empty()) {
        model.components.front().name = target.path;
        model.components.front().content = target.blob;
    }
}

auto spdx_id(std::string_view id) -> std::string {
    auto output = std::string{"SPDXRef-"};
    for (const auto character : id) {
        output.push_back(std::isalnum(static_cast<unsigned char>(character)) || character == '.' ||
                                 character == '-'
                             ? character
                             : '-');
    }
    return output;
}

auto component_properties(const sbom_component& component) -> json::value {
    auto properties = json::value::array{};
    const auto put = [&properties](std::string name, std::string value) {
        properties.emplace_back(json::value::object{{"name", json_text(std::move(name))},
                                                    {"value", json_text(std::move(value))}});
    };
    put("liquid:kind", component.kind);
    put("liquid:inferred", component.inferred ? "true" : "false");
    if (component.content) {
        put("liquid:digest", to_string(*component.content));
    }
    if (component.repository_id) {
        put("liquid:repo-id", *component.repository_id);
    }
    if (component.pinned_change) {
        put("liquid:pinned-change", to_string(*component.pinned_change));
    }
    return json::value{std::move(properties)};
}

auto cyclonedx_component(const sbom_component& component) -> json::value {
    const auto file = component.kind == "input" || component.kind == "artifact" ||
                      component.kind.starts_with("binary/");
    auto output = json::value::object{{"type", json_text(file ? "file" : "library")},
                                      {"bom-ref", json_text(component.id)},
                                      {"name", json_text(component.name)},
                                      {"properties", component_properties(component)}};
    if (component.content && component.content->alg != algorithm::blake3_keyed) {
        auto hashes = json::value::array{};
        hashes.emplace_back(json::value::object{
            {"alg", json_text(std::string{digest_algorithm(component.content->alg)})},
            {"content", json_text(digest_hex(*component.content))}});
        output.emplace_back("hashes", json::value{std::move(hashes)});
    }
    return json::value{std::move(output)};
}

auto attachment_properties(const objects::sbom_record& record,
                           digest record_id,
                           digest attestation_id,
                           const attestation& statement) -> result<json::value> {
    auto encoded = objects::encode(statement);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    auto output = json::value::array{};
    const auto put = [&output](std::string name, std::string value) {
        output.emplace_back(json::value::object{{"name", json_text(std::move(name))},
                                                {"value", json_text(std::move(value))}});
    };
    put("liquid:sbom-record", to_string(record_id));
    put("liquid:sbom-attestation", to_string(attestation_id));
    put("liquid:sbom-attestation-cbor", identity_detail::base64(*encoded));
    put("liquid:sbom-record-format", record.format);
    return json::value{std::move(output)};
}

auto cyclonedx_document(const sbom_model& model,
                        std::optional<json::value> attachment = std::nullopt)
    -> result<std::string> {
    if (model.components.empty()) {
        return std::unexpected(error{errc::invalid_argument, "SBOM has no subject component", {}});
    }
    auto metadata =
        json::value::object{{"timestamp", json_text(timestamp(model.created))},
                            {"component", cyclonedx_component(model.components.front())}};
    auto properties = json::value::array{};
    for (const auto& evidence : model.evidence) {
        properties.emplace_back(json::value::object{{"name", json_text("liquid:analysis-evidence")},
                                                    {"value", json_text(evidence)}});
    }
    if (attachment) {
        auto* attached = std::get_if<json::value::array>(&attachment->data);
        if (!attached) {
            return std::unexpected(
                error{errc::internal, "invalid CycloneDX attachment properties", {}});
        }
        properties.insert(properties.end(),
                          std::make_move_iterator(attached->begin()),
                          std::make_move_iterator(attached->end()));
    }
    if (!properties.empty()) {
        metadata.emplace_back("properties", json::value{std::move(properties)});
    }
    auto components = json::value::array{};
    for (std::size_t index = 1; index < model.components.size(); ++index) {
        components.push_back(cyclonedx_component(model.components[index]));
    }
    auto dependencies = json::value::array{};
    for (const auto& component : model.components) {
        auto depends_on = json::value::array{};
        for (const auto& relationship : model.relationships) {
            if (relationship.from == component.id) {
                depends_on.emplace_back(relationship.to);
            }
        }
        dependencies.emplace_back(json::value::object{
            {"ref", json_text(component.id)}, {"dependsOn", json::value{std::move(depends_on)}}});
    }
    auto assemblies = json::value::array{};
    for (std::size_t index = 1; index < model.components.size(); ++index) {
        assemblies.emplace_back(model.components[index].id);
    }
    auto document = json::value::object{{"bomFormat", json_text("CycloneDX")},
                                        {"specVersion", json_text("1.5")},
                                        {"version", json_number(1)},
                                        {"metadata", json::value{std::move(metadata)}},
                                        {"dependencies", json::value{std::move(dependencies)}}};
    if (!components.empty()) {
        document.emplace_back("components", json::value{std::move(components)});
    }
    if (!assemblies.empty()) {
        auto compositions = json::value::array{};
        compositions.emplace_back(
            json::value::object{{"aggregate", json_text("complete")},
                                {"assemblies", json::value{std::move(assemblies)}}});
        document.emplace_back("compositions", json::value{std::move(compositions)});
    }
    return json::write(json::value{std::move(document)});
}

auto spdx_package(const sbom_component& component) -> json::value {
    auto references = json::value::array{};
    if (component.content) {
        references.emplace_back(
            json::value::object{{"referenceCategory", json_text("OTHER")},
                                {"referenceType", json_text("liquid-digest")},
                                {"referenceLocator", json_text(to_string(*component.content))}});
    }
    if (component.repository_id) {
        references.emplace_back(
            json::value::object{{"referenceCategory", json_text("OTHER")},
                                {"referenceType", json_text("liquid-repository")},
                                {"referenceLocator", json_text(*component.repository_id)}});
    }
    if (component.pinned_change) {
        references.emplace_back(json::value::object{
            {"referenceCategory", json_text("OTHER")},
            {"referenceType", json_text("liquid-pinned-change")},
            {"referenceLocator", json_text(to_string(*component.pinned_change))}});
    }
    auto output =
        json::value::object{{"SPDXID", json_text(spdx_id(component.id))},
                            {"name", json_text(component.name)},
                            {"downloadLocation", json_text("NOASSERTION")},
                            {"filesAnalyzed", json::value{false}},
                            {"copyrightText", json_text("NOASSERTION")},
                            {"comment",
                             json_text("liquid:kind=" + component.kind + "; liquid:inferred=" +
                                       (component.inferred ? "true" : "false"))}};
    if (!references.empty()) {
        output.emplace_back("externalRefs", json::value{std::move(references)});
    }
    if (component.content && component.content->alg == algorithm::sha256) {
        auto checksums = json::value::array{};
        checksums.emplace_back(
            json::value::object{{"algorithm", json_text("SHA256")},
                                {"checksumValue", json_text(digest_hex(*component.content))}});
        output.emplace_back("checksums", json::value{std::move(checksums)});
    }
    return json::value{std::move(output)};
}

auto spdx_relationship_type(std::string_view kind) -> std::string {
    if (kind == "contains") {
        return "CONTAINS";
    }
    if (kind == "depends_on") {
        return "DEPENDS_ON";
    }
    return "GENERATED_FROM";
}

auto spdx_document(const sbom_model& model, std::optional<json::value> annotation = std::nullopt)
    -> result<std::string> {
    if (model.components.empty()) {
        return std::unexpected(error{errc::invalid_argument, "SBOM has no subject component", {}});
    }
    auto creators = json::value::array{};
    creators.emplace_back(std::string{"Tool: liquid"});
    auto creation = json::value::object{{"created", json_text(timestamp(model.created))},
                                        {"creators", json::value{std::move(creators)}}};
    auto packages = json::value::array{};
    for (const auto& component : model.components) {
        packages.push_back(spdx_package(component));
    }
    auto relationships = json::value::array{};
    relationships.emplace_back(json::value::object{
        {"spdxElementId", json_text("SPDXRef-DOCUMENT")},
        {"relationshipType", json_text("DESCRIBES")},
        {"relatedSpdxElement", json_text(spdx_id(model.components.front().id))}});
    for (const auto& relationship : model.relationships) {
        relationships.emplace_back(json::value::object{
            {"spdxElementId", json_text(spdx_id(relationship.from))},
            {"relationshipType", json_text(spdx_relationship_type(relationship.kind))},
            {"relatedSpdxElement", json_text(spdx_id(relationship.to))}});
    }
    auto document = json::value::object{
        {"spdxVersion", json_text("SPDX-2.3")},
        {"dataLicense", json_text("CC0-1.0")},
        {"SPDXID", json_text("SPDXRef-DOCUMENT")},
        {"name", json_text(model.name)},
        {"documentNamespace", json_text("https://liquid.dev/sbom/" + to_string(model.subject))},
        {"creationInfo", json::value{std::move(creation)}},
        {"packages", json::value{std::move(packages)}},
        {"relationships", json::value{std::move(relationships)}}};
    if (!model.evidence.empty()) {
        auto comment = std::string{"liquid binary analysis evidence:"};
        for (const auto& evidence : model.evidence) {
            comment += "\n" + evidence;
        }
        document.emplace_back("comment", json_text(std::move(comment)));
    }
    if (annotation) {
        auto annotations = json::value::array{};
        annotations.push_back(std::move(*annotation));
        document.emplace_back("annotations", json::value{std::move(annotations)});
    }
    return json::write(json::value{std::move(document)});
}

auto render_document(const sbom_model& model,
                     sbom_format format,
                     std::optional<json::value> attachment = std::nullopt) -> result<std::string> {
    return format == sbom_format::spdx ? spdx_document(model, std::move(attachment))
                                       : cyclonedx_document(model, std::move(attachment));
}

auto make_statement(const sbom_model& model, sbom_format format, digest record, actor issuer)
    -> attestation {
    return {.subject = model.subject,
            .profile = "sbom/1",
            .at = model.created,
            .issuer = std::move(issuer),
            .subject_kind = attestation_subject_kind::artifact,
            .attributes = {{"format", std::string{format_name(format)}},
                           {"output", model.name},
                           {"document", to_string(record)},
                           {"derived", model.derived},
                           {"inferred", model.inferred},
                           {"components", component_claims(model)}}};
}

auto encode_record(const sbom_model& model, sbom_format format)
    -> result<std::pair<objects::sbom_record, std::vector<std::byte>>> {
    auto document = render_document(model, format);
    if (!document) {
        return std::unexpected(document.error());
    }
    auto record = objects::sbom_record{
        std::string{format_name(format)},
        {reinterpret_cast<const std::byte*>(document->data()),
         reinterpret_cast<const std::byte*>(document->data() + document->size())}};
    auto encoded = objects::encode(record);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return std::pair{std::move(record), std::move(*encoded)};
}

auto spdx_attachment(const objects::sbom_record& record,
                     digest record_id,
                     digest attestation_id,
                     const attestation& statement) -> result<json::value> {
    auto encoded = objects::encode(statement);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    auto comment = "liquid:profile=sbom/1; record=" + to_string(record_id) +
                   "; attestation=" + to_string(attestation_id) + "; format=" + record.format +
                   "; cbor=" + identity_detail::base64(*encoded);
    return json::value{json::value::object{{"annotator", json_text("Tool: liquid")},
                                           {"annotationDate", json_text(timestamp(statement.at))},
                                           {"annotationType", json_text("OTHER")},
                                           {"comment", json_text(std::move(comment))}}};
}

auto render_attached(const sbom_model& model,
                     sbom_format format,
                     const objects::sbom_record& record,
                     digest record_id,
                     digest attestation_id,
                     const attestation& statement) -> result<std::string> {
    auto attachment = format == sbom_format::spdx
                          ? spdx_attachment(record, record_id, attestation_id, statement)
                          : attachment_properties(record, record_id, attestation_id, statement);
    if (!attachment) {
        return std::unexpected(attachment.error());
    }
    return render_document(model, format, std::move(*attachment));
}

auto read_u16(std::span<const std::byte> bytes, std::size_t offset, bool little)
    -> std::optional<std::uint16_t> {
    if (offset + 2 > bytes.size()) {
        return std::nullopt;
    }
    const auto a = std::to_integer<std::uint16_t>(bytes[offset]);
    const auto b = std::to_integer<std::uint16_t>(bytes[offset + 1]);
    return little ? static_cast<std::uint16_t>(a | (b << 8U))
                  : static_cast<std::uint16_t>((a << 8U) | b);
}

auto read_u32(std::span<const std::byte> bytes, std::size_t offset, bool little)
    -> std::optional<std::uint32_t> {
    if (offset + 4 > bytes.size()) {
        return std::nullopt;
    }
    auto output = std::uint32_t{};
    for (auto index = 0U; index < 4; ++index) {
        const auto source = little ? index : 3U - index;
        output |= std::to_integer<std::uint32_t>(bytes[offset + source]) << (index * 8U);
    }
    return output;
}

auto read_u64(std::span<const std::byte> bytes, std::size_t offset, bool little)
    -> std::optional<std::uint64_t> {
    if (offset + 8 > bytes.size()) {
        return std::nullopt;
    }
    auto output = std::uint64_t{};
    for (auto index = 0U; index < 8; ++index) {
        const auto source = little ? index : 7U - index;
        output |= std::to_integer<std::uint64_t>(bytes[offset + source]) << (index * 8U);
    }
    return output;
}

auto fixed_string(std::span<const std::byte> bytes, std::size_t offset, std::size_t size)
    -> std::string {
    if (offset >= bytes.size()) {
        return {};
    }
    size = std::min(size, bytes.size() - offset);
    auto output = std::string{};
    for (auto index = std::size_t{}; index < size; ++index) {
        const auto value = std::to_integer<unsigned char>(bytes[offset + index]);
        if (value == 0) {
            break;
        }
        if (value < 0x20 || value > 0x7e) {
            return {};
        }
        output.push_back(static_cast<char>(value));
    }
    return output;
}

auto printable_strings(std::span<const std::byte> bytes) -> std::vector<std::string> {
    auto output = std::vector<std::string>{};
    auto current = std::string{};
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned char>(byte);
        if (value >= 0x20 && value <= 0x7e) {
            if (current.size() < 512) {
                current.push_back(static_cast<char>(value));
            }
            continue;
        }
        if (current.size() >= 4 && output.size() < 20'000) {
            output.push_back(std::move(current));
        }
        current.clear();
    }
    if (current.size() >= 4 && output.size() < 20'000) {
        output.push_back(std::move(current));
    }
    return output;
}

void inspect_elf(std::span<const std::byte> bytes,
                 std::vector<std::string>& sections,
                 std::vector<std::string>& symbols) {
    if (bytes.size() < 64 || std::to_integer<unsigned char>(bytes[4]) < 1 ||
        std::to_integer<unsigned char>(bytes[4]) > 2) {
        return;
    }
    const auto is64 = std::to_integer<unsigned char>(bytes[4]) == 2;
    const auto little = std::to_integer<unsigned char>(bytes[5]) == 1;
    const auto section_offset = is64 ? read_u64(bytes, 40, little)
                                     : std::optional<std::uint64_t>{read_u32(bytes, 32, little)};
    const auto entry_size = read_u16(bytes, is64 ? 58 : 46, little);
    const auto count = read_u16(bytes, is64 ? 60 : 48, little);
    const auto string_index = read_u16(bytes, is64 ? 62 : 50, little);
    if (!section_offset || !entry_size || !count || !string_index || *string_index >= *count) {
        return;
    }
    const auto header = *section_offset + static_cast<std::uint64_t>(*entry_size) * *string_index;
    const auto names_offset =
        is64 ? read_u64(bytes, header + 24, little)
             : std::optional<std::uint64_t>{read_u32(bytes, header + 16, little)};
    const auto names_size =
        is64 ? read_u64(bytes, header + 32, little)
             : std::optional<std::uint64_t>{read_u32(bytes, header + 20, little)};
    if (!names_offset || !names_size || *names_offset + *names_size > bytes.size()) {
        return;
    }
    for (auto index = std::uint16_t{}; index < *count; ++index) {
        const auto at = *section_offset + static_cast<std::uint64_t>(*entry_size) * index;
        const auto name = read_u32(bytes, at, little);
        if (!name || *name >= *names_size) {
            continue;
        }
        const auto value = fixed_string(bytes, *names_offset + *name, 96);
        if (!value.empty()) {
            sections.push_back(value);
        }
    }
    const auto strings = printable_strings(bytes);
    for (const auto& value : strings) {
        if ((value.starts_with("_") || value.find("::") != std::string::npos ||
             value.starts_with("GLIBC")) &&
            symbols.size() < 256) {
            symbols.push_back(value);
        }
    }
}

void inspect_macho(std::span<const std::byte> bytes,
                   std::vector<std::string>& sections,
                   std::vector<std::string>& symbols) {
    const auto magic = read_u32(bytes, 0, true);
    if (!magic) {
        return;
    }
    const auto is64 = *magic == 0xfeedfacfU || *magic == 0xcffaedfeU;
    const auto little = *magic == 0xfeedfaceU || *magic == 0xfeedfacfU;
    const auto commands = read_u32(bytes, 16, little);
    auto offset = static_cast<std::size_t>(is64 ? 32 : 28);
    if (!commands) {
        return;
    }
    for (auto index = std::uint32_t{}; index < *commands && offset + 8 <= bytes.size(); ++index) {
        const auto command = read_u32(bytes, offset, little);
        const auto size = read_u32(bytes, offset + 4, little);
        if (!command || !size || *size < 8 || offset + *size > bytes.size()) {
            return;
        }
        if (*command == (is64 ? 0x19U : 0x1U)) {
            const auto count = read_u32(bytes, offset + (is64 ? 64 : 48), little).value_or(0);
            auto section = offset + (is64 ? 72 : 56);
            const auto stride = static_cast<std::size_t>(is64 ? 80 : 68);
            for (auto item = std::uint32_t{}; item < count && section + stride <= offset + *size;
                 ++item) {
                const auto name = fixed_string(bytes, section, 16);
                if (!name.empty()) {
                    sections.push_back(name);
                }
                section += stride;
            }
        }
        if (*command == 0x2U) {
            const auto symbol_offset = read_u32(bytes, offset + 8, little).value_or(0);
            const auto symbol_count = read_u32(bytes, offset + 12, little).value_or(0);
            const auto string_offset = read_u32(bytes, offset + 16, little).value_or(0);
            const auto string_size = read_u32(bytes, offset + 20, little).value_or(0);
            const auto stride = static_cast<std::size_t>(is64 ? 16 : 12);
            for (auto item = std::uint32_t{};
                 item < symbol_count && symbols.size() < 256 &&
                 symbol_offset + (static_cast<std::uint64_t>(item) + 1) * stride <= bytes.size();
                 ++item) {
                const auto name =
                    read_u32(bytes, symbol_offset + item * stride, little).value_or(0);
                if (name < string_size) {
                    const auto value = fixed_string(bytes, string_offset + name, 160);
                    if (!value.empty()) {
                        symbols.push_back(value);
                    }
                }
            }
        }
        offset += *size;
    }
}

void inspect_pe(std::span<const std::byte> bytes,
                std::vector<std::string>& sections,
                std::vector<std::string>& symbols) {
    const auto pe_offset = read_u32(bytes, 0x3c, true);
    if (!pe_offset || *pe_offset + 24 > bytes.size() ||
        fixed_string(bytes, *pe_offset, 4) != "PE") {
        return;
    }
    const auto count = read_u16(bytes, *pe_offset + 6, true).value_or(0);
    const auto symbol_offset = read_u32(bytes, *pe_offset + 12, true).value_or(0);
    const auto symbol_count = read_u32(bytes, *pe_offset + 16, true).value_or(0);
    const auto optional_size = read_u16(bytes, *pe_offset + 20, true).value_or(0);
    auto section = static_cast<std::size_t>(*pe_offset + 24 + optional_size);
    for (auto index = std::uint16_t{}; index < count && section + 40 <= bytes.size(); ++index) {
        const auto name = fixed_string(bytes, section, 8);
        if (!name.empty()) {
            sections.push_back(name);
        }
        section += 40;
    }
    const auto string_table = symbol_offset + static_cast<std::uint64_t>(symbol_count) * 18U;
    for (auto index = std::uint32_t{};
         index < symbol_count && symbols.size() < 256 &&
         symbol_offset + (static_cast<std::uint64_t>(index) + 1) * 18U <= bytes.size();
         ++index) {
        const auto at = symbol_offset + static_cast<std::uint64_t>(index) * 18U;
        auto name = fixed_string(bytes, at, 8);
        if (read_u32(bytes, at, true).value_or(1) == 0) {
            const auto string_offset = read_u32(bytes, at + 4, true).value_or(0);
            name = fixed_string(bytes, string_table + string_offset, 160);
        }
        if (!name.empty()) {
            symbols.push_back(std::move(name));
        }
        index += std::to_integer<unsigned char>(bytes[at + 17]);
    }
}

auto known_version(std::span<const std::string> strings, std::string_view needle)
    -> std::optional<std::string> {
    for (const auto& value : strings) {
        const auto found = value.find(needle);
        if (found == std::string::npos) {
            continue;
        }
        const auto candidate = value.substr(found);
        const auto digit = std::ranges::find_if(candidate, [](char character) {
            return std::isdigit(static_cast<unsigned char>(character));
        });
        if (digit != candidate.end()) {
            const auto offset = static_cast<std::size_t>(digit - candidate.begin());
            auto version = value.substr(found + offset, 48);
            const auto end = version.find_first_not_of("0123456789.-_abcdefghijklmnopqrstuvwxyz");
            if (end != std::string::npos) {
                version.resize(end);
            }
            return version;
        }
    }
    return std::nullopt;
}

auto analyse_binary(const std::filesystem::path& artifact) -> result<sbom_model> {
    auto input = std::ifstream{artifact, std::ios::binary | std::ios::ate};
    if (!input) {
        return std::unexpected(error{errc::io, "cannot read foreign artifact", artifact.string()});
    }
    const auto end = input.tellg();
    if (end < 0) {
        return std::unexpected(error{errc::io, "cannot size foreign artifact", artifact.string()});
    }
    auto bytes = std::vector<std::byte>(static_cast<std::size_t>(end));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input && !bytes.empty()) {
        return std::unexpected(error{errc::io, "cannot read foreign artifact", artifact.string()});
    }
    auto subject = hash(algorithm::blake3, bytes);
    if (!subject) {
        return std::unexpected(subject.error());
    }
    auto format = std::string{"unknown"};
    auto sections = std::vector<std::string>{};
    auto symbols = std::vector<std::string>{};
    if (bytes.size() >= 4 && bytes[0] == std::byte{0x7f} && bytes[1] == std::byte{'E'} &&
        bytes[2] == std::byte{'L'} && bytes[3] == std::byte{'F'}) {
        format = "ELF";
        inspect_elf(bytes, sections, symbols);
    } else if (bytes.size() >= 4 && (read_u32(bytes, 0, true) == 0xfeedfaceU ||
                                     read_u32(bytes, 0, true) == 0xfeedfacfU ||
                                     read_u32(bytes, 0, true) == 0xcefaedfeU ||
                                     read_u32(bytes, 0, true) == 0xcffaedfeU)) {
        format = "Mach-O";
        inspect_macho(bytes, sections, symbols);
    } else if (bytes.size() >= 2 && bytes[0] == std::byte{'M'} && bytes[1] == std::byte{'Z'}) {
        format = "PE";
        inspect_pe(bytes, sections, symbols);
    }
    auto code = std::error_code{};
    const auto modified = std::filesystem::last_write_time(artifact, code);
    if (code) {
        return std::unexpected(error{errc::io, code.message(), artifact.string()});
    }
    const auto created = std::chrono::time_point_cast<std::chrono::seconds>(
        std::chrono::file_clock::to_sys(modified));
    auto model = sbom_model{.subject = *subject,
                            .name = artifact.filename().string(),
                            .created = created,
                            .derived = false,
                            .inferred = true};
    const auto root = add_component(model,
                                    {.name = artifact.filename().string(),
                                     .kind = "binary/" + format,
                                     .content = *subject,
                                     .inferred = true});
    const auto strings = printable_strings(bytes);
    struct signature_entry {
        std::string_view name;
        std::string_view needle;
    };
    constexpr auto signatures = std::array{signature_entry{"OpenSSL", "OpenSSL"},
                                           signature_entry{"zlib", "zlib"},
                                           signature_entry{"glibc", "GLIBC_"},
                                           signature_entry{"libstdc++", "GLIBCXX_"},
                                           signature_entry{"musl", "musl"},
                                           signature_entry{"libc++", "LIBCPP"}};
    for (const auto& signature : signatures) {
        if (!std::ranges::any_of(strings, [&](const auto& value) {
                return value.find(signature.needle) != std::string::npos;
            })) {
            continue;
        }
        auto name = std::string{signature.name};
        if (const auto version = known_version(strings, signature.needle)) {
            name += " " + *version;
        }
        const auto component =
            add_component(model, {.name = std::move(name), .kind = "library", .inferred = true});
        add_relationship(model, root, component, "contains");
    }
    for (const auto& section : sections) {
        model.evidence.push_back("section:" + section);
    }
    for (const auto& symbol : symbols) {
        model.evidence.push_back("symbol:" + symbol);
    }
    for (const auto& value : strings) {
        const auto has_version = std::ranges::any_of(value, [](char character) {
            return std::isdigit(static_cast<unsigned char>(character));
        });
        if (has_version && model.evidence.size() < 512 && value.size() <= 160) {
            model.evidence.push_back("version-string:" + value);
        }
    }
    return model;
}

auto model_for(repository& source,
               const objects::action_record& action,
               const action_result& value,
               const action_output& target,
               std::optional<digest> current_result) -> runtime::task<sbom_model> {
    auto cache = build_detail::disk_cache{source};
    auto opened = co_await cache.open();
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    auto builder = graph_builder{source, cache.entries()};
    co_return co_await builder.build(action, value, target, current_result);
}

auto report_for_model(repository& source,
                      const sbom_model& expected,
                      const action_output& target,
                      std::string_view output_path,
                      sbom_format format,
                      const std::vector<digest>& statement_ids) -> runtime::task<sbom_report> {
    auto expected_record = encode_record(expected, format);
    if (!expected_record) {
        co_return std::unexpected(expected_record.error());
    }
    for (const auto statement_id : statement_ids) {
        auto statement_bytes = co_await repository_detail::build_access::get(source, statement_id);
        if (!statement_bytes) {
            co_return std::unexpected(statement_bytes.error());
        }
        auto statement = objects::decode_attestation(*statement_bytes);
        if (!statement || statement->profile != "sbom/1" ||
            !same_digest(statement->subject, target.blob)) {
            continue;
        }
        const auto* statement_format = text_attribute(*statement, "format");
        const auto* statement_output = text_attribute(*statement, "output");
        if (!statement_format || *statement_format != format_name(format) || !statement_output ||
            *statement_output != output_path) {
            continue;
        }
        auto report = sbom_report{.subject = target.blob,
                                  .attestation = statement_id,
                                  .statement = *statement,
                                  .derived = true,
                                  .inferred = false,
                                  .components = expected.components,
                                  .relationships = expected.relationships};
        auto verified = verify_attestation(*statement);
        if (!verified) {
            co_return std::unexpected(verified.error());
        }
        if (!verified->verified) {
            report.reason = verified->reason;
            co_return report;
        }
        const auto* document = text_attribute(*statement, "document");
        const auto* derived = bool_attribute(*statement, "derived");
        const auto* inferred = bool_attribute(*statement, "inferred");
        const auto* components = list_attribute(*statement, "components");
        auto record_id = document ? parse_digest(*document)
                                  : liquid::result<digest>{std::unexpected(error{
                                        errc::corrupt, "SBOM attestation has no record", {}})};
        if (!record_id || !derived || !*derived || !inferred || *inferred || !components ||
            *components != component_claims(expected)) {
            report.reason = "SBOM attestation does not match the build graph";
            co_return report;
        }
        report.record = *record_id;
        auto record_bytes = co_await repository_detail::build_access::get(source, *record_id);
        if (!record_bytes) {
            co_return std::unexpected(record_bytes.error());
        }
        if (*record_bytes != expected_record->second) {
            report.reason = "SBOM record does not match the build graph";
            co_return report;
        }
        auto record = objects::decode_sbom(*record_bytes);
        if (!record) {
            co_return std::unexpected(record.error());
        }
        auto document_output =
            render_attached(expected, format, *record, *record_id, statement_id, *statement);
        if (!document_output) {
            co_return std::unexpected(document_output.error());
        }
        report.document = std::move(*document_output);
        report.verified = true;
        co_return report;
    }
    co_return std::unexpected(error{errc::not_found,
                                    "result has no SBOM for this artifact and format",
                                    std::string{output_path}});
}

auto index_sbom_statements(repository& source, const std::vector<digest>& statement_ids)
    -> runtime::task<std::optional<std::map<std::string, digest>>> {
    auto output = std::map<std::string, digest>{};
    for (const auto id : statement_ids) {
        auto bytes = co_await repository_detail::build_access::get(source, id);
        if (!bytes) {
            co_return std::nullopt;
        }
        auto statement = objects::decode_attestation(*bytes);
        if (!statement || statement->profile != "sbom/1") {
            co_return std::nullopt;
        }
        const auto* format = text_attribute(*statement, "format");
        const auto* path = text_attribute(*statement, "output");
        if (!format || !path || !output.emplace(*path + "\n" + *format, id).second) {
            co_return std::nullopt;
        }
    }
    co_return output;
}

auto load_action(repository& source, digest id) -> runtime::task<objects::action_record> {
    auto bytes = co_await repository_detail::build_access::get(source, id);
    if (!bytes) {
        co_return std::unexpected(bytes.error());
    }
    co_return objects::decode_action(*bytes);
}

auto load_result(repository& source, result_id id) -> runtime::task<action_result> {
    auto bytes = co_await repository_detail::build_access::get(source, id.value);
    if (!bytes) {
        co_return std::unexpected(bytes.error());
    }
    co_return objects::decode_result(*bytes);
}

auto sign_locally(attestation& statement) -> runtime::task<void> {
    auto signer = co_await identity_detail::local_signer();
    if (!signer) {
        co_return std::unexpected(signer.error());
    }
    statement.issuer = signer->identity();
    auto body = objects::encode_attestation_body(statement);
    if (!body) {
        co_return std::unexpected(body.error());
    }
    auto signed_value = co_await signer->sign("attestation", *body);
    if (!signed_value) {
        co_return std::unexpected(signed_value.error());
    }
    auto signature = identity_detail::canonical_signature(*signed_value);
    if (!signature) {
        co_return std::unexpected(signature.error());
    }
    statement.signature = std::move(*signature);
    co_return result<void>{};
}

auto report_for_inferred(sbom_model model, sbom_format format) -> runtime::task<sbom_report> {
    auto record = encode_record(model, format);
    if (!record) {
        co_return std::unexpected(record.error());
    }
    auto record_id = hash(algorithm::blake3, record->second);
    if (!record_id) {
        co_return std::unexpected(record_id.error());
    }
    auto statement = make_statement(model, format, *record_id, {});
    auto signed_statement = co_await sign_locally(statement);
    if (!signed_statement) {
        co_return std::unexpected(signed_statement.error());
    }
    auto statement_bytes = objects::encode(statement);
    if (!statement_bytes) {
        co_return std::unexpected(statement_bytes.error());
    }
    auto statement_id = hash(algorithm::blake3, *statement_bytes);
    if (!statement_id) {
        co_return std::unexpected(statement_id.error());
    }
    auto document =
        render_attached(model, format, record->first, *record_id, *statement_id, statement);
    if (!document) {
        co_return std::unexpected(document.error());
    }
    co_return sbom_report{.subject = model.subject,
                          .record = *record_id,
                          .attestation = *statement_id,
                          .statement = std::move(statement),
                          .document = std::move(*document),
                          .derived = false,
                          .inferred = true,
                          .verified = true,
                          .components = std::move(model.components),
                          .relationships = std::move(model.relationships)};
}

} // namespace

namespace build_detail {

runtime::task<void> store_derived_sboms(repository& source,
                                        const objects::action_record& action,
                                        action_result& value) {
    value.sboms.clear();
    if (value.outputs.empty()) {
        co_return result<void>{};
    }
    auto cache = build_detail::disk_cache{source};
    auto opened = co_await cache.open();
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    auto builder = graph_builder{source, cache.entries()};
    auto model = co_await builder.build(action, value, value.outputs.front(), std::nullopt);
    if (!model) {
        co_return std::unexpected(model.error());
    }
    for (const auto& output : value.outputs) {
        retarget_model(*model, output);
        for (const auto format : {sbom_format::spdx, sbom_format::cyclonedx}) {
            auto record = encode_record(*model, format);
            if (!record) {
                co_return std::unexpected(record.error());
            }
            auto record_id = co_await repository_detail::build_access::put_record(
                source, std::move(record->second));
            if (!record_id) {
                co_return std::unexpected(record_id.error());
            }
            auto statement = make_statement(*model, format, *record_id, value.producer);
            auto signed_statement =
                co_await repository_detail::build_access::sign_attestation(source, statement);
            if (!signed_statement) {
                co_return std::unexpected(signed_statement.error());
            }
            auto encoded = objects::encode(statement);
            if (!encoded) {
                co_return std::unexpected(encoded.error());
            }
            auto statement_id =
                co_await repository_detail::build_access::put_record(source, std::move(*encoded));
            if (!statement_id) {
                co_return std::unexpected(statement_id.error());
            }
            value.sboms.push_back(*statement_id);
        }
    }
    co_return result<void>{};
}

runtime::task<bool>
validate_derived_sboms(repository& source, result_id id, const action_result& value) {
    if (value.sboms.size() != value.outputs.size() * 2) {
        co_return false;
    }
    if (value.outputs.empty()) {
        co_return true;
    }
    auto action = co_await load_action(source, value.action);
    if (!action) {
        co_return std::unexpected(action.error());
    }
    auto cache = build_detail::disk_cache{source};
    auto opened = co_await cache.open();
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    auto builder = graph_builder{source, cache.entries()};
    auto model = co_await builder.build(*action, value, value.outputs.front(), id.value);
    if (!model) {
        co_return std::unexpected(model.error());
    }
    auto statements = co_await index_sbom_statements(source, value.sboms);
    if (!statements || !*statements) {
        co_return false;
    }
    for (const auto& output : value.outputs) {
        retarget_model(*model, output);
        for (const auto format : {sbom_format::spdx, sbom_format::cyclonedx}) {
            const auto key = output.path + "\n" + std::string{format_name(format)};
            const auto found = (*statements)->find(key);
            if (found == (*statements)->end()) {
                co_return false;
            }
            auto report = co_await report_for_model(
                source, *model, output, output.path, format, {found->second});
            if (!report || !report->verified) {
                co_return false;
            }
        }
    }
    co_return true;
}

} // namespace build_detail

namespace build {

runtime::task<sbom_report>
generate_sbom(repository& source, result_id id, std::string output_path, sbom_format format) {
    auto provenance = co_await verify_provenance(source, id);
    if (!provenance) {
        co_return std::unexpected(provenance.error());
    }
    if (!provenance->verified) {
        co_return std::unexpected(
            error{errc::unauthorized, "result provenance is invalid", provenance->reason});
    }
    const auto target =
        std::ranges::find(provenance->value.outputs, output_path, &action_output::path);
    if (target == provenance->value.outputs.end()) {
        co_return std::unexpected(
            error{errc::not_found, "result does not produce this path", output_path});
    }
    auto action = co_await load_action(source, provenance->value.action);
    if (!action) {
        co_return std::unexpected(action.error());
    }
    auto expected = co_await model_for(source, *action, provenance->value, *target, id.value);
    if (!expected) {
        co_return std::unexpected(expected.error());
    }
    co_return co_await report_for_model(
        source, *expected, *target, output_path, format, provenance->value.sboms);
}

runtime::task<sbom_report>
generate_sbom(repository& source, std::string output_path, sbom_format format) {
    auto content = std::optional<digest>{};
    auto entry = co_await repository_detail::vfs_access::entry(source, output_path);
    if (entry && entry->kind == node_kind::blob) {
        content = entry->object;
    } else if (!entry && entry.error().code != errc::not_found) {
        co_return std::unexpected(entry.error());
    }
    auto cache = build_detail::disk_cache{source};
    auto opened = co_await cache.open();
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    auto found = co_await cache.find_output(output_path, content);
    if (!found) {
        co_return std::unexpected(found.error());
    }
    if (!*found) {
        co_return std::unexpected(
            error{errc::not_found, "no build result produces this path", output_path});
    }
    co_return co_await generate_sbom(source, **found, std::move(output_path), format);
}

runtime::task<sbom_report> generate_wrapped_sbom(std::filesystem::path working_tree,
                                                 std::filesystem::path output_path,
                                                 sbom_format format) {
    auto code = std::error_code{};
    working_tree = std::filesystem::canonical(working_tree, code);
    if (code) {
        co_return std::unexpected(error{errc::io, code.message(), working_tree.string()});
    }
    const auto absolute = output_path.is_absolute() ? output_path : working_tree / output_path;
    const auto normalized = std::filesystem::weakly_canonical(absolute, code);
    if (code) {
        co_return std::unexpected(error{errc::io, code.message(), absolute.string()});
    }
    const auto relative = normalized.lexically_relative(working_tree);
    const auto text = relative.generic_string();
    if (relative.empty() || relative.is_absolute() || text == ".." || text.starts_with("../")) {
        co_return std::unexpected(error{errc::invalid_argument,
                                        "SBOM path is outside the wrapped project",
                                        normalized.string()});
    }
    auto cache_directory = wrap_cache_directory(working_tree);
    if (!cache_directory) {
        co_return std::unexpected(cache_directory.error());
    }
    auto source = co_await repository::open(*cache_directory / "repository");
    if (!source) {
        co_return std::unexpected(source.error());
    }
    co_return co_await generate_sbom(*source, text, format);
}

runtime::task<sbom_report> infer_sbom(std::filesystem::path artifact, sbom_format format) {
    auto code = std::error_code{};
    artifact = std::filesystem::canonical(artifact, code);
    if (code || !std::filesystem::is_regular_file(artifact)) {
        co_return std::unexpected(
            error{errc::not_found, "foreign artifact does not exist", artifact.string()});
    }
    auto analysed = co_await runtime::offload([artifact] { return analyse_binary(artifact); });
    if (!analysed) {
        co_return std::unexpected(analysed.error());
    }
    if (!*analysed) {
        co_return std::unexpected(analysed->error());
    }
    co_return co_await report_for_inferred(std::move(**analysed), format);
}

} // namespace build
} // namespace liquid
