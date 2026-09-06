#include "projection/projection.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <set>
#include <tuple>

#include "formats/deflate/deflate.hpp"
#include "formats/ostree/ostree.hpp"
#include "formats/tar/writer.hpp"
#include "projection/oci.hpp"

namespace liquid::projection_detail {
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
        case '\n':
            output += "\\n";
            break;
        default:
            output.push_back(character);
        }
    }
    output.push_back('\"');
    return output;
}

auto string_array(std::span<const std::string> values) -> std::string {
    auto output = std::string{"["};
    for (auto index = std::size_t{}; index < values.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        output += quote(values[index]);
    }
    output.push_back(']');
    return output;
}

auto object_path(const formats::ostree::object& object) -> std::string {
    const auto hex = formats::package::hex_bytes(object.checksum.bytes);
    return "sysroot/ostree/repo/objects/" + hex.substr(0, 2) + "/" + hex.substr(2) + "." +
           std::string{formats::ostree::object_suffix(object.kind)};
}

auto content_path(const formats::ostree::committed_file& file) -> std::string {
    const auto hex = formats::package::hex_bytes(file.checksum.bytes);
    return "sysroot/ostree/repo/objects/" + hex.substr(0, 2) + "/" + hex.substr(2) + ".file";
}

auto directory(std::string path, std::uint32_t mode = 0755, std::uint32_t order = 0)
    -> formats::tar::entry {
    return {.path = std::move(path), .mode = mode, .directory = true, .order = order};
}

auto regular(std::string path,
             std::vector<std::byte> bytes,
             std::uint32_t mode = 0644,
             std::uint32_t order = 1) -> formats::tar::entry {
    return {.path = std::move(path), .bytes = std::move(bytes), .mode = mode, .order = order};
}

auto symlink(std::string path,
             std::string target,
             std::uint32_t mode = 0777,
             std::uint32_t order = 1) -> formats::tar::entry {
    return {.path = std::move(path),
            .mode = mode,
            .link_path = std::move(target),
            .symbolic_link = true,
            .order = order};
}

auto metadata_entries(const formats::ostree::repository& repository)
    -> std::vector<formats::tar::entry> {
    auto entries = std::map<std::string, formats::tar::entry>{};
    const auto add_directory = [&](std::string path, std::uint32_t mode = 0755) {
        auto entry = directory(path, mode);
        entries.insert_or_assign(std::move(path), std::move(entry));
    };
    for (const auto* path : {"sysroot",
                             "sysroot/ostree",
                             "sysroot/ostree/repo",
                             "sysroot/ostree/repo/objects",
                             "sysroot/ostree/repo/refs",
                             "sysroot/ostree/repo/refs/heads",
                             "sysroot/ostree/repo/refs/mirrors",
                             "sysroot/ostree/repo/refs/remotes",
                             "sysroot/ostree/repo/state",
                             "sysroot/ostree/repo/tmp",
                             "sysroot/ostree/repo/tmp/cache"}) {
        add_directory(path);
    }
    for (const auto& object : repository.objects) {
        const auto hex = formats::package::hex_bytes(object.checksum.bytes);
        add_directory("sysroot/ostree/repo/objects/" + hex.substr(0, 2));
        if (object.kind == formats::ostree::object_kind::file) {
            continue;
        }
        const auto path = object_path(object);
        entries.insert_or_assign(path, regular(path, object.bytes));
    }
    for (const auto& value : repository.directories) {
        if (value.path.empty() || value.path == "sysroot") {
            continue;
        }
        add_directory(value.path, value.mode & 07777U);
    }
    const auto config =
        formats::package::text_bytes("[core]\nrepo_version=1\nmode=bare-split-xattrs\n");
    entries.insert_or_assign("sysroot/ostree/repo/config",
                             regular("sysroot/ostree/repo/config", config));
    const auto checksum = formats::package::hex_bytes(repository.commit.bytes) + "\n";
    entries.insert_or_assign("sysroot/ostree/repo/refs/heads/encapsulated",
                             regular("sysroot/ostree/repo/refs/heads/encapsulated",
                                     formats::package::text_bytes(checksum)));
    auto output = std::vector<formats::tar::entry>{};
    output.reserve(entries.size());
    for (auto& [path, entry] : entries) {
        output.push_back(std::move(entry));
    }
    return output;
}

auto component_entries(std::span<const formats::ostree::committed_file> files)
    -> std::vector<formats::tar::entry> {
    auto output = std::vector<formats::tar::entry>{};
    auto emitted = std::set<std::string>{};
    for (const auto& file : files) {
        const auto object = content_path(file);
        const auto symbolic_link = (file.mode & 0170000U) == 0120000U;
        const auto target =
            symbolic_link
                ? std::string{reinterpret_cast<const char*>(file.bytes.data()), file.bytes.size()}
                : std::string{};
        if (emitted.insert(object).second) {
            const auto xattrs = object.substr(0, object.size() - 5) + ".file-xattrs-link";
            output.push_back(regular(xattrs, {}, 0644, 0));
            if (symbolic_link) {
                output.push_back(symlink(object, target, file.mode & 07777U, 1));
            } else {
                output.push_back(regular(object, file.bytes, file.mode & 07777U, 1));
            }
        }
        if (symbolic_link) {
            output.push_back(symlink(file.path, target, file.mode & 07777U, 2));
        } else if (file.bytes.empty()) {
            output.push_back(regular(file.path, {}, file.mode & 07777U, 2));
        } else {
            output.push_back(
                {.path = file.path, .mode = file.mode & 07777U, .link_path = object, .order = 2});
        }
    }
    return output;
}

auto architecture(std::string_view value) -> std::string_view {
    if (value == "x86_64") {
        return "amd64";
    }
    if (value == "aarch64") {
        return "arm64";
    }
    return value;
}

struct built_bootc {
    formats::ostree::repository repository;
    std::filesystem::path root;
    oci_descriptor manifest;
};

auto build_bootc(prepared_projection prepared) -> result<built_bootc> {
    auto repository = formats::ostree::build(prepared.files,
                                             prepared.directory_modes,
                                             prepared.components,
                                             prepared.options.ostree_ref,
                                             prepared.options.source_date_epoch);
    if (!repository) {
        return std::unexpected(repository.error());
    }
    const auto root = prepared.options.output_directory;
    auto groups = std::map<std::string, std::vector<formats::ostree::committed_file>>{};
    for (const auto& file : repository->files) {
        groups[file.component].push_back(file);
    }
    auto layers = std::vector<oci_descriptor>{};
    auto diff_ids = std::vector<std::string>{};
    const auto add_layer = [&](std::vector<formats::tar::entry> entries,
                               std::map<std::string, std::string> annotations) -> result<void> {
        auto archive = formats::tar::write(entries, prepared.options.source_date_epoch);
        if (!archive) {
            return std::unexpected(archive.error());
        }
        auto diff = formats::package::sha256(*archive);
        if (!diff) {
            return std::unexpected(diff.error());
        }
        diff_ids.push_back(to_string(*diff));
        auto compressed = std::vector<std::byte>{};
        if (prepared.options.compat_gzip) {
            compressed = formats::deflate::gzip(*archive);
        } else {
            auto encoded = formats::package::zstd(*archive);
            if (!encoded) {
                return std::unexpected(encoded.error());
            }
            compressed = std::move(*encoded);
        }
        auto layer =
            oci_blob(root,
                     compressed,
                     prepared.options.compat_gzip ? "application/vnd.oci.image.layer.v1.tar+gzip"
                                                  : "application/vnd.oci.image.layer.v1.tar+zstd");
        if (!layer) {
            return std::unexpected(layer.error());
        }
        layer->annotations = std::move(annotations);
        layers.push_back(std::move(*layer));
        return {};
    };
    auto added = add_layer(metadata_entries(*repository), {{"ostree.encapsulated", "true"}});
    if (!added) {
        return std::unexpected(added.error());
    }
    for (auto& [component, files] : groups) {
        added = add_layer(component_entries(files), {{"ostree.components", component}});
        if (!added) {
            return std::unexpected(added.error());
        }
    }
    const auto created = std::format(
        "{:%FT%TZ}",
        std::chrono::sys_seconds{std::chrono::seconds{prepared.options.source_date_epoch}});
    const auto commit = formats::package::hex_bytes(repository->commit.bytes);
    auto labels = std::map<std::string, std::string>{{"containers.bootc", "1"},
                                                     {"ostree.bootable", "true"},
                                                     {"ostree.commit", commit},
                                                     {"ostree.final-diffid", diff_ids.back()},
                                                     {"version", prepared.metadata.version}};
    auto labels_json = std::string{"{"};
    auto first = true;
    for (const auto& [name, value] : labels) {
        if (!first) {
            labels_json.push_back(',');
        }
        first = false;
        labels_json += quote(name) + ":" + quote(value);
    }
    labels_json.push_back('}');
    auto config_json = std::string{"{\"created\":"} + quote(created) +
                       ",\"architecture\":" + quote(architecture(prepared.metadata.architecture)) +
                       ",\"os\":\"linux\",\"config\":{\"Labels\":" + labels_json +
                       ",\"Entrypoint\":" + string_array(prepared.options.oci_entrypoint) +
                       ",\"Cmd\":" + string_array(prepared.options.oci_command) +
                       "},\"rootfs\":{\"type\":\"layers\",\"diff_ids\":[";
    for (auto index = std::size_t{}; index < diff_ids.size(); ++index) {
        if (index != 0) {
            config_json.push_back(',');
        }
        config_json += quote(diff_ids[index]);
    }
    config_json += "]},\"history\":[";
    for (auto index = std::size_t{}; index < layers.size(); ++index) {
        if (index != 0) {
            config_json.push_back(',');
        }
        config_json += "{\"created\":" + quote(created) + ",\"created_by\":" +
                       quote(index == 0 ? "liquid ostree metadata"
                                        : "liquid ostree component " + std::to_string(index - 1)) +
                       "}";
    }
    config_json += "]}";
    auto config = oci_blob(root,
                           formats::package::text_bytes(config_json),
                           "application/vnd.oci.image.config.v1+json");
    if (!config) {
        return std::unexpected(config.error());
    }
    auto manifest = oci_manifest(root, *config, layers, {}, {}, labels);
    if (!manifest) {
        return std::unexpected(manifest.error());
    }
    manifest->annotations["org.opencontainers.image.ref.name"] =
        prepared.metadata.name + ":" + prepared.metadata.version;
    return built_bootc{std::move(*repository), root, std::move(*manifest)};
}

} // namespace

runtime::task<projection_report> project_bootc(repository& source, prepared_projection prepared) {
    auto built = co_await runtime::offload([prepared]() mutable { return build_bootc(prepared); });
    if (!built) {
        co_return std::unexpected(built.error());
    }
    if (!*built) {
        co_return std::unexpected(built->error());
    }
    auto& value = **built;
    auto projection = co_await store_projection_attestation(
        source, prepared, projection_target::bootc_image, value.manifest.content);
    if (!projection) {
        co_return std::unexpected(projection.error());
    }
    auto referrers =
        co_await attach_attestations(source, prepared, value.root, value.manifest, *projection);
    if (!referrers) {
        co_return std::unexpected(referrers.error());
    }
    auto indexed = co_await runtime::offload(
        [root = value.root, manifest = value.manifest, referrers = *referrers] {
            return write_oci_index(root, std::span{&manifest, 1}, referrers);
        });
    if (!indexed) {
        co_return std::unexpected(indexed.error());
    }
    if (!*indexed) {
        co_return std::unexpected(indexed->error());
    }
    auto report = projection_report{
        .artifacts = {{value.root, value.manifest.content, value.manifest.media_type}},
        .attestations = prepared.source_attestations,
        .pinned = {{prepared.metadata.name, value.manifest.content}},
        .ostree_commit = value.repository.commit,
        .abi_break_allowed = prepared.abi_break_allowed};
    report.attestations.push_back(*projection);
    co_return report;
}

} // namespace liquid::projection_detail
