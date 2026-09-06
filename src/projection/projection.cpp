#include "projection/projection.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <map>
#include <ranges>

#include "build/relocation.hpp"
#include "build/repository_access.hpp"
#include "liquid/abi.hpp"
#include "liquid/version.hpp"
#include "objects/records.hpp"

namespace liquid::projection_detail {
namespace {

auto failure(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto same_digest(const digest& left, const digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

auto collect_tree(repository& source,
                  digest root,
                  std::string prefix,
                  std::string component,
                  bool replace,
                  std::map<std::string, formats::package::file>& files,
                  std::map<std::string, std::uint32_t>& directories,
                  std::map<std::string, std::string>& components) -> runtime::task<void> {
    auto tree = co_await source.tree(root);
    if (!tree) {
        co_return std::unexpected(tree.error());
    }
    for (const auto& entry : tree->entries) {
        const auto path = prefix.empty() ? entry.name : prefix + "/" + entry.name;
        if (entry.kind == node_kind::tree) {
            if (replace) {
                files.erase(path);
                components.erase(path);
                auto ancestor = std::filesystem::path{path}.parent_path();
                while (!ancestor.empty()) {
                    files.erase(ancestor.generic_string());
                    components.erase(ancestor.generic_string());
                    ancestor = ancestor.parent_path();
                }
            }
            directories.insert_or_assign(path, entry.mode);
            auto nested = co_await collect_tree(
                source, entry.ref, path, component, replace, files, directories, components);
            if (!nested) {
                co_return std::unexpected(nested.error());
            }
            continue;
        }
        if (entry.kind != node_kind::blob) {
            co_return std::unexpected(
                failure(errc::unsupported, "projection source contains a repository entry", path));
        }
        auto bytes = co_await source.blob(entry.ref);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        auto value = formats::package::file{path, std::move(*bytes), entry.mode};
        const auto found = files.find(path);
        if (!replace && found != files.end() &&
            (found->second.mode != value.mode || found->second.bytes != value.bytes)) {
            co_return std::unexpected(
                failure(errc::conflict, "projection sources claim different file bytes", path));
        }
        if (replace) {
            const auto descendant = path + "/";
            std::erase_if(files,
                          [&](const auto& item) { return item.first.starts_with(descendant); });
            std::erase_if(directories,
                          [&](const auto& item) { return item.first.starts_with(descendant); });
            std::erase_if(components,
                          [&](const auto& item) { return item.first.starts_with(descendant); });
            directories.erase(path);
        }
        files.insert_or_assign(path, std::move(value));
        components.insert_or_assign(path, component);
    }
    co_return result<void>{};
}

auto add_generated_file(std::map<std::string, formats::package::file>& files,
                        std::string path,
                        std::vector<std::byte> bytes,
                        std::uint32_t mode) -> result<void> {
    auto value = formats::package::file{std::move(path), std::move(bytes), mode};
    const auto found = files.find(value.path);
    if (found != files.end() &&
        (found->second.mode != value.mode || found->second.bytes != value.bytes)) {
        return std::unexpected(failure(
            errc::conflict, "generated install intent claims an existing path", value.path));
    }
    files.insert_or_assign(value.path, std::move(value));
    return {};
}

auto systemd_unit(const install_service& service) -> std::string {
    auto output = std::string{"[Unit]\nDescription="} + service.name + "\n";
    if (!service.after.empty()) {
        output += "After=";
        for (std::size_t index = 0; index < service.after.size(); ++index) {
            if (index != 0) {
                output.push_back(' ');
            }
            output += service.after[index] + ".service";
        }
        output.push_back('\n');
    }
    output += "\n[Service]\nType=simple\nUser=" + service.user +
              "\nExecStart=" + service.executable.generic_string() +
              "\nRestart=" + std::string{to_string(service.restart)} +
              "\n\n[Install]\nWantedBy=multi-user.target\n";
    return output;
}

auto add_install_files(const package_metadata& metadata,
                       const install_intent& intent,
                       std::map<std::string, formats::package::file>& files) -> result<void> {
    auto encoded = encode_install_intent(intent);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    auto manifest = add_generated_file(
        files, "share/liquid/intents/" + metadata.name + ".cbor", std::move(*encoded), 0644);
    if (!manifest) {
        return manifest;
    }
    for (const auto& service : intent.services) {
        auto text = systemd_unit(service);
        auto added =
            add_generated_file(files,
                               "lib/systemd/system/" + service.name + ".service",
                               {reinterpret_cast<const std::byte*>(text.data()),
                                reinterpret_cast<const std::byte*>(text.data() + text.size())},
                               0644);
        if (!added) {
            return added;
        }
    }
    return {};
}

auto text_attribute(const attestation& statement, std::string_view name) -> const std::string* {
    const auto found = std::ranges::find(statement.attributes, name, &attestation_attribute::name);
    return found == statement.attributes.end() ? nullptr : std::get_if<std::string>(&found->value);
}

auto check_abi(repository& source, const action_result& result, bool allow) -> runtime::task<bool> {
    auto allowed = false;
    for (const auto& id : result.abi_compatibility) {
        auto page = co_await source.attestations({.id = id});
        if (!page) {
            co_return std::unexpected(page.error());
        }
        if (page->records.empty()) {
            co_return std::unexpected(
                failure(errc::corrupt, "ABI compatibility attestation is absent", to_string(id)));
        }
        const auto& statement = page->records.front().value;
        auto verified = verify_attestation(statement);
        if (!verified || !verified->verified) {
            co_return std::unexpected(
                failure(errc::unauthorized,
                        "ABI compatibility attestation is invalid",
                        verified ? verified->reason : verified.error().message));
        }
        const auto* verdict = text_attribute(statement, "compatibility");
        if (!verdict || *verdict != "incompatible") {
            continue;
        }
        if (!allow) {
            co_return std::unexpected(failure(errc::conflict,
                                              "projection refused an incompatible library ABI",
                                              to_string(statement.subject)));
        }
        allowed = true;
    }
    co_return allowed;
}

auto inspect_dependencies(std::span<const formats::package::file> files)
    -> std::pair<std::vector<std::string>, std::vector<std::string>> {
    auto needed = std::vector<std::string>{};
    auto sonames = std::vector<std::string>{};
    for (const auto& file : files) {
        auto descriptor = extract_abi(file.bytes);
        if (!descriptor) {
            continue;
        }
        needed.insert(
            needed.end(), descriptor->needed_libraries.begin(), descriptor->needed_libraries.end());
        if (descriptor->install_name) {
            sonames.push_back(*descriptor->install_name);
        }
    }
    std::ranges::sort(needed);
    needed.erase(std::unique(needed.begin(), needed.end()), needed.end());
    std::ranges::sort(sonames);
    sonames.erase(std::unique(sonames.begin(), sonames.end()), sonames.end());
    return {std::move(needed), std::move(sonames)};
}

auto canonical_architecture(std::string_view value) -> std::string_view {
    if (value == "amd64") {
        return "x86_64";
    }
    if (value == "arm64") {
        return "aarch64";
    }
    return value;
}

auto unique_digests(std::vector<digest> values) -> std::vector<digest> {
    std::ranges::sort(values, [](const digest& left, const digest& right) {
        return to_string(left) < to_string(right);
    });
    values.erase(std::unique(values.begin(), values.end(), same_digest), values.end());
    return values;
}

auto script_values(const prepared_projection& prepared, projection_target target)
    -> std::vector<std::pair<std::string, std::string>> {
    if (!prepared.scripts) {
        return {};
    }
    auto names = std::array<std::string_view, 4>{};
    if (target == projection_target::deb) {
        names = {"preinst", "postinst", "prerm", "postrm"};
    } else if (target == projection_target::rpm) {
        names = {"%pre", "%post", "%preun", "%postun"};
    } else {
        names = {".pre-install", ".post-install", ".pre-deinstall", ".post-deinstall"};
    }
    const auto values = std::array<std::string_view, 4>{prepared.scripts->pre_install,
                                                        prepared.scripts->post_install,
                                                        prepared.scripts->pre_remove,
                                                        prepared.scripts->post_remove};
    auto output = std::vector<std::pair<std::string, std::string>>{};
    for (std::size_t index = 0; index < values.size(); ++index) {
        auto bytes = std::span{reinterpret_cast<const std::byte*>(values[index].data()),
                               values[index].size()};
        auto content = formats::package::sha256(bytes);
        if (content) {
            output.emplace_back(names[index], to_string(*content));
        }
    }
    return output;
}

auto script_claims(const prepared_projection& prepared, projection_target target)
    -> std::vector<std::string> {
    auto output = std::vector<std::string>{};
    for (const auto& [name, content] : script_values(prepared, target)) {
        output.push_back(name + "=" + content);
    }
    return output;
}

auto json_quote(std::string_view value) -> std::string {
    auto output = std::string{"\""};
    for (const auto item : value) {
        if (item == '\\' || item == '"') {
            output.push_back('\\');
        }
        output.push_back(item);
    }
    output.push_back('"');
    return output;
}

auto metadata_is_safe(const package_metadata& metadata) -> bool {
    const auto safe = [](std::string_view value) {
        return value.find_first_of("\r\n") == std::string_view::npos &&
               value.find('\0') == std::string_view::npos;
    };
    if (!safe(metadata.name) || !safe(metadata.version) || !safe(metadata.release) ||
        !safe(metadata.architecture) || !safe(metadata.description) || !safe(metadata.maintainer) ||
        !safe(metadata.licence) || !safe(metadata.homepage) || !safe(metadata.identifier) ||
        !safe(metadata.publisher) || !safe(metadata.display_name) || !safe(metadata.executable)) {
        return false;
    }
    for (const auto* values : {&metadata.declared_dependencies,
                               &metadata.provides,
                               &metadata.conflicts,
                               &metadata.replaces}) {
        if (!std::ranges::all_of(*values, safe)) {
            return false;
        }
    }
    return true;
}

auto component_is_safe(std::string_view value) -> bool {
    return !value.empty() && value != "." && value != ".." &&
           value.find_first_of("/\\") == std::string_view::npos;
}

auto relative_path_is_safe(const std::filesystem::path& path) -> bool {
    return !path.is_absolute() && std::ranges::none_of(path, [](const auto& component) {
        return component == "." || component == "..";
    });
}

auto options_are_safe(const projection_options& options) -> bool {
    const auto safe = [](std::string_view value) {
        return value.find_first_of("\r\n") == std::string_view::npos &&
               value.find('\0') == std::string_view::npos;
    };
    for (const auto* values : {&options.pre_depends,
                               &options.recommends,
                               &options.breaks,
                               &options.conffiles,
                               &options.oci_entrypoint,
                               &options.oci_command}) {
        if (!std::ranges::all_of(*values, safe)) {
            return false;
        }
    }
    if (options.entitlements.string().find_first_of("\r\n\0") != std::string::npos ||
        !safe(options.installer_url)) {
        return false;
    }
    return std::ranges::all_of(options.conffiles, [](const auto& path) {
        const auto value = std::filesystem::path{path};
        return !value.empty() && !value.is_absolute() &&
               std::ranges::none_of(value, [](const auto& component) { return component == ".."; });
    });
}

auto trace_layers(repository& source, result_id selected, digest target)
    -> runtime::task<std::map<std::string, std::uint8_t>> {
    auto options = record_page_options{};
    while (true) {
        auto page = co_await source.traces(options);
        if (!page) {
            co_return std::unexpected(page.error());
        }
        for (const auto& trace : page->records) {
            if (!std::ranges::contains(
                    trace.actions, to_string(selected.value), [](const auto& action) {
                        return to_string(action.result.value);
                    })) {
                continue;
            }
            co_return trace_output_layers(trace, target);
        }
        if (page->done || !page->next) {
            break;
        }
        options.cursor = page->next;
    }
    co_return std::map<std::string, std::uint8_t>{};
}

} // namespace

runtime::task<prepared_projection> prepare(repository& source, const projection_request& request) {
    if (request.sources.empty()) {
        co_return std::unexpected(failure(errc::invalid_argument, "projection has no source"));
    }
    const auto os_target = request.target == projection_target::ostree_commit ||
                           request.target == projection_target::bootc_image;
    if (!os_target && (request.metadata.name.empty() || request.metadata.version.empty())) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "projection requires package name and version"));
    }
    if (!metadata_is_safe(request.metadata)) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "projection metadata contains a control character"));
    }
    if (!options_are_safe(request.options)) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "projection options contain an invalid value"));
    }
    auto output = prepared_projection{.metadata = request.metadata, .options = request.options};
    if (os_target && output.metadata.name.empty()) {
        output.metadata.name = "liquid-os";
    }
    if (os_target && output.metadata.version.empty()) {
        output.metadata.version = std::to_string(output.options.source_date_epoch);
    }
    if (!component_is_safe(output.metadata.name) || !component_is_safe(output.metadata.version) ||
        !component_is_safe(output.metadata.release)) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "package identity is not a safe path component"));
    }
    if (os_target && output.options.ostree_ref.empty()) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "ostree projection requires a ref", "--ref"));
    }
    auto files = std::map<std::string, formats::package::file>{};
    for (auto source_index = std::size_t{}; source_index < request.sources.size(); ++source_index) {
        const auto& source_value = request.sources[source_index];
        if (!relative_path_is_safe(source_value.path)) {
            co_return std::unexpected(failure(errc::invalid_argument,
                                              "projection source path is invalid",
                                              source_value.path.string()));
        }
        if (os_target && source_index == 0 && !source_value.path.empty()) {
            co_return std::unexpected(failure(errc::invalid_argument,
                                              "ostree base source must be mounted at the root",
                                              source_value.path.string()));
        }
        auto tree = digest{};
        if (const auto* result_id = std::get_if<liquid::result_id>(&source_value.value)) {
            auto result = co_await build::result(source, *result_id);
            if (!result) {
                co_return std::unexpected(result.error());
            }
            if (!result->output_tree) {
                co_return std::unexpected(failure(errc::unsupported,
                                                  "source result has no install tree",
                                                  to_string(result_id->value)));
            }
            auto abi = co_await check_abi(source, *result, request.options.allow_abi_break);
            if (!abi) {
                co_return std::unexpected(abi.error());
            }
            output.abi_break_allowed = output.abi_break_allowed || *abi;
            auto action = co_await source.action(result->action);
            if (!action) {
                co_return std::unexpected(action.error());
            }
            if (output.metadata.architecture.empty()) {
                output.metadata.architecture = action->platform.arch;
            } else if (!action->platform.arch.empty() &&
                       canonical_architecture(output.metadata.architecture) !=
                           canonical_architecture(action->platform.arch)) {
                co_return std::unexpected(
                    failure(errc::conflict,
                            "metadata architecture differs from the action platform",
                            action->platform.arch));
            } else if (!action->platform.arch.empty()) {
                output.metadata.architecture = action->platform.arch;
            }
            if (output.options.source_date_epoch == 0 && result->finished_at) {
                output.options.source_date_epoch =
                    static_cast<std::uint64_t>(result->finished_at->time_since_epoch().count());
            }
            auto layers = co_await trace_layers(source, *result_id, result->action);
            if (!layers) {
                co_return std::unexpected(layers.error());
            }
            for (auto& [path, layer] : *layers) {
                output.stability_layers.insert_or_assign(std::move(path), layer);
            }
            tree = *result->output_tree;
            output.source_results.push_back(*result_id);
            if (result->provenance) {
                output.source_attestations.push_back(*result->provenance);
            }
            output.source_attestations.insert(
                output.source_attestations.end(), result->sboms.begin(), result->sboms.end());
        } else {
            tree = std::get<digest>(source_value.value);
        }
        const auto source_id =
            os_target ? to_string(tree)
            : std::holds_alternative<liquid::result_id>(source_value.value)
                ? to_string(std::get<liquid::result_id>(source_value.value).value)
                : to_string(tree);
        output.source_ids.push_back(source_id);
        if (source_index == 0) {
            output.base_source = to_string(tree);
        } else {
            output.overlay_sources.push_back(to_string(tree));
        }
        const auto prefix = source_value.path.generic_string();
        const auto component =
            source_index == 0 ? std::string{"base"} : "overlay-" + std::to_string(source_index);
        if (!prefix.empty()) {
            if (os_target && source_index != 0) {
                auto ancestor = std::filesystem::path{prefix};
                while (!ancestor.empty()) {
                    files.erase(ancestor.generic_string());
                    output.components.erase(ancestor.generic_string());
                    ancestor = ancestor.parent_path();
                }
            }
            output.directory_modes.try_emplace(prefix, 0040755U);
        }
        auto collected = co_await collect_tree(source,
                                               tree,
                                               prefix,
                                               component,
                                               os_target && source_index != 0,
                                               files,
                                               output.directory_modes,
                                               output.components);
        if (!collected) {
            co_return std::unexpected(collected.error());
        }
    }
    if (request.install) {
        auto valid = validate_install_intent(*request.install);
        if (!valid) {
            co_return std::unexpected(valid.error());
        }
        auto added = add_install_files(output.metadata, *request.install, files);
        if (!added) {
            co_return std::unexpected(added.error());
        }
        output.install = request.install;
        if (request.target == projection_target::deb || request.target == projection_target::rpm ||
            request.target == projection_target::apk) {
            auto scripts = generate_maintainer_scripts(request.target, *request.install);
            if (!scripts) {
                co_return std::unexpected(scripts.error());
            }
            output.scripts = std::move(*scripts);
        }
    }
    if (output.metadata.architecture.empty() &&
        request.target != projection_target::ostree_commit) {
        co_return std::unexpected(failure(
            errc::invalid_argument, "tree-digest projection requires an architecture", "--arch"));
    }
    if (!output.metadata.architecture.empty() && !component_is_safe(output.metadata.architecture)) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "package architecture is not a safe path component"));
    }
    if (output.metadata.description.empty()) {
        output.metadata.description = output.metadata.name;
    }
    if (output.metadata.maintainer.empty()) {
        output.metadata.maintainer = "unknown";
    }
    if (output.metadata.licence.empty()) {
        output.metadata.licence = "NOASSERTION";
    }
    for (auto& [path, file] : files) {
        output.files.push_back(std::move(file));
    }
    if (os_target) {
        output.source_attestations = unique_digests(std::move(output.source_attestations));
        co_return output;
    }
    auto relocation_files = std::vector<build_detail::relocation_file>{};
    relocation_files.reserve(output.files.size());
    for (const auto& file : output.files) {
        relocation_files.push_back({file.path, file.bytes});
    }
    auto relocation = co_await runtime::offload([files = std::move(relocation_files)] {
        return build_detail::inspect_relocation(files, std::nullopt);
    });
    if (!relocation) {
        co_return std::unexpected(relocation.error());
    }
    if (!*relocation) {
        co_return std::unexpected(relocation->error());
    }
    if (!(**relocation).relocatable) {
        co_return std::unexpected(build_detail::relocation_failure(**relocation));
    }
    auto inspected =
        co_await runtime::offload([files = output.files] { return inspect_dependencies(files); });
    if (!inspected) {
        co_return std::unexpected(inspected.error());
    }
    output.needed = std::move(inspected->first);
    output.sonames = std::move(inspected->second);
    output.source_attestations = unique_digests(std::move(output.source_attestations));
    co_return output;
}

runtime::task<digest> store_projection_attestation(repository& source,
                                                   const prepared_projection& prepared,
                                                   projection_target target,
                                                   digest subject) {
    auto statement = attestation{
        .subject = subject,
        .profile = "projection/1",
        .at = std::chrono::sys_seconds{std::chrono::seconds{prepared.options.source_date_epoch}},
        .subject_kind = attestation_subject_kind::artifact,
        .attributes = {{"target", std::string{to_string(target)}},
                       {"source_result", prepared.source_ids},
                       {"normalisation", std::string{"epoch,uid=0,gid=0,modes,sorted"}},
                       {"tool", "liquid/" + std::string{version_string()}}}};
    const auto os_target =
        target == projection_target::ostree_commit || target == projection_target::bootc_image;
    if (os_target && !prepared.base_source.empty()) {
        statement.attributes.push_back({"base_tree", prepared.base_source});
    }
    if (os_target && !prepared.overlay_sources.empty()) {
        statement.attributes.push_back({"overlay_tree", prepared.overlay_sources});
    }
    const auto scripts = script_claims(prepared, target);
    if (!scripts.empty()) {
        statement.attributes.push_back({"scripts", scripts});
    }
    auto signed_statement =
        co_await repository_detail::build_access::sign_attestation(source, statement);
    if (!signed_statement) {
        co_return std::unexpected(signed_statement.error());
    }
    co_return co_await source.store_attestation(std::move(statement));
}

runtime::task<digest> store_script_sbom(repository& source,
                                        const prepared_projection& prepared,
                                        projection_target target,
                                        digest subject,
                                        std::string output_name) {
    const auto scripts = script_values(prepared, target);
    if (scripts.empty()) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "projection has no generated scripts", output_name));
    }
    auto files = std::string{};
    auto relationships = std::string{};
    auto claims = std::vector<std::string>{};
    for (std::size_t index = 0; index < scripts.size(); ++index) {
        if (index != 0) {
            files.push_back(',');
            relationships.push_back(',');
        }
        const auto id = "SPDXRef-Script-" + std::to_string(index + 1);
        const auto digest = scripts[index].second.substr(std::string_view{"sha256:"}.size());
        files +=
            "{\"SPDXID\":" + json_quote(id) + ",\"fileName\":" + json_quote(scripts[index].first) +
            ",\"checksums\":[{\"algorithm\":\"SHA256\",\"checksumValue\":" + json_quote(digest) +
            "}]}";
        relationships += "{\"spdxElementId\":\"SPDXRef-Package\",\"relationshipType\":"
                         "\"CONTAINS\",\"relatedSpdxElement\":" +
                         json_quote(id) + "}";
        claims.push_back(scripts[index].first + "=" + scripts[index].second);
    }
    auto document =
        std::string{"{\"spdxVersion\":\"SPDX-2.3\",\"dataLicense\":\"CC0-1.0\","
                    "\"SPDXID\":\"SPDXRef-DOCUMENT\",\"name\":"} +
        json_quote(output_name + " generated maintainer scripts") +
        ",\"documentNamespace\":" + json_quote("https://liquid.dev/sbom/" + to_string(subject)) +
        ",\"creationInfo\":{\"created\":\"1970-01-01T00:00:00Z\",\"creators\":["
        "\"Tool: liquid\"]},\"packages\":[{\"SPDXID\":\"SPDXRef-Package\",\"name\":" +
        json_quote(output_name) +
        ",\"downloadLocation\":\"NOASSERTION\",\"filesAnalyzed\":true}],\"files\":[" + files +
        "],\"relationships\":[" + relationships + "]}";
    auto record = objects::sbom_record{
        "SPDX-2.3",
        {reinterpret_cast<const std::byte*>(document.data()),
         reinterpret_cast<const std::byte*>(document.data() + document.size())}};
    auto encoded = objects::encode(record);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    auto record_id =
        co_await repository_detail::build_access::put_record(source, std::move(*encoded));
    if (!record_id) {
        co_return std::unexpected(record_id.error());
    }
    auto statement = attestation{
        .subject = subject,
        .profile = "sbom/1",
        .at = std::chrono::sys_seconds{std::chrono::seconds{prepared.options.source_date_epoch}},
        .subject_kind = attestation_subject_kind::artifact,
        .attributes = {{"format", std::string{"SPDX-2.3"}},
                       {"output", output_name},
                       {"document", to_string(*record_id)},
                       {"derived", true},
                       {"inferred", false},
                       {"components", claims}}};
    auto signed_statement =
        co_await repository_detail::build_access::sign_attestation(source, statement);
    if (!signed_statement) {
        co_return std::unexpected(signed_statement.error());
    }
    co_return co_await source.store_attestation(std::move(statement));
}

result<void> write_file(std::filesystem::path path, std::span<const std::byte> bytes) {
    auto code = std::error_code{};
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), code);
    }
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), path.parent_path().string()));
    }
    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
        return std::unexpected(failure(errc::io, "cannot write projected artifact", path.string()));
    }
    return {};
}

} // namespace liquid::projection_detail

namespace liquid {

namespace {

auto project_prepared(repository& source,
                      projection_target target,
                      projection_detail::prepared_projection prepared,
                      std::span<const projection_service> services)
    -> runtime::task<projection_report> {
    switch (target) {
    case projection_target::deb:
    case projection_target::rpm:
    case projection_target::apk:
        co_return co_await projection_detail::project_package(source, target, std::move(prepared));
    case projection_target::oci_image:
        co_return co_await projection_detail::project_oci(source, std::move(prepared));
    case projection_target::ostree_commit:
        co_return co_await projection_detail::project_ostree(source, std::move(prepared));
    case projection_target::bootc_image:
        co_return co_await projection_detail::project_bootc(source, std::move(prepared));
    case projection_target::helm_chart:
        co_return co_await projection_detail::project_helm(source, std::move(prepared), services);
    case projection_target::homebrew:
        co_return co_await projection_detail::project_homebrew(source, std::move(prepared));
    case projection_target::pkgbuild:
        co_return co_await projection_detail::project_pkgbuild(source, std::move(prepared));
    case projection_target::appimage:
        co_return co_await projection_detail::project_appimage(source, std::move(prepared));
    case projection_target::flatpak:
        co_return co_await projection_detail::project_flatpak(source, std::move(prepared));
    case projection_target::snap:
        co_return co_await projection_detail::project_snap(source, std::move(prepared));
    }
    co_return std::unexpected(error{errc::unsupported, "unknown projection target", "project"});
}

} // namespace

std::string_view to_string(projection_target target) noexcept {
    switch (target) {
    case projection_target::oci_image:
        return "oci";
    case projection_target::ostree_commit:
        return "ostree";
    case projection_target::bootc_image:
        return "bootc";
    case projection_target::deb:
        return "deb";
    case projection_target::rpm:
        return "rpm";
    case projection_target::apk:
        return "apk";
    case projection_target::helm_chart:
        return "helm";
    case projection_target::macos_pkg:
        return "macos-pkg";
    case projection_target::macos_dmg:
        return "macos-dmg";
    case projection_target::msix:
        return "msix";
    case projection_target::winget_manifest:
        return "winget";
    case projection_target::homebrew:
        return "homebrew";
    case projection_target::pkgbuild:
        return "pkgbuild";
    case projection_target::appimage:
        return "appimage";
    case projection_target::flatpak:
        return "flatpak";
    case projection_target::snap:
        return "snap";
    }
    return "oci";
}

std::string_view to_string(ostree_repository_mode mode) noexcept {
    switch (mode) {
    case ostree_repository_mode::archive_z2:
        return "archive-z2";
    case ostree_repository_mode::bare_user:
        return "bare-user";
    }
    return "archive-z2";
}

runtime::task<projection_report> project(repository& source, projection_request request) {
    if (request.target == projection_target::winget_manifest) {
        co_return co_await projection_detail::project_winget(source, request);
    }
    auto prepared = co_await projection_detail::prepare(source, request);
    if (!prepared) {
        co_return std::unexpected(prepared.error());
    }
    if (request.options.against) {
        auto conflicts = co_await projection_detail::resolve_projection_conflicts(
            *prepared, request.target, *request.options.against);
        if (!conflicts) {
            co_return std::unexpected(conflicts.error());
        }
    }
    switch (request.target) {
    case projection_target::deb:
    case projection_target::rpm:
    case projection_target::apk:
        co_return co_await projection_detail::project_package(
            source, request.target, std::move(*prepared));
    case projection_target::oci_image:
        co_return co_await projection_detail::project_oci(source, std::move(*prepared));
    case projection_target::ostree_commit:
        co_return co_await projection_detail::project_ostree(source, std::move(*prepared));
    case projection_target::bootc_image:
        co_return co_await projection_detail::project_bootc(source, std::move(*prepared));
    case projection_target::helm_chart:
        co_return co_await projection_detail::project_helm(
            source, std::move(*prepared), request.services);
    case projection_target::macos_pkg:
    case projection_target::macos_dmg:
        co_return co_await projection_detail::project_macos(
            source, request.target, std::move(*prepared));
    case projection_target::msix:
        co_return co_await projection_detail::project_msix(source, std::move(*prepared));
    case projection_target::winget_manifest:
        break;
    case projection_target::homebrew:
        co_return co_await projection_detail::project_homebrew(source, std::move(*prepared));
    case projection_target::pkgbuild:
        co_return co_await projection_detail::project_pkgbuild(source, std::move(*prepared));
    case projection_target::appimage:
        co_return co_await projection_detail::project_appimage(source, std::move(*prepared));
    case projection_target::flatpak:
        co_return co_await projection_detail::project_flatpak(source, std::move(*prepared));
    case projection_target::snap:
        co_return co_await projection_detail::project_snap(source, std::move(*prepared));
    }
    co_return co_await project_prepared(
        source, request.target, std::move(*prepared), request.services);
}

runtime::task<std::vector<projection_report>> project(repository& source,
                                                      std::vector<projection_request> requests) {
    if (requests.empty()) {
        co_return std::unexpected(
            error{errc::invalid_argument, "projection request set is empty", "project"});
    }
    auto prepared = std::vector<projection_detail::prepared_projection>{};
    auto targets = std::vector<projection_target>{};
    prepared.reserve(requests.size());
    targets.reserve(requests.size());
    for (const auto& request : requests) {
        if (request.options.against) {
            co_return std::unexpected(error{errc::invalid_argument,
                                            "batch projection does not accept --against",
                                            request.metadata.name});
        }
        auto value = co_await projection_detail::prepare(source, request);
        if (!value) {
            co_return std::unexpected(value.error());
        }
        prepared.push_back(std::move(*value));
        targets.push_back(request.target);
    }
    auto conflicts = projection_detail::resolve_projection_conflicts(prepared, targets);
    if (!conflicts) {
        co_return std::unexpected(conflicts.error());
    }
    auto reports = std::vector<projection_report>{};
    reports.reserve(requests.size());
    for (std::size_t index = 0; index < requests.size(); ++index) {
        auto report = co_await project_prepared(
            source, requests[index].target, std::move(prepared[index]), requests[index].services);
        if (!report) {
            co_return std::unexpected(report.error());
        }
        reports.push_back(std::move(*report));
    }
    co_return reports;
}

namespace {

auto quote(std::string_view value) -> std::string {
    auto output = std::string{"\""};
    for (const auto character : value) {
        switch (character) {
        case '\\':
        case '\"':
            output.push_back('\\');
            output.push_back(character);
            break;
        case '\b':
            output += "\\b";
            break;
        case '\f':
            output += "\\f";
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
            if (static_cast<unsigned char>(character) < 0x20) {
                constexpr auto digits = std::string_view{"0123456789abcdef"};
                const auto byte = static_cast<unsigned char>(character);
                output += "\\u00";
                output.push_back(digits[byte >> 4U]);
                output.push_back(digits[byte & 0x0fU]);
            } else {
                output.push_back(character);
            }
        }
    }
    output.push_back('\"');
    return output;
}

} // namespace

result<std::string> projection_report_json(const projection_report& report) {
    auto output = std::string{"{\"artifacts\":["};
    for (auto index = std::size_t{}; index < report.artifacts.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        const auto& artifact = report.artifacts[index];
        output += "{\"path\":" + quote(artifact.path.generic_string()) +
                  ",\"digest\":" + quote(to_string(artifact.digest)) +
                  ",\"media_type\":" + quote(artifact.media_type) +
                  ",\"reproducible\":" + (artifact.reproducible ? "true" : "false") + "}";
    }
    output += "],\"signing_requests\":[";
    for (auto index = std::size_t{}; index < report.signing_requests.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        const auto& request = report.signing_requests[index];
        output += "{\"path\":" + quote(request.path.generic_string()) +
                  ",\"artifact\":" + quote(request.artifact.generic_string()) +
                  ",\"unsigned_digest\":" + quote(to_string(request.unsigned_digest)) +
                  ",\"platform\":" + quote(to_string(request.platform)) + "}";
    }
    output += "],\"attestations\":[";
    for (auto index = std::size_t{}; index < report.attestations.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        output += quote(to_string(report.attestations[index]));
    }
    output += "],\"pinned\":[";
    for (auto index = std::size_t{}; index < report.pinned.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        output += "{\"name\":" + quote(report.pinned[index].name) +
                  ",\"digest\":" + quote(to_string(report.pinned[index].digest)) + "}";
    }
    output += "],\"dependencies\":[";
    for (auto index = std::size_t{}; index < report.dependencies.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        output += "{\"value\":" + quote(report.dependencies[index].value) +
                  ",\"declared\":" + (report.dependencies[index].declared ? "true" : "false") + "}";
    }
    output += "],\"replacements\":[";
    for (auto index = std::size_t{}; index < report.replacements.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        output += "{\"package\":" + quote(report.replacements[index].package) + ",\"paths\":[";
        for (auto path = std::size_t{}; path < report.replacements[index].paths.size(); ++path) {
            if (path != 0) {
                output.push_back(',');
            }
            output += quote(report.replacements[index].paths[path]);
        }
        output += "]}";
    }
    output += "],\"abi_break_allowed\":";
    output += report.abi_break_allowed ? "true" : "false";
    output += ",\"ostree_commit\":";
    output += report.ostree_commit ? quote(to_string(*report.ostree_commit)) : "null";
    output.push_back('}');
    return output;
}

std::string_view to_string(platform_signing_system platform) noexcept {
    switch (platform) {
    case platform_signing_system::macos:
        return "macos";
    case platform_signing_system::windows:
        return "windows";
    }
    return "unknown";
}

} // namespace liquid
