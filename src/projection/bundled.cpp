// SPDX-License-Identifier: MIT

#include "projection/projection.hpp"

#include <algorithm>
#include <array>
#include <cctype>

#include "formats/squashfs/squashfs.hpp"
#include "projection/appimage_runtime.hpp"
#include "projection/attestations.hpp"

namespace liquid::projection_detail {
namespace {

struct appimage_output {
    std::filesystem::path path;
    std::vector<std::byte> bytes;
    digest digest;
};

struct flatpak_output {
    std::filesystem::path root;
    std::filesystem::path manifest_path;
    std::vector<std::byte> manifest;
    digest manifest_digest;
    std::string metadata;
    std::vector<formats::package::file> payload;
};

struct snap_output {
    std::filesystem::path root;
    std::filesystem::path snap_path;
    std::vector<std::byte> snap;
    digest snap_digest;
    std::filesystem::path snapcraft_path;
    std::vector<std::byte> snapcraft;
    digest snapcraft_digest;
    std::vector<formats::package::file> payload;
    std::vector<formats::package::file> prime;
};

auto executable_name(std::span<const formats::package::file> files) -> result<std::string> {
    for (const auto& file : files) {
        if (file.path.starts_with("bin/") && file.path.find('/', 4) == std::string::npos &&
            (file.mode & 0111U) != 0) {
            return file.path.substr(4);
        }
    }
    return std::unexpected(
        error{errc::invalid_argument, "bundled projection requires a bin executable", "bin/"});
}

auto json_quote(std::string_view value) -> std::string {
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
            output.push_back(character);
        }
    }
    output.push_back('\"');
    return output;
}

auto yaml_quote(std::string_view value) -> std::string {
    auto output = std::string{"'"};
    for (const auto character : value) {
        if (character == '\'') {
            output += "''";
        } else {
            output.push_back(character);
        }
    }
    output.push_back('\'');
    return output;
}

auto desktop_name(std::string_view name) -> std::string {
    auto output = std::string{};
    auto capital = true;
    for (const auto character : name) {
        if (!std::isalnum(static_cast<unsigned char>(character))) {
            capital = true;
            continue;
        }
        output.push_back(capital ? static_cast<char>(std::toupper(character)) : character);
        capital = false;
    }
    if (output.empty() || !std::isalpha(static_cast<unsigned char>(output.front()))) {
        output.insert(0, "Liquid");
    }
    return output;
}

auto flatpak_architecture(std::string_view value) -> std::string {
    if (value == "x86_64" || value == "amd64") {
        return "x86_64";
    }
    if (value == "aarch64" || value == "arm64") {
        return "aarch64";
    }
    return std::string{value};
}

auto snap_architecture(std::string_view value) -> std::string {
    if (value == "x86_64") {
        return "amd64";
    }
    if (value == "aarch64") {
        return "arm64";
    }
    return std::string{value};
}

auto prefixed_files(std::span<const formats::package::file> files, std::string_view prefix)
    -> std::vector<formats::package::file> {
    auto output = std::vector<formats::package::file>{};
    output.reserve(files.size());
    for (auto file : files) {
        file.path = std::string{prefix} + file.path;
        output.push_back(std::move(file));
    }
    return output;
}

auto transparent_icon() -> std::vector<std::byte> {
    constexpr auto bytes = std::to_array<std::uint8_t>(
        {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
         0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00,
         0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41, 0x54, 0x08,
         0xd7, 0x63, 0xf8, 0xcf, 0xc0, 0xf0, 0x1f, 0x00, 0x05, 0x00, 0x01, 0xff, 0x89, 0x99,
         0x3d, 0x1d, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82});
    auto output = std::vector<std::byte>{};
    output.reserve(bytes.size());
    for (const auto value : bytes) {
        output.push_back(std::byte{value});
    }
    return output;
}

auto desktop_file(std::string_view app_id, std::string_view name, std::string_view executable)
    -> std::string {
    return std::string{"[Desktop Entry]\nType=Application\nName="} + std::string{name} +
           "\nExec=" + std::string{executable} + "\nIcon=" + std::string{app_id} +
           "\nCategories=Utility;\n";
}

auto write_tree(const std::filesystem::path& root, std::span<const formats::package::file> files)
    -> result<void> {
    for (const auto& file : files) {
        auto written = write_file(root / file.path, file.bytes);
        if (!written) {
            return std::unexpected(written.error());
        }
        if ((file.mode & 0111U) == 0) {
            continue;
        }
        auto code = std::error_code{};
        std::filesystem::permissions(root / file.path,
                                     std::filesystem::perms::owner_exec |
                                         std::filesystem::perms::group_exec |
                                         std::filesystem::perms::others_exec,
                                     std::filesystem::perm_options::add,
                                     code);
        if (code) {
            return std::unexpected(error{errc::io, code.message(), (root / file.path).string()});
        }
    }
    return {};
}

auto write_executable(const std::filesystem::path& path, std::span<const std::byte> bytes)
    -> result<void> {
    auto written = write_file(path, bytes);
    if (!written) {
        return written;
    }
    auto code = std::error_code{};
    std::filesystem::permissions(path,
                                 std::filesystem::perms::owner_exec |
                                     std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::add,
                                 code);
    if (code) {
        return std::unexpected(error{errc::io, code.message(), path.string()});
    }
    return {};
}

auto generate_appimage(const prepared_projection& prepared) -> result<appimage_output> {
    auto executable = executable_name(prepared.files);
    auto runtime = appimage_runtime(prepared.metadata.architecture);
    if (!executable || !runtime) {
        return std::unexpected(executable ? runtime.error() : executable.error());
    }
    auto files = prefixed_files(prepared.files, "usr/");
    const auto app_id = "org.liquid." + desktop_name(prepared.metadata.name);
    const auto app_run =
        std::string{"#!/bin/sh\nAPPDIR=\"${APPDIR:-$(dirname \"$0\")}\"\nexec \"$APPDIR/usr/bin/"} +
        *executable + "\" \"$@\"\n";
    files.push_back({"AppRun", formats::package::text_bytes(app_run), 0755});
    files.push_back({prepared.metadata.name + ".desktop",
                     formats::package::text_bytes(
                         desktop_file(app_id, prepared.metadata.description, *executable)),
                     0644});
    files.push_back({".DirIcon", transparent_icon(), 0644});
    auto squashfs = formats::squashfs::write(files, prepared.options.source_date_epoch);
    if (!squashfs) {
        return std::unexpected(squashfs.error());
    }
    auto bytes = std::vector<std::byte>{runtime->begin(), runtime->end()};
    bytes.insert(bytes.end(), squashfs->begin(), squashfs->end());
    auto digest = formats::package::sha256(bytes);
    if (!digest) {
        return std::unexpected(digest.error());
    }
    return appimage_output{prepared.options.output_directory /
                               (prepared.metadata.name + "-" + prepared.metadata.version + "-" +
                                flatpak_architecture(prepared.metadata.architecture) + ".AppImage"),
                           std::move(bytes),
                           *digest};
}

auto generate_flatpak(const prepared_projection& prepared) -> result<flatpak_output> {
    auto executable = executable_name(prepared.files);
    if (!executable) {
        return std::unexpected(executable.error());
    }
    const auto app_id = "org.liquid." + desktop_name(prepared.metadata.name);
    const auto architecture = flatpak_architecture(prepared.metadata.architecture);
    const auto desktop = desktop_file(app_id, prepared.metadata.description, *executable);
    auto payload = prepared.files;
    payload.push_back(
        {"share/applications/" + app_id + ".desktop", formats::package::text_bytes(desktop), 0644});
    payload.push_back(
        {"share/icons/hicolor/1x1/apps/" + app_id + ".png", transparent_icon(), 0644});
    auto manifest =
        std::string{"{\n  \"app-id\": "} + json_quote(app_id) +
        ",\n  \"runtime\": \"org.freedesktop.Platform\",\n" +
        "  \"runtime-version\": \"24.08\",\n" +
        "  \"sdk\": \"org.freedesktop.Sdk\",\n  \"command\": " + json_quote(*executable) +
        ",\n  \"finish-args\": [],\n  \"modules\": [\n" +
        "    {\n      \"name\": " + json_quote(prepared.metadata.name) +
        ",\n      \"buildsystem\": \"simple\",\n" +
        "      \"build-commands\": [\"cp -a . /app/\"],\n" +
        "      \"sources\": [{\"type\": \"dir\", \"path\": \"payload\"}]\n" + "    }\n  ]\n}\n";
    auto metadata = std::string{"[Application]\nname="} + app_id +
                    "\nruntime=org.freedesktop.Platform/" + architecture +
                    "/24.08\nsdk=org.freedesktop.Sdk/" + architecture +
                    "/24.08\ncommand=" + *executable + "\n";
    auto manifest_bytes = formats::package::text_bytes(manifest);
    auto manifest_digest = formats::package::sha256(manifest_bytes);
    if (!manifest_digest) {
        return std::unexpected(manifest_digest.error());
    }
    const auto root = prepared.options.output_directory /
                      (prepared.metadata.name + "-" + prepared.metadata.version + ".flatpak");
    return flatpak_output{root,
                          root / "manifest.json",
                          std::move(manifest_bytes),
                          *manifest_digest,
                          std::move(metadata),
                          std::move(payload)};
}

auto valid_snap_name(std::string_view name) -> bool {
    if (name.size() < 2 || name.size() > 40 ||
        !std::isalnum(static_cast<unsigned char>(name.front())) ||
        !std::isalnum(static_cast<unsigned char>(name.back())) || name.contains("--")) {
        return false;
    }
    return std::ranges::all_of(name, [](unsigned char character) {
        return std::islower(character) || std::isdigit(character) || character == '-';
    });
}

auto generate_snap(const prepared_projection& prepared) -> result<snap_output> {
    if (!valid_snap_name(prepared.metadata.name)) {
        return std::unexpected(error{
            errc::invalid_argument, "package name is not valid for Snap", prepared.metadata.name});
    }
    auto executable = executable_name(prepared.files);
    if (!executable) {
        return std::unexpected(executable.error());
    }
    const auto architecture = snap_architecture(prepared.metadata.architecture);
    auto snap_yaml = std::string{"name: "} + prepared.metadata.name +
                     "\nversion: " + yaml_quote(prepared.metadata.version) +
                     "\nsummary: " + yaml_quote(prepared.metadata.description) +
                     "\ndescription: " + yaml_quote(prepared.metadata.description) +
                     "\narchitectures:\n  - " + architecture +
                     "\nconfinement: strict\ngrade: stable\napps:\n  " + prepared.metadata.name +
                     ":\n    command: usr/bin/" + *executable + "\n";
    auto prime = prefixed_files(prepared.files, "usr/");
    prime.push_back({"meta/snap.yaml", formats::package::text_bytes(snap_yaml), 0644});
    auto snap = formats::squashfs::write(prime, prepared.options.source_date_epoch);
    if (!snap) {
        return std::unexpected(snap.error());
    }
    auto snap_digest = formats::package::sha256(*snap);
    if (!snap_digest) {
        return std::unexpected(snap_digest.error());
    }
    auto snapcraft = std::string{"name: "} + prepared.metadata.name +
                     "\nbase: core24\nversion: " + yaml_quote(prepared.metadata.version) +
                     "\nsummary: " + yaml_quote(prepared.metadata.description) +
                     "\ndescription: " + yaml_quote(prepared.metadata.description) +
                     "\ngrade: stable\nconfinement: strict\narchitectures:\n" + "  - build-on: [" +
                     architecture + "]\n    build-for: [" + architecture +
                     "]\nparts:\n  application:\n    plugin: dump\n    source: payload\n" +
                     "    organize:\n      '*': usr/\napps:\n  " + prepared.metadata.name +
                     ":\n    command: usr/bin/" + *executable + "\n";
    auto snapcraft_bytes = formats::package::text_bytes(snapcraft);
    auto snapcraft_digest = formats::package::sha256(snapcraft_bytes);
    if (!snapcraft_digest) {
        return std::unexpected(snapcraft_digest.error());
    }
    const auto root = prepared.options.output_directory /
                      (prepared.metadata.name + "-" + prepared.metadata.version + ".snapcraft");
    return snap_output{root,
                       prepared.options.output_directory /
                           (prepared.metadata.name + "_" + prepared.metadata.version + "_" +
                            architecture + ".snap"),
                       std::move(*snap),
                       *snap_digest,
                       root / "snapcraft.yaml",
                       std::move(snapcraft_bytes),
                       *snapcraft_digest,
                       prepared.files,
                       std::move(prime)};
}

auto append_attached(repository& source,
                     const prepared_projection& prepared,
                     projection_target target,
                     projection_report& report,
                     const projected_artifact& artifact) -> runtime::task<void> {
    auto attached =
        co_await attach_sidecar(source, prepared, target, artifact.path, artifact.digest);
    if (!attached) {
        co_return std::unexpected(attached.error());
    }
    report.artifacts.push_back(artifact);
    report.artifacts.push_back(attached->sidecar);
    report.attestations.push_back(attached->projection_attestation);
    co_return result<void>{};
}

} // namespace

runtime::task<projection_report> project_appimage(repository& source,
                                                  prepared_projection prepared) {
    auto generated = co_await runtime::offload([prepared] { return generate_appimage(prepared); });
    if (!generated) {
        co_return std::unexpected(generated.error());
    }
    if (!*generated) {
        co_return std::unexpected(generated->error());
    }
    auto output = std::move(**generated);
    auto stored = co_await runtime::offload(
        [path = output.path, bytes = output.bytes] { return write_executable(path, bytes); });
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    if (!*stored) {
        co_return std::unexpected(stored->error());
    }
    auto report = projection_report{.attestations = prepared.source_attestations,
                                    .pinned = {{prepared.metadata.name, output.digest}},
                                    .abi_break_allowed = prepared.abi_break_allowed};
    auto attached =
        co_await append_attached(source,
                                 prepared,
                                 projection_target::appimage,
                                 report,
                                 {output.path, output.digest, "application/vnd.appimage"});
    if (!attached) {
        co_return std::unexpected(attached.error());
    }
    co_return report;
}

runtime::task<projection_report> project_flatpak(repository& source, prepared_projection prepared) {
    auto generated = co_await runtime::offload([prepared] { return generate_flatpak(prepared); });
    if (!generated) {
        co_return std::unexpected(generated.error());
    }
    if (!*generated) {
        co_return std::unexpected(generated->error());
    }
    auto output = std::move(**generated);
    auto stored = co_await runtime::offload([output] {
        auto manifest = write_file(output.manifest_path, output.manifest);
        auto metadata =
            write_file(output.root / "metadata", formats::package::text_bytes(output.metadata));
        if (!manifest || !metadata) {
            return manifest ? metadata : manifest;
        }
        auto payload = write_tree(output.root / "payload", output.payload);
        auto files = write_tree(output.root / "files", output.payload);
        return payload ? files : payload;
    });
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    if (!*stored) {
        co_return std::unexpected(stored->error());
    }
    auto report = projection_report{.attestations = prepared.source_attestations,
                                    .pinned = {{prepared.metadata.name, output.manifest_digest}},
                                    .abi_break_allowed = prepared.abi_break_allowed};
    auto attached = co_await append_attached(
        source,
        prepared,
        projection_target::flatpak,
        report,
        {output.manifest_path, output.manifest_digest, "application/vnd.flatpak.manifest+json"});
    if (!attached) {
        co_return std::unexpected(attached.error());
    }
    co_return report;
}

runtime::task<projection_report> project_snap(repository& source, prepared_projection prepared) {
    auto generated = co_await runtime::offload([prepared] { return generate_snap(prepared); });
    if (!generated) {
        co_return std::unexpected(generated.error());
    }
    if (!*generated) {
        co_return std::unexpected(generated->error());
    }
    auto output = std::move(**generated);
    auto stored = co_await runtime::offload([output] {
        auto archive = write_file(output.snap_path, output.snap);
        auto recipe = write_file(output.snapcraft_path, output.snapcraft);
        if (!archive || !recipe) {
            return archive ? recipe : archive;
        }
        auto payload = write_tree(output.root / "payload", output.payload);
        auto prime = write_tree(output.root / "prime", output.prime);
        return payload ? prime : payload;
    });
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    if (!*stored) {
        co_return std::unexpected(stored->error());
    }
    auto report = projection_report{.attestations = prepared.source_attestations,
                                    .pinned = {{prepared.metadata.name, output.snap_digest}},
                                    .abi_break_allowed = prepared.abi_break_allowed};
    const auto artifacts = std::array{
        projected_artifact{output.snap_path, output.snap_digest, "application/vnd.snap"},
        projected_artifact{
            output.snapcraft_path, output.snapcraft_digest, "application/vnd.snapcraft.yaml"}};
    for (const auto& artifact : artifacts) {
        auto attached =
            co_await append_attached(source, prepared, projection_target::snap, report, artifact);
        if (!attached) {
            co_return std::unexpected(attached.error());
        }
    }
    co_return report;
}

} // namespace liquid::projection_detail
