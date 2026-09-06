#include "formats/deb/deb.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <map>

#include <zstd.h>

#include "formats/deflate/deflate.hpp"
#include "liquid/format.hpp"

namespace liquid::formats::deb {
namespace {

void field(std::vector<std::byte>& output, std::string value, std::size_t width) {
    value.resize(width, ' ');
    output.insert(output.end(),
                  reinterpret_cast<const std::byte*>(value.data()),
                  reinterpret_cast<const std::byte*>(value.data() + width));
}

auto ar(std::span<const std::pair<std::string, std::vector<std::byte>>> members)
    -> result<std::vector<std::byte>> {
    auto output = package::text_bytes("!<arch>\n");
    for (const auto& [name, bytes] : members) {
        if (name.size() > 15) {
            return std::unexpected(
                error{errc::invalid_argument, "deb member name is too long", name});
        }
        field(output, name + "/", 16);
        field(output, "0", 12);
        field(output, "0", 6);
        field(output, "0", 6);
        field(output, "100644", 8);
        field(output, std::to_string(bytes.size()), 10);
        output.push_back(std::byte{'`'});
        output.push_back(std::byte{'\n'});
        output.insert(output.end(), bytes.begin(), bytes.end());
        if (bytes.size() % 2 != 0) {
            output.push_back(std::byte{'\n'});
        }
    }
    return output;
}

void relationship(std::string& control,
                  std::string_view name,
                  std::span<const std::string> values) {
    if (!values.empty()) {
        control += std::string{name} + ": " + package::join(values, ", ") + "\n";
    }
}

auto decompress_zstd(std::span<const std::byte> input, std::uint64_t maximum_output)
    -> result<std::vector<std::byte>> {
    const auto size = ZSTD_getFrameContentSize(input.data(), input.size());
    if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN ||
        size > maximum_output) {
        return std::unexpected(error{errc::corrupt, "invalid deb zstd member", "deb"});
    }
    auto output = std::vector<std::byte>(static_cast<std::size_t>(size));
    const auto decoded = ZSTD_decompress(output.data(), output.size(), input.data(), input.size());
    if (ZSTD_isError(decoded) || decoded != output.size()) {
        return std::unexpected(error{errc::corrupt, "cannot decode deb zstd member", "deb"});
    }
    return output;
}

} // namespace

result<std::vector<std::byte>> write(const package_metadata& metadata,
                                     const projection_options& options,
                                     std::span<const std::string> dependencies,
                                     std::span<const package::file> files,
                                     const generated_maintainer_scripts* scripts) {
    auto installed_size = std::uint64_t{};
    auto md5sums = std::string{};
    for (const auto& file : files) {
        installed_size += file.bytes.size();
        md5sums += package::md5_hex(file.bytes) + "  usr/" + file.path + "\n";
    }
    auto control = std::string{"Package: "} + metadata.name + "\nVersion: " + metadata.version +
                   "-" + metadata.release + "\nArchitecture: " + metadata.architecture +
                   "\nMaintainer: " + metadata.maintainer +
                   "\nInstalled-Size: " + std::to_string((installed_size + 1023) / 1024) +
                   "\nDescription: " + metadata.description + "\n";
    if (!metadata.homepage.empty()) {
        control += "Homepage: " + metadata.homepage + "\n";
    }
    if (!metadata.licence.empty()) {
        control += "License: " + metadata.licence + "\n";
    }
    relationship(control, "Depends", dependencies);
    relationship(control, "Pre-Depends", options.pre_depends);
    relationship(control, "Recommends", options.recommends);
    relationship(control, "Conflicts", metadata.conflicts);
    relationship(control, "Breaks", options.breaks);
    relationship(control, "Replaces", metadata.replaces);
    relationship(control, "Provides", metadata.provides);
    auto control_files =
        std::vector<package::file>{{"control", package::text_bytes(control), 0644},
                                   {"md5sums", package::text_bytes(md5sums), 0644}};
    if (!options.conffiles.empty()) {
        auto value = std::string{};
        for (const auto& path : options.conffiles) {
            value += "/usr/" + path + "\n";
        }
        control_files.push_back({"conffiles", package::text_bytes(value), 0644});
    }
    if (scripts) {
        control_files.push_back({"preinst", package::text_bytes(scripts->pre_install), 0755});
        control_files.push_back({"postinst", package::text_bytes(scripts->post_install), 0755});
        control_files.push_back({"prerm", package::text_bytes(scripts->pre_remove), 0755});
        control_files.push_back({"postrm", package::text_bytes(scripts->post_remove), 0755});
    }
    auto control_tar = package::tar(control_files, options.source_date_epoch);
    auto data_tar = package::tar(files, options.source_date_epoch, "usr");
    if (!control_tar || !data_tar) {
        return std::unexpected(control_tar ? data_tar.error() : control_tar.error());
    }
    auto control_bytes = std::vector<std::byte>{};
    auto data_bytes = std::vector<std::byte>{};
    auto control_name = std::string{};
    auto data_name = std::string{};
    if (options.compat_gzip) {
        control_bytes = deflate::gzip(*control_tar);
        data_bytes = deflate::gzip(*data_tar);
        control_name = "control.tar.gz";
        data_name = "data.tar.gz";
    } else {
        auto control_zstd = package::zstd(*control_tar);
        auto data_zstd = package::zstd(*data_tar);
        if (!control_zstd || !data_zstd) {
            return std::unexpected(control_zstd ? data_zstd.error() : control_zstd.error());
        }
        control_bytes = std::move(*control_zstd);
        data_bytes = std::move(*data_zstd);
        control_name = "control.tar.zst";
        data_name = "data.tar.zst";
    }
    const auto members =
        std::array{std::pair{std::string{"debian-binary"}, package::text_bytes("2.0\n")},
                   std::pair{std::move(control_name), std::move(control_bytes)},
                   std::pair{std::move(data_name), std::move(data_bytes)}};
    return ar(members);
}

result<archive> read(std::span<const std::byte> bytes, std::uint64_t maximum_output) {
    if (bytes.size() < 8 ||
        std::string_view{reinterpret_cast<const char*>(bytes.data()), 8} != "!<arch>\n") {
        return std::unexpected(error{errc::corrupt, "invalid ar magic", "deb"});
    }
    auto members = std::map<std::string, std::vector<std::byte>>{};
    auto offset = std::size_t{8};
    while (offset < bytes.size()) {
        if (bytes.size() - offset < 60) {
            return std::unexpected(error{errc::corrupt, "truncated ar header", "deb"});
        }
        auto name = std::string{reinterpret_cast<const char*>(bytes.data() + offset), 16};
        name.erase(name.find_last_not_of(" / ") + 1);
        auto size_text =
            std::string_view{reinterpret_cast<const char*>(bytes.data() + offset + 48), 10};
        auto size = std::size_t{};
        const auto [end, code] =
            std::from_chars(size_text.data(), size_text.data() + size_text.size(), size);
        if (code != std::errc{} || end == size_text.data() || size > bytes.size() - offset - 60) {
            return std::unexpected(error{errc::corrupt, "invalid ar member size", "deb"});
        }
        members[name] = {bytes.begin() + static_cast<std::ptrdiff_t>(offset + 60),
                         bytes.begin() + static_cast<std::ptrdiff_t>(offset + 60 + size)};
        offset += 60 + size + size % 2;
    }
    const auto gzip = members.contains("control.tar.gz") && members.contains("data.tar.gz");
    const auto zstd = members.contains("control.tar.zst") && members.contains("data.tar.zst");
    if (!gzip && !zstd) {
        return std::unexpected(error{errc::corrupt, "deb members are incomplete", "deb"});
    }
    auto decompress_member = [&](std::string_view stem,
                                 std::uint64_t limit) -> result<std::vector<std::byte>> {
        if (!gzip) {
            return decompress_zstd(members.at(std::string{stem} + ".zst"), limit);
        }
        auto decoded = deflate::gunzip(members.at(std::string{stem} + ".gz"), limit);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        return std::move(decoded->first);
    };
    auto control_tar = decompress_member("control.tar", maximum_output);
    if (!control_tar) {
        return std::unexpected(control_tar.error());
    }
    auto data_tar = decompress_member("data.tar", maximum_output - control_tar->size());
    if (!data_tar) {
        return std::unexpected(data_tar.error());
    }
    auto& registry = format_registry::global();
    auto control_tree = registry.unpack(*control_tar, "control.tar");
    auto data_tree = registry.unpack(*data_tar, "data.tar");
    if (!control_tree || !data_tree) {
        return std::unexpected(control_tree ? data_tree.error() : control_tree.error());
    }
    auto output = archive{};
    const auto control = std::ranges::find(control_tree->entries, "control", &format_entry::path);
    if (control == control_tree->entries.end()) {
        return std::unexpected(error{errc::corrupt, "deb control is absent", "deb"});
    }
    output.control.assign(reinterpret_cast<const char*>(control->bytes.data()),
                          control->bytes.size());
    auto scripts = generated_maintainer_scripts{};
    auto found_scripts = false;
    for (const auto& entry : control_tree->entries) {
        auto* destination = entry.path == "preinst"    ? &scripts.pre_install
                            : entry.path == "postinst" ? &scripts.post_install
                            : entry.path == "prerm"    ? &scripts.pre_remove
                            : entry.path == "postrm"   ? &scripts.post_remove
                                                       : nullptr;
        if (destination) {
            destination->assign(reinterpret_cast<const char*>(entry.bytes.data()),
                                entry.bytes.size());
            found_scripts = true;
        }
    }
    if (found_scripts) {
        output.scripts = std::move(scripts);
    }
    for (const auto& entry : data_tree->entries) {
        output.files.push_back(entry.path);
        if (entry.kind != format_entry_kind::member) {
            continue;
        }
        auto mode_text = package::attribute(entry.attributes, "mode");
        if (!mode_text) {
            return std::unexpected(mode_text.error());
        }
        auto mode = std::uint32_t{};
        const auto [end, code] =
            std::from_chars(mode_text->data(), mode_text->data() + mode_text->size(), mode);
        if (code != std::errc{} || end != mode_text->data() + mode_text->size()) {
            return std::unexpected(error{errc::corrupt, "deb file mode is invalid", entry.path});
        }
        output.payload.push_back({entry.path, entry.bytes, mode});
    }
    return output;
}

} // namespace liquid::formats::deb
