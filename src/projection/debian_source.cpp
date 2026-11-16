// SPDX-License-Identifier: MIT

#include "projection/projection.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "debian_path_packages.hpp"
#include "formats/deflate/deflate.hpp"
#include "formats/package/common.hpp"
#include "formats/tar/writer.hpp"
#include "git/sha1.hpp"
#include "liquid/configuration.hpp"
#include "projection/attestations.hpp"

namespace liquid::projection_detail {
namespace {

struct path_package {
    std::string path;
    std::string package;
    bool implicit;
};

struct generated_file {
    std::string path;
    std::vector<std::byte> bytes;
    std::uint32_t mode{0644};
};

struct source_output {
    std::filesystem::path tree_path;
    std::filesystem::path dsc_path;
    std::vector<std::byte> dsc;
    digest dsc_digest;
    std::filesystem::path orig_path;
    std::vector<std::byte> orig;
    digest orig_digest;
    std::filesystem::path debian_path;
    std::vector<std::byte> debian;
    digest debian_digest;
    std::vector<generated_file> packaging;
    std::vector<std::string> dependencies;
    std::vector<std::string> diagnostics;
};

auto text_bytes(std::string_view value) -> std::vector<std::byte> {
    return formats::package::text_bytes(value);
}

auto split(std::string_view input, char separator) -> std::vector<std::string_view> {
    auto output = std::vector<std::string_view>{};
    for (auto begin = std::size_t{}; begin <= input.size();) {
        const auto end = input.find(separator, begin);
        output.push_back(input.substr(begin, end - begin));
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return output;
}

auto path_packages() -> std::vector<path_package> {
    auto output = std::vector<path_package>{};
    for (const auto line : split(debian_path_packages, '\n')) {
        if (line.empty() || line.starts_with('#')) {
            continue;
        }
        const auto fields = split(line, '\t');
        if (fields.size() >= 3) {
            output.push_back(
                {std::string{fields[0]}, std::string{fields[1]}, fields[2] == "implicit"});
        }
    }
    return output;
}

auto executable_path(std::string_view executable, std::span<const path_package> packages)
    -> std::optional<std::string> {
    if (executable.starts_with('/')) {
        return std::string{executable};
    }
    auto found = std::optional<std::string>{};
    for (const auto& item : packages) {
        if (std::filesystem::path{item.path}.filename() != executable) {
            continue;
        }
        if (found) {
            return std::nullopt;
        }
        found = item.path;
    }
    return found;
}

auto mapped_package(std::string_view path, std::span<const path_package> packages)
    -> std::optional<path_package> {
    const auto found = std::ranges::find(packages, path, &path_package::path);
    return found == packages.end() ? std::nullopt : std::optional{*found};
}

auto build_dependencies(const prepared_projection& prepared,
                        const action_input& target,
                        std::vector<std::string>& diagnostics) -> std::vector<std::string> {
    const auto packages = path_packages();
    auto paths = std::set<std::string>{};
    if (!target.command.empty()) {
        const auto command = executable_path(target.command.front(), packages);
        if (command) {
            paths.insert(*command);
        } else {
            diagnostics.push_back("unmapped build executable: " + target.command.front());
        }
        for (auto index = std::size_t{1}; index + 1 < target.command.size(); ++index) {
            if (target.command[index] == "-G" && target.command[index + 1] == "Ninja") {
                paths.insert("/usr/bin/ninja");
            }
        }
    }
    for (const auto& input : prepared.build_manifest) {
        if (input.path.starts_with('/')) {
            paths.insert(input.path);
        }
    }
    for (const auto& path : prepared.build_toolchain_paths) {
        paths.insert(path);
    }
    auto dependencies = std::set<std::string>{"debhelper-compat (= 13)"};
    for (const auto& path : paths) {
        const auto mapped = mapped_package(path, packages);
        if (mapped && !mapped->implicit) {
            dependencies.insert(mapped->package);
        }
    }
    return {dependencies.begin(), dependencies.end()};
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
    output.push_back('\'');
    return output;
}

auto is_shell_command(std::string_view value) -> bool {
    const auto name = std::filesystem::path{value}.filename().string();
    constexpr auto shells = std::array<std::string_view, 12>{"sh",
                                                             "bash",
                                                             "dash",
                                                             "zsh",
                                                             "fish",
                                                             "csh",
                                                             "tcsh",
                                                             "ksh",
                                                             "mksh",
                                                             "pwsh",
                                                             "powershell",
                                                             "cmd.exe"};
    return std::ranges::contains(shells, name);
}

auto command_text(const action_input& action) -> std::string {
    auto output = std::string{};
    if (action.cwd != ".") {
        output += "cd " + shell_quote(action.cwd) + " && ";
    }
    if (!action.env.empty()) {
        output += "env";
        for (const auto& [name, value] : action.env) {
            output += " " + shell_quote(name + "=" + value);
        }
        output.push_back(' ');
    }
    for (auto index = std::size_t{}; index < action.command.size(); ++index) {
        if (index != 0) {
            output.push_back(' ');
        }
        output += shell_quote(action.command[index]);
    }
    return output;
}

auto selected_target(const prepared_projection& prepared) -> result<action_input> {
    if (prepared.build_action) {
        if (prepared.build_action->command.empty()) {
            return std::unexpected(error{errc::invalid_argument,
                                         "recorded build action has no command",
                                         "Debian source projection"});
        }
        if (is_shell_command(prepared.build_action->command.front())) {
            return std::unexpected(error{errc::invalid_argument,
                                         "Debian rules refuse a shell command target",
                                         prepared.build_action->command.front()});
        }
        return *prepared.build_action;
    }
    auto targets = std::vector<target>{};
    for (const auto& file : prepared.files) {
        if (!file.path.ends_with(".liquid") || file.path.find('/') != std::string::npos) {
            continue;
        }
        const auto text =
            std::string{reinterpret_cast<const char*>(file.bytes.data()), file.bytes.size()};
        auto parsed = parse_configuration(text, file.path);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        auto lowered = lower(*parsed);
        if (!lowered) {
            return std::unexpected(lowered.error());
        }
        targets.insert(targets.end(), lowered->targets.begin(), lowered->targets.end());
    }
    if (targets.size() != 1) {
        return std::unexpected(error{errc::invalid_argument,
                                     "Debian source projection requires exactly one lowered target",
                                     "*.liquid"});
    }
    const auto& value = targets.front();
    if (value.command.empty()) {
        return std::unexpected(
            error{errc::invalid_argument, "lowered target has no build command", value.name});
    }
    if (is_shell_command(value.command.front())) {
        return std::unexpected(error{errc::invalid_argument,
                                     "Debian rules refuse a shell command target",
                                     value.command.front()});
    }
    const auto inputs = value.inputs.value_or(digest{});
    const auto toolchain = value.toolchain.value_or(digest{});
    return action_input{.command = value.command,
                        .cwd = value.cwd,
                        .env = value.environment,
                        .outputs = value.outputs,
                        .inputs = inputs,
                        .toolchain = toolchain};
}

auto target_install_intent(const prepared_projection& prepared)
    -> result<std::optional<install_intent>> {
    if (prepared.install) {
        return prepared.install;
    }
    for (const auto& file : prepared.files) {
        if (!file.path.ends_with(".liquid") || file.path.find('/') != std::string::npos) {
            continue;
        }
        const auto text =
            std::string{reinterpret_cast<const char*>(file.bytes.data()), file.bytes.size()};
        auto parsed = parse_configuration(text, file.path);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        auto lowered = lower(*parsed);
        if (!lowered) {
            return std::unexpected(lowered.error());
        }
        if (lowered->targets.size() == 1) {
            return lowered->targets.front().install;
        }
    }
    return std::optional<install_intent>{};
}

auto debian_package_name_is_valid(std::string_view name) -> bool {
    return name.size() >= 2 && std::isdigit(static_cast<unsigned char>(name.front())) == 0 &&
           std::ranges::all_of(name, [](unsigned char character) {
               return std::islower(character) != 0 || std::isdigit(character) != 0 ||
                      character == '+' || character == '.' || character == '-';
           });
}

auto debian_identity_is_valid(const package_metadata& metadata, const projection_options& options)
    -> bool {
    const auto version_character = [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '.' || character == '+' ||
               character == '~';
    };
    return debian_package_name_is_valid(metadata.name) && !metadata.version.empty() &&
           std::ranges::all_of(metadata.version, version_character) && !metadata.release.empty() &&
           std::ranges::all_of(metadata.release, version_character) &&
           !options.distribution.empty() &&
           std::ranges::all_of(options.distribution, [](unsigned char character) {
               return std::isalnum(character) != 0 || character == '-' || character == '+' ||
                      character == '.';
           });
}

auto source_files(const prepared_projection& prepared) -> std::vector<formats::tar::entry> {
    auto output = std::vector<formats::tar::entry>{};
    const auto prefix = prepared.metadata.name + "-" + prepared.metadata.version + "/";
    for (const auto& file : prepared.files) {
        if (file.path == "debian" || file.path.starts_with("debian/")) {
            continue;
        }
        const auto symbolic_link = (file.mode & 0170000U) == 0120000U;
        auto link = std::string{};
        if (symbolic_link) {
            link.assign(reinterpret_cast<const char*>(file.bytes.data()), file.bytes.size());
        }
        output.push_back({.path = prefix + file.path,
                          .bytes = symbolic_link ? std::vector<std::byte>{} : file.bytes,
                          .mode = file.mode & 0111U ? 0755U : 0644U,
                          .link_path = std::move(link),
                          .symbolic_link = symbolic_link});
    }
    return output;
}

auto install_path(std::string path) -> std::string {
    if (path.starts_with("bin/")) {
        return "usr/" + path;
    }
    if (path.starts_with("include/")) {
        return "usr/" + path;
    }
    if (path.starts_with("share/")) {
        return "usr/" + path;
    }
    if (path.starts_with("lib/")) {
        const auto tail = path.substr(4);
        if (tail.starts_with("pkgconfig/")) {
            return "usr/lib/*/pkgconfig/" + tail.substr(10);
        }
        return "usr/lib/*/" + tail;
    }
    return path;
}

auto install_file(const action_input& target) -> std::string {
    auto paths = std::vector<std::string>{};
    paths.reserve(target.outputs.size());
    for (const auto& output : target.outputs) {
        paths.push_back(install_path(output));
    }
    std::ranges::sort(paths);
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    auto text = std::string{};
    for (const auto& path : paths) {
        text += path + "\n";
    }
    return text;
}

auto rules_file(const action_input& target, std::uint64_t epoch) -> std::string {
    auto output = std::string{"#!/usr/bin/make -f\n\n"};
    output += "export DH_VERBOSE = 1\n";
    output += "export DEB_BUILD_MAINT_OPTIONS = hardening=+all reproducible=+all\n";
    output += "DPKG_EXPORT_BUILDFLAGS = 1\n";
    output += "include /usr/share/dpkg/buildflags.mk\n";
    output += "include /usr/share/dpkg/architecture.mk\n";
    output += "export SOURCE_DATE_EPOCH = " + std::to_string(epoch) + "\n\n";
    output += "%:\n\tdh $@\n\n";
    output += "override_dh_auto_build:\n\t" + command_text(target) + "\n\n";
    output += "override_dh_auto_install:\n";
    output += "\tdh_auto_install --destdir=debian/tmp\n";
    return output;
}

auto debian_date(std::chrono::sys_seconds value) -> std::string {
    constexpr auto weekdays = std::array{"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};
    constexpr auto months = std::array{
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const auto day = std::chrono::floor<std::chrono::days>(value);
    const auto date = std::chrono::year_month_day{day};
    const auto time = std::chrono::hh_mm_ss{value - day};
    const auto day_number = day.time_since_epoch().count();
    const auto weekday = static_cast<std::size_t>((day_number % 7 + 7) % 7);
    return std::format("{}, {:02} {} {:04} {:02}:{:02}:{:02} +0000",
                       weekdays[weekday],
                       static_cast<unsigned>(date.day()),
                       months[static_cast<unsigned>(date.month()) - 1],
                       static_cast<int>(date.year()),
                       time.hours().count(),
                       time.minutes().count(),
                       time.seconds().count());
}

auto changelog_file(const prepared_projection& prepared) -> std::string {
    auto messages = std::vector<std::string>{};
    auto at = std::chrono::sys_seconds{std::chrono::seconds{prepared.options.source_date_epoch}};
    for (const auto& change : prepared.source_changes) {
        if (!change.message.empty()) {
            messages.push_back(change.message);
        }
    }
    if (!prepared.source_changes.empty()) {
        at = prepared.source_changes.front().at;
    }
    if (messages.empty()) {
        messages.push_back("Project the recorded liquid source tree.");
    }
    auto output = prepared.metadata.name + " (" + prepared.metadata.version + "-" +
                  prepared.metadata.release + ") " + prepared.options.distribution +
                  "; urgency=medium\n\n";
    for (const auto& message : messages) {
        output += "  * " + message + "\n";
    }
    output += "\n -- " + prepared.metadata.maintainer + "  " + debian_date(at) + "\n";
    return output;
}

auto control_file(const prepared_projection& prepared,
                  std::span<const std::string> build_dependencies) -> std::string {
    auto output = std::string{"Source: "} + prepared.metadata.name + "\n";
    output += "Section: devel\nPriority: optional\n";
    output += "Maintainer: " + prepared.metadata.maintainer + "\n";
    output += "Build-Depends: " + formats::package::join(build_dependencies, ", ") + "\n";
    output += "Standards-Version: 4.6.2.0\nRules-Requires-Root: no\n";
    if (!prepared.metadata.homepage.empty()) {
        output += "Homepage: " + prepared.metadata.homepage + "\n";
    }
    output += "\nPackage: " + prepared.metadata.name + "\nArchitecture: any\n";
    auto dependencies = std::vector<std::string>{"${shlibs:Depends}", "${misc:Depends}"};
    dependencies.insert(dependencies.end(),
                        prepared.metadata.declared_dependencies.begin(),
                        prepared.metadata.declared_dependencies.end());
    output += "Depends: " + formats::package::join(dependencies, ", ") + "\n";
    output += "Description: " + prepared.metadata.description + "\n";
    output += " This package is built reproducibly from a digest-pinned liquid source tree.\n";
    return output;
}

auto copyright_file(const prepared_projection& prepared, std::vector<std::string>& diagnostics)
    -> std::string {
    auto licence = prepared.metadata.licence;
    auto copyright = prepared.metadata.copyright;
    if (licence.empty() || licence == "NOASSERTION") {
        licence = "UNKNOWN";
        diagnostics.push_back("unknown licence: *");
    }
    if (copyright.empty() || copyright == "NOASSERTION") {
        copyright = "UNKNOWN";
        diagnostics.push_back("unknown copyright: *");
    }
    auto output = std::string{"Format: https://www.debian.org/doc/packaging-manuals/"};
    output += "copyright-format/1.0/\n";
    output += "Upstream-Name: " + prepared.metadata.name + "\n";
    output +=
        "Source: " +
        (prepared.metadata.homepage.empty() ? std::string{"UNKNOWN"} : prepared.metadata.homepage) +
        "\n\n";
    output += "Files: *\nCopyright: " + copyright + "\nLicense: " + licence + "\n";
    if (licence == "Apache-2.0") {
        output += " On Debian systems, the complete text of the Apache License 2.0 is in\n";
        output += " /usr/share/common-licenses/Apache-2.0.\n";
    } else if (licence == "UNKNOWN") {
        output += " The source graph did not contain licence text; review is required.\n";
    } else {
        output += " The complete licence text is distributed with the upstream source.\n";
    }
    return output;
}

auto upstream_metadata(const prepared_projection& prepared) -> std::string {
    if (prepared.metadata.homepage.empty()) {
        return "---\n";
    }
    return "Repository-Browse: " + prepared.metadata.homepage + "\n";
}

auto packaging_files(const prepared_projection& prepared,
                     const action_input& target,
                     std::span<const std::string> dependencies,
                     std::vector<std::string>& diagnostics) -> result<std::vector<generated_file>> {
    auto output = std::vector<generated_file>{};
    output.push_back({"debian/control", text_bytes(control_file(prepared, dependencies)), 0644});
    output.push_back(
        {"debian/rules", text_bytes(rules_file(target, prepared.options.source_date_epoch)), 0755});
    output.push_back({"debian/changelog", text_bytes(changelog_file(prepared)), 0644});
    output.push_back({"debian/copyright", text_bytes(copyright_file(prepared, diagnostics)), 0644});
    output.push_back({"debian/source/format", text_bytes("3.0 (quilt)\n"), 0644});
    output.push_back({"debian/upstream/metadata", text_bytes(upstream_metadata(prepared)), 0644});
    output.push_back(
        {"debian/" + prepared.metadata.name + ".install", text_bytes(install_file(target)), 0644});
    auto intent = target_install_intent(prepared);
    if (!intent) {
        return std::unexpected(intent.error());
    }
    if (*intent) {
        auto scripts = generate_maintainer_scripts(projection_target::deb, **intent);
        if (!scripts) {
            return std::unexpected(scripts.error());
        }
        output.push_back({"debian/" + prepared.metadata.name + ".preinst",
                          text_bytes(scripts->pre_install),
                          0755});
        output.push_back({"debian/" + prepared.metadata.name + ".postinst",
                          text_bytes(scripts->post_install),
                          0755});
        output.push_back(
            {"debian/" + prepared.metadata.name + ".prerm", text_bytes(scripts->pre_remove), 0755});
        output.push_back({"debian/" + prepared.metadata.name + ".postrm",
                          text_bytes(scripts->post_remove),
                          0755});
    }
    return output;
}

auto debian_archive(std::span<const generated_file> files, std::uint64_t epoch)
    -> result<std::vector<std::byte>> {
    auto entries = std::vector<formats::tar::entry>{};
    entries.reserve(files.size());
    for (const auto& file : files) {
        entries.push_back({.path = file.path, .bytes = file.bytes, .mode = file.mode});
    }
    auto archive = formats::tar::write(entries, epoch);
    if (!archive) {
        return std::unexpected(archive.error());
    }
    return formats::deflate::gzip(*archive);
}

auto checksum_lines(std::string_view name, std::span<const std::byte> bytes)
    -> std::tuple<std::string, std::string, std::string> {
    const auto sha1 = git_detail::sha1_hex(git_detail::sha1_hash(bytes));
    const auto sha256 = formats::package::sha256(bytes);
    if (!sha256) {
        return {};
    }
    const auto size = std::to_string(bytes.size());
    return {" " + sha1 + " " + size + " " + std::string{name} + "\n",
            " " + formats::package::hex_bytes(sha256->bytes) + " " + size + " " +
                std::string{name} + "\n",
            " " + formats::package::md5_hex(bytes) + " " + size + " " + std::string{name} + "\n"};
}

auto dsc_file(const prepared_projection& prepared,
              std::span<const std::string> dependencies,
              std::string_view orig_name,
              std::span<const std::byte> orig,
              std::string_view debian_name,
              std::span<const std::byte> debian) -> std::string {
    const auto [orig_sha1, orig_sha256, orig_md5] = checksum_lines(orig_name, orig);
    const auto [debian_sha1, debian_sha256, debian_md5] = checksum_lines(debian_name, debian);
    auto output = std::string{"Format: 3.0 (quilt)\n"};
    output += "Source: " + prepared.metadata.name + "\n";
    output += "Binary: " + prepared.metadata.name + "\nArchitecture: any\n";
    output += "Version: " + prepared.metadata.version + "-" + prepared.metadata.release + "\n";
    output += "Maintainer: " + prepared.metadata.maintainer + "\n";
    if (!prepared.metadata.homepage.empty()) {
        output += "Homepage: " + prepared.metadata.homepage + "\n";
    }
    output += "Standards-Version: 4.6.2.0\n";
    output += "Build-Depends: " + formats::package::join(dependencies, ", ") + "\n";
    output += "Package-List:\n " + prepared.metadata.name + " deb devel optional arch=any\n";
    output += "Checksums-Sha1:\n" + orig_sha1 + debian_sha1;
    output += "Checksums-Sha256:\n" + orig_sha256 + debian_sha256;
    output += "Files:\n" + orig_md5 + debian_md5;
    return output;
}

auto generate_source(prepared_projection prepared) -> result<source_output> {
    if (!debian_identity_is_valid(prepared.metadata, prepared.options)) {
        return std::unexpected(error{errc::invalid_argument,
                                     "package identity is not valid for a Debian source package",
                                     prepared.metadata.name});
    }
    if (prepared.metadata.maintainer.find('<') == std::string::npos ||
        prepared.metadata.maintainer.find('@') == std::string::npos ||
        prepared.metadata.maintainer.find('>') == std::string::npos) {
        return std::unexpected(error{errc::invalid_argument,
                                     "Debian source maintainer must include a name and email",
                                     prepared.metadata.maintainer});
    }
    auto target = selected_target(prepared);
    if (!target) {
        return std::unexpected(target.error());
    }
    auto diagnostics = std::vector<std::string>{};
    auto dependencies = build_dependencies(prepared, *target, diagnostics);
    auto packaging = packaging_files(prepared, *target, dependencies, diagnostics);
    if (!packaging) {
        return std::unexpected(packaging.error());
    }
    auto orig_tar = formats::tar::write(source_files(prepared), prepared.options.source_date_epoch);
    if (!orig_tar) {
        return std::unexpected(orig_tar.error());
    }
    auto orig = formats::deflate::gzip(*orig_tar);
    auto debian = debian_archive(*packaging, prepared.options.source_date_epoch);
    if (!debian) {
        return std::unexpected(debian.error());
    }
    auto orig_digest = formats::package::sha256(orig);
    auto debian_digest = formats::package::sha256(*debian);
    if (!orig_digest || !debian_digest) {
        return std::unexpected(orig_digest ? debian_digest.error() : orig_digest.error());
    }
    const auto stem = prepared.metadata.name + "_" + prepared.metadata.version;
    const auto orig_name = stem + ".orig.tar.gz";
    const auto debian_name = stem + "-" + prepared.metadata.release + ".debian.tar.gz";
    auto dsc = text_bytes(dsc_file(prepared, dependencies, orig_name, orig, debian_name, *debian));
    auto dsc_digest = formats::package::sha256(dsc);
    if (!dsc_digest) {
        return std::unexpected(dsc_digest.error());
    }
    return source_output{.tree_path = prepared.options.output_directory /
                                      (prepared.metadata.name + "-" + prepared.metadata.version),
                         .dsc_path = prepared.options.output_directory /
                                     (stem + "-" + prepared.metadata.release + ".dsc"),
                         .dsc = std::move(dsc),
                         .dsc_digest = *dsc_digest,
                         .orig_path = prepared.options.output_directory / orig_name,
                         .orig = std::move(orig),
                         .orig_digest = *orig_digest,
                         .debian_path = prepared.options.output_directory / debian_name,
                         .debian = std::move(*debian),
                         .debian_digest = *debian_digest,
                         .packaging = std::move(*packaging),
                         .dependencies = std::move(dependencies),
                         .diagnostics = std::move(diagnostics)};
}

auto write_mode(const std::filesystem::path& path, std::uint32_t mode) -> result<void> {
    auto code = std::error_code{};
    auto permissions = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                       std::filesystem::perms::group_read | std::filesystem::perms::others_read;
    if ((mode & 0111U) != 0) {
        permissions |= std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                       std::filesystem::perms::others_exec;
    }
    std::filesystem::permissions(path, permissions, std::filesystem::perm_options::replace, code);
    if (code) {
        return std::unexpected(error{errc::io, code.message(), path.string()});
    }
    return {};
}

auto write_generated(const std::filesystem::path& root, const generated_file& file)
    -> result<void> {
    auto written = write_file(root / file.path, file.bytes);
    if (!written) {
        return written;
    }
    return write_mode(root / file.path, file.mode);
}

auto write_source_tree(const prepared_projection& prepared, const source_output& output)
    -> result<void> {
    auto code = std::error_code{};
    if (std::filesystem::exists(output.tree_path, code) &&
        !std::filesystem::is_empty(output.tree_path, code)) {
        return std::unexpected(error{
            errc::conflict, "Debian source tree output is not empty", output.tree_path.string()});
    }
    std::filesystem::create_directories(output.tree_path, code);
    if (code) {
        return std::unexpected(error{errc::io, code.message(), output.tree_path.string()});
    }
    for (const auto& file : prepared.files) {
        if (file.path == "debian" || file.path.starts_with("debian/")) {
            continue;
        }
        const auto path = output.tree_path / file.path;
        if ((file.mode & 0170000U) == 0120000U) {
            std::filesystem::create_directories(path.parent_path(), code);
            const auto target =
                std::string{reinterpret_cast<const char*>(file.bytes.data()), file.bytes.size()};
            std::filesystem::create_symlink(target, path, code);
            if (code) {
                return std::unexpected(error{errc::io, code.message(), path.string()});
            }
            continue;
        }
        auto written = write_file(path, file.bytes);
        if (!written) {
            return written;
        }
        auto mode = write_mode(path, file.mode);
        if (!mode) {
            return mode;
        }
    }
    for (const auto& file : output.packaging) {
        auto written = write_generated(output.tree_path, file);
        if (!written) {
            return written;
        }
    }
    return {};
}

auto write_output(const prepared_projection& prepared, const source_output& output)
    -> result<void> {
    auto tree = write_source_tree(prepared, output);
    if (!tree) {
        return tree;
    }
    const auto files =
        std::array{std::pair{output.dsc_path, std::span<const std::byte>{output.dsc}},
                   std::pair{output.orig_path, std::span<const std::byte>{output.orig}},
                   std::pair{output.debian_path, std::span<const std::byte>{output.debian}}};
    for (const auto& [path, bytes] : files) {
        auto written = write_file(path, bytes);
        if (!written) {
            return written;
        }
    }
    return {};
}

} // namespace

runtime::task<projection_report> project_debian_source(repository& source,
                                                       prepared_projection prepared) {
    auto generated = co_await runtime::offload([prepared] { return generate_source(prepared); });
    if (!generated) {
        co_return std::unexpected(generated.error());
    }
    if (!*generated) {
        co_return std::unexpected(generated->error());
    }
    auto output = std::move(**generated);
    auto stored =
        co_await runtime::offload([prepared, output] { return write_output(prepared, output); });
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    if (!*stored) {
        co_return std::unexpected(stored->error());
    }
    auto report = projection_report{
        .artifacts = {{output.dsc_path, output.dsc_digest, "text/vnd.debian.dsc"},
                      {output.orig_path, output.orig_digest, "application/gzip"},
                      {output.debian_path, output.debian_digest, "application/gzip"}},
        .attestations = prepared.source_attestations,
        .pinned = {{prepared.metadata.name, output.dsc_digest}},
        .abi_break_allowed = prepared.abi_break_allowed,
        .diagnostics = output.diagnostics};
    for (const auto& dependency : output.dependencies) {
        report.dependencies.push_back({dependency, false});
    }
    auto attached = co_await attach_sidecar(
        source, prepared, projection_target::debian_source, output.dsc_path, output.dsc_digest);
    if (!attached) {
        co_return std::unexpected(attached.error());
    }
    report.artifacts.push_back(attached->sidecar);
    report.attestations.push_back(attached->projection_attestation);
    co_return report;
}

} // namespace liquid::projection_detail
