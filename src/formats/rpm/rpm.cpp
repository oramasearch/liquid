#include "formats/rpm/rpm.hpp"

#include <algorithm>
#include <array>
#include <map>

#include <zstd.h>

namespace liquid::formats::rpm {
namespace {

enum class value_type : std::uint32_t { int16 = 3, int32 = 4, string = 6, binary = 7, strings = 8 };

struct header_value {
    std::uint32_t tag{};
    value_type type{value_type::string};
    std::vector<std::byte> bytes;
    std::uint32_t count{};
};

void be16(std::vector<std::byte>& output, std::uint16_t value) {
    output.push_back(std::byte{static_cast<std::uint8_t>(value >> 8U)});
    output.push_back(std::byte{static_cast<std::uint8_t>(value)});
}

void be32(std::vector<std::byte>& output, std::uint32_t value) {
    for (auto shift : {24U, 16U, 8U, 0U}) {
        output.push_back(std::byte{static_cast<std::uint8_t>(value >> shift)});
    }
}

auto read_be32(std::span<const std::byte> input, std::size_t offset) -> std::uint32_t {
    auto output = std::uint32_t{};
    for (auto index = 0U; index < 4; ++index) {
        output = (output << 8U) | std::to_integer<std::uint8_t>(input[offset + index]);
    }
    return output;
}

auto string_value(std::uint32_t tag, std::string_view text) -> header_value {
    auto bytes = package::text_bytes(text);
    bytes.push_back(std::byte{});
    return {tag, value_type::string, std::move(bytes), 1};
}

auto strings_value(std::uint32_t tag, std::span<const std::string> values) -> header_value {
    auto bytes = std::vector<std::byte>{};
    for (const auto& value : values) {
        auto text = package::text_bytes(value);
        bytes.insert(bytes.end(), text.begin(), text.end());
        bytes.push_back(std::byte{});
    }
    return {tag, value_type::strings, std::move(bytes), static_cast<std::uint32_t>(values.size())};
}

auto int32_value(std::uint32_t tag, std::span<const std::uint32_t> values) -> header_value {
    auto bytes = std::vector<std::byte>{};
    for (const auto value : values) {
        be32(bytes, value);
    }
    return {tag, value_type::int32, std::move(bytes), static_cast<std::uint32_t>(values.size())};
}

auto int16_value(std::uint32_t tag, std::span<const std::uint16_t> values) -> header_value {
    auto bytes = std::vector<std::byte>{};
    for (const auto value : values) {
        be16(bytes, value);
    }
    return {tag, value_type::int16, std::move(bytes), static_cast<std::uint32_t>(values.size())};
}

auto binary_value(std::uint32_t tag, std::span<const std::byte> value) -> header_value {
    return {tag,
            value_type::binary,
            {value.begin(), value.end()},
            static_cast<std::uint32_t>(value.size())};
}

auto header(std::vector<header_value> values) -> std::vector<std::byte> {
    std::ranges::sort(values, {}, &header_value::tag);
    auto indexes = std::vector<std::array<std::uint32_t, 4>>{};
    auto store = std::vector<std::byte>{};
    for (auto& value : values) {
        const auto alignment = value.type == value_type::int32   ? 4U
                               : value.type == value_type::int16 ? 2U
                                                                 : 1U;
        while (store.size() % alignment != 0) {
            store.push_back(std::byte{});
        }
        indexes.push_back({value.tag,
                           static_cast<std::uint32_t>(value.type),
                           static_cast<std::uint32_t>(store.size()),
                           value.count});
        store.insert(store.end(), value.bytes.begin(), value.bytes.end());
    }
    auto output = std::vector<std::byte>{std::byte{0x8e},
                                         std::byte{0xad},
                                         std::byte{0xe8},
                                         std::byte{0x01},
                                         std::byte{},
                                         std::byte{},
                                         std::byte{},
                                         std::byte{}};
    be32(output, static_cast<std::uint32_t>(indexes.size()));
    be32(output, static_cast<std::uint32_t>(store.size()));
    for (const auto& index : indexes) {
        for (const auto value : index) {
            be32(output, value);
        }
    }
    output.insert(output.end(), store.begin(), store.end());
    return output;
}

auto lead(const package_metadata& metadata) -> std::vector<std::byte> {
    auto output = std::vector<std::byte>{std::byte{0xed},
                                         std::byte{0xab},
                                         std::byte{0xee},
                                         std::byte{0xdb},
                                         std::byte{0x03},
                                         std::byte{0x00}};
    be16(output, 0);
    be16(output, metadata.architecture == "noarch" ? 255 : 1);
    auto name = metadata.name + "-" + metadata.version + "-" + metadata.release;
    name.resize(66, '\0');
    output.insert(output.end(),
                  reinterpret_cast<const std::byte*>(name.data()),
                  reinterpret_cast<const std::byte*>(name.data() + 66));
    be16(output, 1);
    be16(output, 5);
    output.resize(96);
    return output;
}

auto parse_header(std::span<const std::byte> bytes,
                  std::size_t offset,
                  std::map<std::uint32_t, std::string>* strings = nullptr) -> result<std::size_t> {
    if (offset > bytes.size() || bytes.size() - offset < 16 || bytes[offset] != std::byte{0x8e} ||
        bytes[offset + 1] != std::byte{0xad} || bytes[offset + 2] != std::byte{0xe8}) {
        return std::unexpected(error{errc::corrupt, "invalid rpm header", "rpm"});
    }
    const auto count = read_be32(bytes, offset + 8);
    const auto data_size = read_be32(bytes, offset + 12);
    const auto index_size = static_cast<std::uint64_t>(count) * 16;
    if (index_size + data_size > bytes.size() - offset - 16) {
        return std::unexpected(error{errc::corrupt, "truncated rpm header", "rpm"});
    }
    const auto data = offset + 16 + static_cast<std::size_t>(index_size);
    if (strings) {
        for (auto index = 0U; index < count; ++index) {
            const auto entry = offset + 16 + index * 16;
            const auto tag = read_be32(bytes, entry);
            const auto type = read_be32(bytes, entry + 4);
            const auto value_offset = read_be32(bytes, entry + 8);
            if (type != 6 || value_offset >= data_size) {
                continue;
            }
            auto end = data + value_offset;
            while (end < data + data_size && bytes[end] != std::byte{}) {
                ++end;
            }
            (*strings)[tag] =
                std::string{reinterpret_cast<const char*>(bytes.data() + data + value_offset),
                            end - data - value_offset};
        }
    }
    return data + data_size;
}

auto cpio_files(std::span<const std::byte> bytes) -> result<std::vector<std::string>> {
    auto output = std::vector<std::string>{};
    auto offset = std::size_t{};
    while (offset + 110 <= bytes.size()) {
        if (std::string_view{reinterpret_cast<const char*>(bytes.data() + offset), 6} != "070701") {
            return std::unexpected(error{errc::corrupt, "invalid cpio payload", "rpm"});
        }
        const auto parse_hex = [&](std::size_t field) {
            auto value = std::uint32_t{};
            for (auto index = 0U; index < 8; ++index) {
                const auto character =
                    static_cast<char>(std::to_integer<std::uint8_t>(bytes[offset + field + index]));
                value = value * 16U + static_cast<std::uint32_t>(character <= '9'
                                                                     ? character - '0'
                                                                     : character - 'a' + 10);
            }
            return value;
        };
        const auto size = parse_hex(54);
        const auto name_size = parse_hex(94);
        if (name_size == 0 || name_size > bytes.size() - offset - 110) {
            return std::unexpected(error{errc::corrupt, "invalid cpio name", "rpm"});
        }
        const auto name_start = offset + 110;
        auto name =
            std::string{reinterpret_cast<const char*>(bytes.data() + name_start), name_size - 1};
        offset = (name_start + name_size + 3) & ~std::size_t{3};
        if (size > bytes.size() - offset) {
            return std::unexpected(error{errc::corrupt, "truncated cpio file", "rpm"});
        }
        if (name == "TRAILER!!!") {
            return output;
        }
        output.push_back(std::move(name));
        offset = (offset + size + 3) & ~std::size_t{3};
    }
    return std::unexpected(error{errc::corrupt, "cpio trailer is absent", "rpm"});
}

} // namespace

result<std::vector<std::byte>> write(const package_metadata& metadata,
                                     const projection_options& options,
                                     std::span<const std::string> dependencies,
                                     std::span<const package::file> files) {
    auto payload_uncompressed = package::cpio(files, options.source_date_epoch);
    if (!payload_uncompressed) {
        return std::unexpected(payload_uncompressed.error());
    }
    auto payload = package::zstd(*payload_uncompressed);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    auto basenames = std::vector<std::string>{};
    auto dirnames = std::vector<std::string>{};
    auto dirindexes = std::vector<std::uint32_t>{};
    auto sizes = std::vector<std::uint32_t>{};
    auto modes = std::vector<std::uint16_t>{};
    auto mtimes = std::vector<std::uint32_t>{};
    auto digests = std::vector<std::string>{};
    auto empty = std::vector<std::string>{};
    auto zeros = std::vector<std::uint32_t>{};
    auto zero16 = std::vector<std::uint16_t>{};
    auto roots = std::vector<std::string>{};
    auto total_size = std::uint64_t{};
    for (const auto& file : files) {
        const auto slash = file.path.rfind('/');
        basenames.push_back(slash == std::string::npos ? file.path : file.path.substr(slash + 1));
        const auto directory = slash == std::string::npos
                                   ? std::string{"/usr/"}
                                   : "/usr/" + file.path.substr(0, slash + 1);
        auto found = std::ranges::find(dirnames, directory);
        if (found == dirnames.end()) {
            dirnames.push_back(directory);
            found = std::prev(dirnames.end());
        }
        dirindexes.push_back(static_cast<std::uint32_t>(std::distance(dirnames.begin(), found)));
        sizes.push_back(static_cast<std::uint32_t>(file.bytes.size()));
        modes.push_back(static_cast<std::uint16_t>(0100000U | (file.mode & 0111U ? 0755U : 0644U)));
        mtimes.push_back(static_cast<std::uint32_t>(options.source_date_epoch));
        auto digest = package::sha256(file.bytes);
        if (!digest) {
            return std::unexpected(digest.error());
        }
        digests.push_back(package::hex_bytes(digest->bytes));
        empty.emplace_back();
        zeros.push_back(0);
        zero16.push_back(0);
        roots.emplace_back("root");
        total_size += file.bytes.size();
    }
    auto require_names = std::vector<std::string>{dependencies.begin(), dependencies.end()};
    auto require_versions = std::vector<std::string>(require_names.size());
    auto require_flags = std::vector<std::uint32_t>(require_names.size());
    require_names.push_back("rpmlib(PayloadIsZstd)");
    require_versions.push_back("5.4.18-1");
    require_flags.push_back(0x0100000aU);
    auto provide_names = metadata.provides;
    provide_names.push_back(metadata.name);
    auto provide_versions = std::vector<std::string>(provide_names.size());
    provide_versions.back() = metadata.version + "-" + metadata.release;
    auto provide_flags = std::vector<std::uint32_t>(provide_names.size());
    provide_flags.back() = 8;
    auto conflict_flags = std::vector<std::uint32_t>(metadata.conflicts.size());
    auto conflict_versions = std::vector<std::string>(metadata.conflicts.size());
    auto obsolete_flags = std::vector<std::uint32_t>(metadata.replaces.size());
    auto obsolete_versions = std::vector<std::string>(metadata.replaces.size());
    const auto zero = std::array<std::uint32_t, 1>{0};
    const auto total = std::array<std::uint32_t, 1>{static_cast<std::uint32_t>(total_size)};
    const auto algorithm = std::array<std::uint32_t, 1>{8};
    auto values = std::vector<header_value>{
        string_value(1000, metadata.name),
        string_value(1001, metadata.version),
        string_value(1002, metadata.release),
        string_value(1004, metadata.description),
        string_value(1005, metadata.description),
        int32_value(1006, zero),
        string_value(1007, "liquid"),
        int32_value(1009, total),
        string_value(1014, metadata.licence),
        string_value(1016, "Applications/System"),
        string_value(1020, metadata.homepage),
        string_value(1021, "linux"),
        string_value(1022, metadata.architecture),
        int32_value(1028, sizes),
        int16_value(1030, modes),
        int16_value(1033, zero16),
        int32_value(1034, mtimes),
        strings_value(1035, digests),
        strings_value(1036, empty),
        int32_value(1037, zeros),
        strings_value(1039, roots),
        strings_value(1040, roots),
        int32_value(1045, std::vector<std::uint32_t>(files.size(), 0xffffffffU)),
        strings_value(1047, provide_names),
        int32_value(1048, require_flags),
        strings_value(1049, require_names),
        strings_value(1050, require_versions),
        int32_value(1053, conflict_flags),
        strings_value(1054, metadata.conflicts),
        strings_value(1055, conflict_versions),
        string_value(1064, "4.18.0"),
        int32_value(1095, zeros),
        int32_value(1096, zeros),
        strings_value(1090, metadata.replaces),
        int32_value(1106, zero),
        int32_value(1112, provide_flags),
        strings_value(1113, provide_versions),
        int32_value(1114, obsolete_flags),
        strings_value(1115, obsolete_versions),
        int32_value(1116, dirindexes),
        strings_value(1117, basenames),
        strings_value(1118, dirnames),
        string_value(1124, "cpio"),
        string_value(1125, "zstd"),
        string_value(1126, "19"),
        int32_value(5011, algorithm)};
    auto main_header = header(std::move(values));
    auto header_digest = package::sha256(main_header);
    if (!header_digest) {
        return std::unexpected(header_digest.error());
    }
    const auto payload_size =
        std::array<std::uint32_t, 1>{static_cast<std::uint32_t>(payload->size())};
    auto signature = header({string_value(273, package::hex_bytes(header_digest->bytes)),
                             int32_value(1000, payload_size),
                             int32_value(1007, payload_size)});
    auto output = lead(metadata);
    output.insert(output.end(), signature.begin(), signature.end());
    while (output.size() % 8 != 0) {
        output.push_back(std::byte{});
    }
    output.insert(output.end(), main_header.begin(), main_header.end());
    output.insert(output.end(), payload->begin(), payload->end());
    return output;
}

result<archive> read(std::span<const std::byte> bytes, std::uint64_t maximum_output) {
    if (bytes.size() < 96 || bytes[0] != std::byte{0xed} || bytes[1] != std::byte{0xab} ||
        bytes[2] != std::byte{0xee} || bytes[3] != std::byte{0xdb}) {
        return std::unexpected(error{errc::corrupt, "invalid rpm lead", "rpm"});
    }
    auto signature_end = parse_header(bytes, 96);
    if (!signature_end) {
        return std::unexpected(signature_end.error());
    }
    const auto main_offset = (*signature_end + 7) & ~std::size_t{7};
    auto tags = std::map<std::uint32_t, std::string>{};
    auto payload_offset = parse_header(bytes, main_offset, &tags);
    if (!payload_offset || *payload_offset >= bytes.size()) {
        return std::unexpected(payload_offset ? error{errc::corrupt, "rpm payload is absent", "rpm"}
                                              : payload_offset.error());
    }
    const auto payload = bytes.subspan(*payload_offset);
    const auto size = ZSTD_getFrameContentSize(payload.data(), payload.size());
    if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN ||
        size > maximum_output) {
        return std::unexpected(error{errc::corrupt, "invalid rpm zstd payload", "rpm"});
    }
    auto decoded = std::vector<std::byte>(static_cast<std::size_t>(size));
    const auto decompressed =
        ZSTD_decompress(decoded.data(), decoded.size(), payload.data(), payload.size());
    if (ZSTD_isError(decompressed) || decompressed != decoded.size()) {
        return std::unexpected(error{errc::corrupt, "cannot decode rpm payload", "rpm"});
    }
    auto files = cpio_files(decoded);
    if (!files) {
        return std::unexpected(files.error());
    }
    return archive{tags[1000], tags[1001], std::move(*files)};
}

} // namespace liquid::formats::rpm
