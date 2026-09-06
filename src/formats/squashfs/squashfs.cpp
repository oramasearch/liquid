#include "formats/squashfs/squashfs.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>

namespace liquid::formats::squashfs {
namespace {

constexpr auto superblock_size = std::size_t{96};
constexpr auto data_block_size = std::size_t{128 * 1024};
constexpr auto metadata_block_size = std::size_t{8 * 1024};
constexpr auto uncompressed_metadata = std::uint16_t{1U << 15U};
constexpr auto uncompressed_data = std::uint32_t{1U << 24U};
constexpr auto invalid_u32 = std::numeric_limits<std::uint32_t>::max();
constexpr auto invalid_u64 = std::numeric_limits<std::uint64_t>::max();

enum class inode_type : std::uint16_t {
    directory = 8,
    regular = 9,
};

enum class directory_entry_type : std::uint16_t {
    directory = 1,
    regular = 2,
};

struct node {
    std::string path;
    std::vector<std::byte> bytes;
    std::uint32_t mode{0755};
    bool directory{true};
    std::uint32_t inode_number{};
    std::size_t inode_offset{};
    std::uint64_t data_start{};
    std::vector<std::uint32_t> data_blocks;
    std::size_t directory_offset{};
    std::size_t directory_size{};
};

auto failure(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

template <class Integer> void append_le(std::vector<std::byte>& output, Integer value) {
    using unsigned_type = std::make_unsigned_t<Integer>;
    const auto unsigned_value = static_cast<unsigned_type>(value);
    for (auto index = std::size_t{}; index < sizeof(Integer); ++index) {
        output.push_back(std::byte{static_cast<std::uint8_t>(unsigned_value >> (index * 8U))});
    }
}

auto metadata_location(std::size_t logical_offset) -> std::uint32_t {
    const auto blocks = logical_offset / metadata_block_size;
    return static_cast<std::uint32_t>(blocks * (metadata_block_size + 2));
}

auto metadata_offset(std::size_t logical_offset) -> std::uint16_t {
    return static_cast<std::uint16_t>(logical_offset % metadata_block_size);
}

auto inode_reference(const node& value) -> std::uint64_t {
    return std::uint64_t{metadata_location(value.inode_offset)} << 16U |
           metadata_offset(value.inode_offset);
}

auto parent_path(std::string_view path) -> std::string {
    const auto slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string{} : std::string{path.substr(0, slash)};
}

auto base_name(std::string_view path) -> std::string_view {
    const auto slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

auto valid_path(std::string_view path) -> bool {
    if (path.empty() || path.front() == '/' || path.back() == '/') {
        return false;
    }
    auto offset = std::size_t{};
    while (offset < path.size()) {
        const auto slash = path.find('/', offset);
        const auto component = path.substr(offset, slash - offset);
        if (component.empty() || component == "." || component == ".." || component.size() > 256) {
            return false;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        offset = slash + 1;
    }
    return true;
}

auto collect_nodes(std::span<const package::file> files) -> result<std::vector<node>> {
    auto values = std::map<std::string, node>{};
    values.emplace(std::string{}, node{});
    for (const auto& file : files) {
        if (!valid_path(file.path)) {
            return std::unexpected(
                failure(errc::invalid_argument, "invalid SquashFS path", file.path));
        }
        auto parent = parent_path(file.path);
        while (!parent.empty()) {
            const auto [parent_position, parent_inserted] =
                values.try_emplace(parent, node{.path = parent});
            if (!parent_inserted && !parent_position->second.directory) {
                return std::unexpected(
                    failure(errc::conflict, "SquashFS parent is a regular file", parent));
            }
            parent = parent_path(parent);
        }
        auto [position, inserted] =
            values.try_emplace(file.path,
                               node{.path = file.path,
                                    .bytes = file.bytes,
                                    .mode = file.mode & 0111U ? 0755U : 0644U,
                                    .directory = false});
        if (!inserted) {
            return std::unexpected(failure(errc::conflict, "duplicate SquashFS path", file.path));
        }
    }
    auto output = std::vector<node>{};
    output.reserve(values.size());
    for (auto& [path, value] : values) {
        output.push_back(std::move(value));
    }
    for (auto index = std::size_t{}; index < output.size(); ++index) {
        output[index].inode_number = static_cast<std::uint32_t>(index + 1);
    }
    return output;
}

auto write_data(std::vector<std::byte>& output, std::vector<node>& nodes) -> result<void> {
    for (auto& value : nodes) {
        if (value.directory) {
            continue;
        }
        value.data_start = output.size();
        for (auto offset = std::size_t{}; offset < value.bytes.size(); offset += data_block_size) {
            const auto count = std::min(data_block_size, value.bytes.size() - offset);
            const auto block = std::span<const std::byte>{value.bytes}.subspan(offset, count);
            auto compressed = package::zstd(block);
            if (!compressed) {
                return std::unexpected(compressed.error());
            }
            if (compressed->size() < block.size()) {
                if (compressed->size() >= uncompressed_data) {
                    return std::unexpected(
                        failure(errc::unsupported, "compressed SquashFS block is too large"));
                }
                value.data_blocks.push_back(static_cast<std::uint32_t>(compressed->size()));
                output.insert(output.end(), compressed->begin(), compressed->end());
                continue;
            }
            value.data_blocks.push_back(static_cast<std::uint32_t>(block.size()) |
                                        uncompressed_data);
            output.insert(output.end(), block.begin(), block.end());
        }
    }
    return {};
}

void assign_inode_offsets(std::vector<node>& nodes) {
    auto offset = std::size_t{};
    for (auto& value : nodes) {
        value.inode_offset = offset;
        offset += value.directory ? 40 : 56 + value.data_blocks.size() * sizeof(std::uint32_t);
    }
}

auto child_nodes(const std::vector<node>& nodes, std::string_view parent)
    -> std::vector<const node*> {
    auto output = std::vector<const node*>{};
    for (const auto& value : nodes) {
        if (!value.path.empty() && parent_path(value.path) == parent) {
            output.push_back(&value);
        }
    }
    return output;
}

auto append_directory_group(std::vector<std::byte>& output, std::span<const node* const> children)
    -> result<void> {
    if (children.empty()) {
        return {};
    }
    const auto inode_block = metadata_location(children.front()->inode_offset);
    const auto first_inode = children.front()->inode_number;
    append_le(output, static_cast<std::uint32_t>(children.size() - 1));
    append_le(output, inode_block);
    append_le(output, first_inode);
    for (const auto* child : children) {
        const auto name = base_name(child->path);
        const auto delta = static_cast<std::int64_t>(child->inode_number) - first_inode;
        if (metadata_location(child->inode_offset) != inode_block || delta < -32768 ||
            delta > 32767 || name.empty() || name.size() > 256) {
            return std::unexpected(
                failure(errc::invalid_argument, "invalid SquashFS directory group", child->path));
        }
        append_le(output, metadata_offset(child->inode_offset));
        append_le(output, static_cast<std::int16_t>(delta));
        append_le(output,
                  static_cast<std::uint16_t>(child->directory ? directory_entry_type::directory
                                                              : directory_entry_type::regular));
        append_le(output, static_cast<std::uint16_t>(name.size() - 1));
        output.insert(output.end(),
                      reinterpret_cast<const std::byte*>(name.data()),
                      reinterpret_cast<const std::byte*>(name.data() + name.size()));
    }
    return {};
}

auto write_directories(std::vector<node>& nodes) -> result<std::vector<std::byte>> {
    auto output = std::vector<std::byte>{};
    for (auto& directory : nodes) {
        if (!directory.directory) {
            continue;
        }
        directory.directory_offset = output.size();
        const auto children = child_nodes(nodes, directory.path);
        auto begin = std::size_t{};
        while (begin < children.size()) {
            const auto block = metadata_location(children[begin]->inode_offset);
            const auto first_inode = children[begin]->inode_number;
            auto end = begin;
            while (end < children.size() && end - begin < 256 &&
                   metadata_location(children[end]->inode_offset) == block &&
                   static_cast<std::int64_t>(children[end]->inode_number) - first_inode <= 32767) {
                ++end;
            }
            auto written = append_directory_group(
                output, std::span<const node* const>{children.data() + begin, end - begin});
            if (!written) {
                return std::unexpected(written.error());
            }
            begin = end;
        }
        directory.directory_size = output.size() - directory.directory_offset;
        if (directory.directory_size > std::numeric_limits<std::uint32_t>::max() - 3) {
            return std::unexpected(
                failure(errc::unsupported, "SquashFS directory table is too large"));
        }
    }
    return output;
}

void append_inode_base(std::vector<std::byte>& output,
                       inode_type type,
                       const node& value,
                       std::uint32_t source_date_epoch) {
    append_le(output, static_cast<std::uint16_t>(type));
    append_le(output, static_cast<std::uint16_t>(value.mode));
    append_le(output, std::uint16_t{});
    append_le(output, std::uint16_t{});
    append_le(output, source_date_epoch);
    append_le(output, value.inode_number);
}

auto write_inodes(const std::vector<node>& nodes, std::uint32_t source_date_epoch)
    -> std::vector<std::byte> {
    auto output = std::vector<std::byte>{};
    for (const auto& value : nodes) {
        if (value.directory) {
            append_inode_base(output, inode_type::directory, value, source_date_epoch);
            const auto children = child_nodes(nodes, value.path);
            const auto directory_children =
                std::ranges::count_if(children, [](const node* child) { return child->directory; });
            append_le(output, static_cast<std::uint32_t>(directory_children + 2));
            append_le(output, static_cast<std::uint32_t>(value.directory_size + 3));
            append_le(output, metadata_location(value.directory_offset));
            const auto parent_node = std::ranges::find(nodes, parent_path(value.path), &node::path);
            const auto parent = value.path.empty() || parent_node == nodes.end()
                                    ? value.inode_number
                                    : parent_node->inode_number;
            append_le(output, parent);
            append_le(output, std::uint16_t{});
            append_le(output, metadata_offset(value.directory_offset));
            append_le(output, invalid_u32);
            continue;
        }
        append_inode_base(output, inode_type::regular, value, source_date_epoch);
        append_le(output, value.data_start);
        append_le(output, static_cast<std::uint64_t>(value.bytes.size()));
        append_le(output, std::uint64_t{});
        append_le(output, std::uint32_t{1});
        append_le(output, invalid_u32);
        append_le(output, std::uint32_t{});
        append_le(output, invalid_u32);
        for (const auto size : value.data_blocks) {
            append_le(output, size);
        }
    }
    return output;
}

auto uncompressed_metadata_blocks(std::span<const std::byte> input)
    -> result<std::vector<std::byte>> {
    auto output = std::vector<std::byte>{};
    for (auto offset = std::size_t{}; offset < input.size(); offset += metadata_block_size) {
        const auto count = std::min(metadata_block_size, input.size() - offset);
        if (count >= uncompressed_metadata) {
            return std::unexpected(failure(errc::internal, "SquashFS metadata block is too large"));
        }
        append_le(
            output,
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(count) | uncompressed_metadata));
        output.insert(output.end(),
                      input.begin() + static_cast<std::ptrdiff_t>(offset),
                      input.begin() + static_cast<std::ptrdiff_t>(offset + count));
    }
    return output;
}

auto epoch32(std::uint64_t value) -> result<std::uint32_t> {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(
            failure(errc::invalid_argument, "SOURCE_DATE_EPOCH exceeds SquashFS limits"));
    }
    return static_cast<std::uint32_t>(value);
}

void write_superblock(std::vector<std::byte>& output,
                      const std::vector<node>& nodes,
                      std::uint32_t source_date_epoch,
                      std::uint64_t inode_table_start,
                      std::uint64_t directory_table_start,
                      std::uint64_t id_table_start) {
    auto header = std::vector<std::byte>{};
    append_le(header, std::uint32_t{0x73717368});
    append_le(header, static_cast<std::uint32_t>(nodes.size()));
    append_le(header, source_date_epoch);
    append_le(header, static_cast<std::uint32_t>(data_block_size));
    append_le(header, std::uint32_t{});
    append_le(header, std::uint16_t{6});
    append_le(header, std::uint16_t{17});
    append_le(header, std::uint16_t{(1U << 0U) | (1U << 4U) | (1U << 9U) | (1U << 11U)});
    append_le(header, std::uint16_t{1});
    append_le(header, std::uint16_t{4});
    append_le(header, std::uint16_t{});
    append_le(header, inode_reference(nodes.front()));
    append_le(header, static_cast<std::uint64_t>(output.size()));
    append_le(header, id_table_start);
    append_le(header, invalid_u64);
    append_le(header, inode_table_start);
    append_le(header, directory_table_start);
    append_le(header, invalid_u64);
    append_le(header, invalid_u64);
    std::copy(header.begin(), header.end(), output.begin());
}

} // namespace

result<std::vector<std::byte>> write(std::span<const package::file> files,
                                     std::uint64_t source_date_epoch) {
    auto timestamp = epoch32(source_date_epoch);
    if (!timestamp) {
        return std::unexpected(timestamp.error());
    }
    auto collected = collect_nodes(files);
    if (!collected) {
        return std::unexpected(collected.error());
    }
    auto nodes = std::move(*collected);
    auto output = std::vector<std::byte>(superblock_size);
    auto data = write_data(output, nodes);
    if (!data) {
        return std::unexpected(data.error());
    }
    assign_inode_offsets(nodes);
    auto directories = write_directories(nodes);
    if (!directories) {
        return std::unexpected(directories.error());
    }
    const auto inode_table_start = output.size();
    auto inodes = uncompressed_metadata_blocks(write_inodes(nodes, *timestamp));
    if (!inodes) {
        return std::unexpected(inodes.error());
    }
    output.insert(output.end(), inodes->begin(), inodes->end());
    const auto directory_table_start = output.size();
    auto directory_blocks = uncompressed_metadata_blocks(*directories);
    if (!directory_blocks) {
        return std::unexpected(directory_blocks.error());
    }
    output.insert(output.end(), directory_blocks->begin(), directory_blocks->end());
    const auto id_metadata_start = output.size();
    auto ids = std::vector<std::byte>{};
    append_le(ids, std::uint32_t{});
    auto id_blocks = uncompressed_metadata_blocks(ids);
    if (!id_blocks) {
        return std::unexpected(id_blocks.error());
    }
    output.insert(output.end(), id_blocks->begin(), id_blocks->end());
    const auto id_table_start = output.size();
    append_le(output, static_cast<std::uint64_t>(id_metadata_start));
    write_superblock(
        output, nodes, *timestamp, inode_table_start, directory_table_start, id_table_start);
    return output;
}

} // namespace liquid::formats::squashfs
