// SPDX-License-Identifier: MIT

#include "formats/ostree/ostree.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <ranges>
#include <set>
#include <system_error>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/xattr.h>
#endif

namespace liquid::formats::ostree {
namespace {

struct variant {
    std::vector<std::byte> bytes;
    std::size_t alignment{1};
    bool fixed{};
};

struct directory_node {
    std::uint32_t mode{0040755};
    std::map<std::string, std::unique_ptr<directory_node>> directories;
    std::map<std::string, std::size_t> files;
};

auto failure(std::string message, std::string context = {}) -> error {
    return {errc::invalid_argument, std::move(message), std::move(context)};
}

void append_big_endian(std::vector<std::byte>& output, std::uint64_t value, std::size_t width) {
    for (auto index = width; index != 0; --index) {
        output.push_back(
            std::byte{static_cast<std::uint8_t>(value >> static_cast<unsigned>((index - 1) * 8))});
    }
}

void append_little_endian(std::vector<std::byte>& output, std::uint64_t value, std::size_t width) {
    for (auto index = std::size_t{}; index < width; ++index) {
        output.push_back(
            std::byte{static_cast<std::uint8_t>(value >> static_cast<unsigned>(index * 8))});
    }
}

void align(std::vector<std::byte>& output, std::size_t alignment) {
    while (output.size() % alignment != 0) {
        output.push_back(std::byte{});
    }
}

auto offset_width(std::size_t data_size, std::size_t count) -> std::size_t {
    for (const auto width : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        const auto maximum = width == 8 ? std::numeric_limits<std::uint64_t>::max()
                                        : (std::uint64_t{1} << (width * 8U)) - 1U;
        if (data_size <= maximum && count <= (maximum - data_size) / width) {
            return width;
        }
    }
    return 8;
}

auto unsigned32(std::uint32_t value) -> variant {
    auto output = variant{.alignment = 4, .fixed = true};
    append_big_endian(output.bytes, value, 4);
    return output;
}

auto unsigned64(std::uint64_t value) -> variant {
    auto output = variant{.alignment = 8, .fixed = true};
    append_big_endian(output.bytes, value, 8);
    return output;
}

auto string(std::string_view value) -> variant {
    auto output = variant{};
    output.bytes.insert(output.bytes.end(),
                        reinterpret_cast<const std::byte*>(value.data()),
                        reinterpret_cast<const std::byte*>(value.data() + value.size()));
    output.bytes.push_back(std::byte{});
    return output;
}

auto byte_array(std::span<const std::byte> bytes) -> variant {
    return {{bytes.begin(), bytes.end()}, 1, false};
}

auto array(std::span<const variant> children, std::size_t element_alignment) -> variant {
    auto output = variant{.alignment = element_alignment};
    if (children.empty()) {
        return output;
    }
    auto offsets = std::vector<std::size_t>{};
    const auto variable = !children.front().fixed;
    for (const auto& child : children) {
        align(output.bytes, element_alignment);
        output.bytes.insert(output.bytes.end(), child.bytes.begin(), child.bytes.end());
        if (variable) {
            offsets.push_back(output.bytes.size());
        }
    }
    if (variable) {
        const auto width = offset_width(output.bytes.size(), offsets.size());
        for (const auto offset : offsets) {
            append_little_endian(output.bytes, offset, width);
        }
    }
    return output;
}

auto tuple(std::span<const variant> children) -> variant {
    auto output = variant{.fixed = true};
    auto offsets = std::vector<std::size_t>{};
    for (const auto& child : children) {
        output.alignment = std::max(output.alignment, child.alignment);
        align(output.bytes, child.alignment);
        output.bytes.insert(output.bytes.end(), child.bytes.begin(), child.bytes.end());
        output.fixed = output.fixed && child.fixed;
        if (!child.fixed) {
            offsets.push_back(output.bytes.size());
        }
    }
    if (!children.empty() && !children.back().fixed && !offsets.empty()) {
        offsets.pop_back();
    }
    const auto width = offset_width(output.bytes.size(), offsets.size());
    for (const auto offset : std::views::reverse(offsets)) {
        append_little_endian(output.bytes, offset, width);
    }
    return output;
}

auto variant_value(variant child, std::string_view type) -> variant {
    auto output = variant{.alignment = 8};
    output.bytes = std::move(child.bytes);
    output.bytes.push_back(std::byte{});
    output.bytes.insert(output.bytes.end(),
                        reinterpret_cast<const std::byte*>(type.data()),
                        reinterpret_cast<const std::byte*>(type.data() + type.size()));
    return output;
}

auto string_array(std::span<const std::string> values) -> variant {
    auto children = std::vector<variant>{};
    children.reserve(values.size());
    for (const auto& value : values) {
        children.push_back(string(value));
    }
    return array(children, 1);
}

auto empty_dictionary() -> variant {
    return array(std::span<const variant>{}, 8);
}

auto ref_binding_metadata(std::string_view ref) -> variant {
    auto refs = std::vector<std::string>{std::string{ref}};
    auto value = variant_value(string_array(refs), "as");
    auto entry_children = std::array{string("ostree.ref-binding"), std::move(value)};
    auto entries = std::array{tuple(entry_children)};
    return array(entries, 8);
}

auto empty_xattrs() -> variant {
    return array(std::span<const variant>{}, 1);
}

auto unix_metadata(std::uint32_t mode) -> std::vector<std::byte> {
    const auto children =
        std::array{unsigned32(0), unsigned32(0), unsigned32(mode), empty_xattrs()};
    return tuple(children).bytes;
}

auto directory_metadata(std::uint32_t mode) -> std::vector<std::byte> {
    return unix_metadata(0040000U | (mode & 07777U));
}

auto file_header(std::uint32_t mode, std::string_view symlink_target) -> std::vector<std::byte> {
    const auto children = std::array{unsigned32(0),
                                     unsigned32(0),
                                     unsigned32(mode),
                                     unsigned32(0),
                                     string(symlink_target),
                                     empty_xattrs()};
    return tuple(children).bytes;
}

auto archive_file_header(std::uint64_t size, std::uint32_t mode, std::string_view symlink_target)
    -> std::vector<std::byte> {
    const auto children = std::array{unsigned64(size),
                                     unsigned32(0),
                                     unsigned32(0),
                                     unsigned32(mode),
                                     unsigned32(0),
                                     string(symlink_target),
                                     empty_xattrs()};
    return tuple(children).bytes;
}

auto length_prefixed(std::span<const std::byte> bytes) -> std::vector<std::byte> {
    auto output = std::vector<std::byte>{};
    append_big_endian(output, bytes.size(), 4);
    output.resize(8);
    output.insert(output.end(), bytes.begin(), bytes.end());
    return output;
}

auto raw_deflate(std::span<const std::byte> input) -> std::vector<std::byte> {
    auto output = std::vector<std::byte>{};
    auto offset = std::size_t{};
    do {
        const auto count = std::min<std::size_t>(65'535, input.size() - offset);
        const auto final = offset + count == input.size();
        output.push_back(final ? std::byte{0x01} : std::byte{0x00});
        append_little_endian(output, count, 2);
        append_little_endian(output, static_cast<std::uint16_t>(~count), 2);
        output.insert(output.end(),
                      input.begin() + static_cast<std::ptrdiff_t>(offset),
                      input.begin() + static_cast<std::ptrdiff_t>(offset + count));
        offset += count;
    } while (offset < input.size());
    return output;
}

auto sha256(std::span<const std::byte> bytes) -> result<digest> {
    return liquid::hash(algorithm::sha256, bytes);
}

auto hex_bytes(std::span<const std::byte> bytes) -> std::string {
    constexpr auto digits = std::string_view{"0123456789abcdef"};
    auto output = std::string{};
    output.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        const auto value = std::to_integer<std::uint8_t>(byte);
        output.push_back(digits[value >> 4]);
        output.push_back(digits[value & 0x0f]);
    }
    return output;
}

auto text_bytes(std::string_view value) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

auto checksum_bytes(const digest& value) -> std::span<const std::byte> {
    return value.bytes;
}

auto object_key(const digest& checksum, object_kind kind) -> std::string {
    return hex_bytes(checksum.bytes) + "." + std::string{object_suffix(kind)};
}

auto valid_path(std::string_view value) -> bool {
    const auto path = std::filesystem::path{value};
    return !value.empty() && !path.is_absolute() &&
           std::ranges::none_of(path, [](const auto& part) { return part == ".." || part == "."; });
}

auto valid_ref(std::string_view value) -> bool {
    return valid_path(value) && std::ranges::all_of(value, [](unsigned char character) {
               return (character >= 'a' && character <= 'z') ||
                      (character >= 'A' && character <= 'Z') ||
                      (character >= '0' && character <= '9') || character == '/' ||
                      character == '-' || character == '_' || character == '.';
           });
}

auto components(std::string_view path) -> std::vector<std::string> {
    auto output = std::vector<std::string>{};
    for (const auto& part : std::filesystem::path{path}) {
        output.push_back(part.generic_string());
    }
    return output;
}

auto write_bytes(const std::filesystem::path& path, std::span<const std::byte> bytes)
    -> result<void> {
    auto code = std::error_code{};
    std::filesystem::create_directories(path.parent_path(), code);
    if (code) {
        return std::unexpected(error{errc::io, code.message(), path.parent_path().string()});
    }
    auto stream = std::ofstream{path, std::ios::binary | std::ios::trunc};
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    stream.close();
    if (!stream) {
        return std::unexpected(error{errc::io, "cannot write ostree object", path.string()});
    }
    return {};
}

auto set_mode(const std::filesystem::path& path, std::uint32_t mode) -> result<void> {
    auto code = std::error_code{};
    std::filesystem::permissions(path,
                                 static_cast<std::filesystem::perms>(mode & 0777U),
                                 std::filesystem::perm_options::replace,
                                 code);
    if (code) {
        return std::unexpected(error{errc::io, code.message(), path.string()});
    }
    return {};
}

auto set_bare_user_metadata(const std::filesystem::path& path, std::uint32_t mode) -> result<void> {
    const auto metadata = unix_metadata(mode);
#if defined(__APPLE__)
    const auto status =
        setxattr(path.c_str(), "user.ostreemeta", metadata.data(), metadata.size(), 0, 0);
#elif defined(__linux__)
    const auto status =
        setxattr(path.c_str(), "user.ostreemeta", metadata.data(), metadata.size(), 0);
#else
    const auto status = -1;
    errno = ENOTSUP;
#endif
    if (status != 0) {
        return std::unexpected(error{errc::io, std::strerror(errno), path.string()});
    }
    return {};
}

} // namespace

std::string_view object_suffix(object_kind kind) {
    switch (kind) {
    case object_kind::file:
        return "file";
    case object_kind::dirtree:
        return "dirtree";
    case object_kind::dirmeta:
        return "dirmeta";
    case object_kind::commit:
        return "commit";
    }
    return "commit";
}

result<repository> build(std::span<const package::file> input_files,
                         const std::map<std::string, std::uint32_t>& directory_modes,
                         const std::map<std::string, std::string>& input_components,
                         std::string ref,
                         std::uint64_t source_date_epoch) {
    if (!valid_ref(ref)) {
        return std::unexpected(failure("ostree ref is not a safe relative name", ref));
    }
    auto output = repository{.ref = std::move(ref), .source_date_epoch = source_date_epoch};
    auto root = directory_node{};
    for (const auto& [path, mode] : directory_modes) {
        if (path.empty()) {
            root.mode = 0040000U | (mode & 07777U);
            continue;
        }
        if (!valid_path(path)) {
            return std::unexpected(failure("ostree directory path is invalid", path));
        }
        auto* current = &root;
        for (const auto& part : components(path)) {
            auto& child = current->directories[part];
            if (!child) {
                child = std::make_unique<directory_node>();
            }
            current = child.get();
        }
        current->mode = 0040000U | (mode & 07777U);
    }
    auto files = std::vector<package::file>{input_files.begin(), input_files.end()};
    std::ranges::sort(files, {}, &package::file::path);
    for (auto index = std::size_t{}; index < files.size(); ++index) {
        auto& file = files[index];
        if (!valid_path(file.path)) {
            return std::unexpected(failure("ostree file path is invalid", file.path));
        }
        auto parts = components(file.path);
        auto* current = &root;
        for (auto part = parts.begin(); part + 1 != parts.end(); ++part) {
            auto& child = current->directories[*part];
            if (!child) {
                child = std::make_unique<directory_node>();
            }
            current = child.get();
        }
        if (current->directories.contains(parts.back())) {
            return std::unexpected(failure("ostree path is both a file and directory", file.path));
        }
        if (current->files.contains(parts.back())) {
            return std::unexpected(failure("ostree file path is duplicated", file.path));
        }
        current->files.emplace(parts.back(), index);
    }

    auto object_positions = std::map<std::string, std::size_t>{};
    auto add_object = [&](object_kind kind, digest checksum, std::vector<std::byte> bytes) {
        const auto key = object_key(checksum, kind);
        if (object_positions.contains(key)) {
            return;
        }
        object_positions.emplace(key, output.objects.size());
        output.objects.push_back({kind, checksum, std::move(bytes)});
    };
    for (const auto& file : files) {
        const auto type = file.mode & 0170000U;
        const auto mode = type == 0 ? 0100000U | (file.mode & 07777U) : file.mode;
        if (type != 0 && type != 0100000U && type != 0120000U) {
            return std::unexpected(failure("ostree file type is unsupported", file.path));
        }
        const auto symlink = (mode & 0170000U) == 0120000U;
        const auto target = symlink
                                ? std::string_view{reinterpret_cast<const char*>(file.bytes.data()),
                                                   file.bytes.size()}
                                : std::string_view{};
        if (symlink && (target.empty() || target.find('\0') != std::string_view::npos)) {
            return std::unexpected(failure("ostree symlink target is invalid", file.path));
        }
        auto canonical = length_prefixed(file_header(mode, target));
        if (!symlink) {
            canonical.insert(canonical.end(), file.bytes.begin(), file.bytes.end());
        }
        auto checksum = sha256(canonical);
        if (!checksum) {
            return std::unexpected(checksum.error());
        }
        add_object(object_kind::file, *checksum, file.bytes);
        const auto component = input_components.find(file.path);
        output.files.push_back({file.path,
                                file.bytes,
                                mode,
                                *checksum,
                                component == input_components.end() ? "base" : component->second});
    }

    auto build_directory =
        std::function<result<std::pair<digest, digest>>(const directory_node&, std::string)>{};
    build_directory = [&](const directory_node& node,
                          std::string path) -> result<std::pair<digest, digest>> {
        auto file_entries = std::vector<variant>{};
        for (const auto& [name, index] : node.files) {
            const auto children =
                std::array{string(name), byte_array(checksum_bytes(output.files[index].checksum))};
            file_entries.push_back(tuple(children));
        }
        auto directory_entries = std::vector<variant>{};
        for (const auto& [name, child] : node.directories) {
            const auto child_path = path.empty() ? name : path + "/" + name;
            auto checksums = build_directory(*child, child_path);
            if (!checksums) {
                return std::unexpected(checksums.error());
            }
            const auto children = std::array{string(name),
                                             byte_array(checksum_bytes(checksums->first)),
                                             byte_array(checksum_bytes(checksums->second))};
            directory_entries.push_back(tuple(children));
        }
        const auto tree_children = std::array{array(file_entries, 1), array(directory_entries, 1)};
        auto tree_bytes = tuple(tree_children).bytes;
        auto tree_checksum = sha256(tree_bytes);
        if (!tree_checksum) {
            return std::unexpected(tree_checksum.error());
        }
        add_object(object_kind::dirtree, *tree_checksum, std::move(tree_bytes));
        auto meta_bytes = directory_metadata(node.mode);
        auto meta_checksum = sha256(meta_bytes);
        if (!meta_checksum) {
            return std::unexpected(meta_checksum.error());
        }
        add_object(object_kind::dirmeta, *meta_checksum, std::move(meta_bytes));
        output.directories.push_back({std::move(path), node.mode});
        return std::pair{*tree_checksum, *meta_checksum};
    };
    auto root_checksums = build_directory(root, {});
    if (!root_checksums) {
        return std::unexpected(root_checksums.error());
    }
    output.root_tree = root_checksums->first;
    output.root_meta = root_checksums->second;

    const auto commit_children = std::array{ref_binding_metadata(output.ref),
                                            byte_array(std::span<const std::byte>{}),
                                            array(std::span<const variant>{}, 1),
                                            string({}),
                                            string({}),
                                            unsigned64(source_date_epoch),
                                            byte_array(checksum_bytes(output.root_tree)),
                                            byte_array(checksum_bytes(output.root_meta))};
    auto commit_bytes = tuple(commit_children).bytes;
    auto commit_checksum = sha256(commit_bytes);
    if (!commit_checksum) {
        return std::unexpected(commit_checksum.error());
    }
    output.commit = *commit_checksum;
    add_object(object_kind::commit, output.commit, commit_bytes);

    const auto summary_value_children = std::array{unsigned64(commit_bytes.size()),
                                                   byte_array(checksum_bytes(output.commit)),
                                                   empty_dictionary()};
    const auto summary_entry_children =
        std::array{string(output.ref), tuple(summary_value_children)};
    const auto summary_entries = std::array{tuple(summary_entry_children)};
    const auto summary_children = std::array{array(summary_entries, 8), empty_dictionary()};
    output.summary = tuple(summary_children).bytes;
    std::ranges::sort(output.objects, [](const object& left, const object& right) {
        return object_key(left.checksum, left.kind) < object_key(right.checksum, right.kind);
    });
    std::ranges::sort(output.directories, {}, &committed_directory::path);
    return output;
}

result<void>
write(const repository& value, const std::filesystem::path& output, ostree_repository_mode mode) {
    const auto config =
        std::string{"[core]\nrepo_version=1\nmode="} + std::string{to_string(mode)} + "\n";
    auto written = write_bytes(output / "config", text_bytes(config));
    if (!written) {
        return std::unexpected(written.error());
    }
    for (const auto& object : value.objects) {
        const auto hex = hex_bytes(object.checksum.bytes);
        auto suffix = std::string{object_suffix(object.kind)};
        auto bytes = object.bytes;
        if (object.kind == object_kind::file && mode == ostree_repository_mode::archive_z2) {
            const auto found = std::ranges::find(value.files, hex, [](const committed_file& file) {
                return hex_bytes(file.checksum.bytes);
            });
            if (found == value.files.end()) {
                return std::unexpected(error{errc::corrupt, "ostree file object is absent", hex});
            }
            const auto symlink = (found->mode & 0170000U) == 0120000U;
            const auto target =
                symlink ? std::string_view{reinterpret_cast<const char*>(found->bytes.data()),
                                           found->bytes.size()}
                        : std::string_view{};
            bytes = length_prefixed(
                archive_file_header(symlink ? 0 : found->bytes.size(), found->mode, target));
            auto compressed = raw_deflate(symlink ? std::span<const std::byte>{}
                                                  : std::span<const std::byte>{found->bytes});
            bytes.insert(bytes.end(), compressed.begin(), compressed.end());
            suffix = "filez";
        }
        const auto path = output / "objects" / hex.substr(0, 2) / (hex.substr(2) + "." + suffix);
        written = write_bytes(path, bytes);
        if (!written) {
            return std::unexpected(written.error());
        }
        if (object.kind == object_kind::file && mode == ostree_repository_mode::bare_user) {
            const auto found = std::ranges::find(value.files, hex, [](const committed_file& file) {
                return hex_bytes(file.checksum.bytes);
            });
            if (found == value.files.end()) {
                return std::unexpected(error{errc::corrupt, "ostree file object is absent", hex});
            }
            if ((found->mode & 0170000U) == 0120000U) {
                bytes = found->bytes;
                bytes.push_back(std::byte{});
                written = write_bytes(path, bytes);
                if (!written) {
                    return std::unexpected(written.error());
                }
            }
            auto metadata = set_bare_user_metadata(path, found->mode);
            if (!metadata) {
                return std::unexpected(metadata.error());
            }
            auto permissions = set_mode(path, (found->mode & 0775U) | 0400U);
            if (!permissions) {
                return std::unexpected(permissions.error());
            }
        }
    }
    const auto commit = hex_bytes(value.commit.bytes) + "\n";
    written = write_bytes(output / "refs" / "heads" / value.ref, text_bytes(commit));
    if (!written) {
        return std::unexpected(written.error());
    }
    return write_bytes(output / "summary", value.summary);
}

} // namespace liquid::formats::ostree
