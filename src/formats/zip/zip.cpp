// SPDX-License-Identifier: MIT

#include "formats/builtin.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <utility>

#include <zlib.h>

namespace liquid::formats::detail {
namespace {

constexpr auto local_signature = std::uint32_t{0x04034b50};
constexpr auto central_signature = std::uint32_t{0x02014b50};
constexpr auto eocd_signature = std::uint32_t{0x06054b50};
constexpr auto zip64_eocd_signature = std::uint32_t{0x06064b50};
constexpr auto zip64_locator_signature = std::uint32_t{0x07064b50};

auto failure(std::string message, std::size_t offset = 0) -> error {
    return {errc::corrupt, std::move(message), "zip byte " + std::to_string(offset)};
}

template <class Integer>
auto little(std::span<const std::byte> input, std::size_t offset) -> result<Integer> {
    if (offset > input.size() || input.size() - offset < sizeof(Integer)) {
        return std::unexpected(failure("truncated integer", offset));
    }
    auto output = Integer{};
    for (auto index = std::size_t{}; index < sizeof(Integer); ++index) {
        output |= static_cast<Integer>(std::to_integer<std::uint8_t>(input[offset + index]))
                  << (index * 8U);
    }
    return output;
}

auto slice(std::span<const std::byte> input, std::uint64_t offset, std::uint64_t size)
    -> result<std::span<const std::byte>> {
    if (offset > input.size() || size > input.size() - static_cast<std::size_t>(offset)) {
        return std::unexpected(failure("range exceeds archive", static_cast<std::size_t>(offset)));
    }
    return input.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(size));
}

auto signature_at(std::span<const std::byte> input, std::size_t offset, std::uint32_t expected)
    -> result<void> {
    auto value = little<std::uint32_t>(input, offset);
    if (!value) {
        return std::unexpected(value.error());
    }
    if (*value != expected) {
        return std::unexpected(failure("invalid signature", offset));
    }
    return {};
}

struct directory_location {
    std::uint64_t offset{};
    std::uint64_t size{};
    std::uint64_t entries{};
};

auto find_eocd(std::span<const std::byte> input) -> result<std::size_t> {
    if (input.size() < 22) {
        return std::unexpected(failure("end of central directory is absent"));
    }
    const auto earliest = input.size() > 65'557 ? input.size() - 65'557 : 0;
    for (auto offset = input.size() - 22;; --offset) {
        auto signature = little<std::uint32_t>(input, offset);
        if (signature && *signature == eocd_signature) {
            auto comment_size = little<std::uint16_t>(input, offset + 20);
            if (comment_size && offset + 22 + *comment_size == input.size()) {
                return offset;
            }
        }
        if (offset == earliest) {
            break;
        }
    }
    return std::unexpected(failure("end of central directory is absent"));
}

auto directory(std::span<const std::byte> input) -> result<directory_location> {
    auto eocd = find_eocd(input);
    if (!eocd) {
        return std::unexpected(eocd.error());
    }
    auto disk = little<std::uint16_t>(input, *eocd + 4);
    auto directory_disk = little<std::uint16_t>(input, *eocd + 6);
    auto entries = little<std::uint16_t>(input, *eocd + 10);
    auto size = little<std::uint32_t>(input, *eocd + 12);
    auto offset = little<std::uint32_t>(input, *eocd + 16);
    if (!disk || !directory_disk || !entries || !size || !offset) {
        return std::unexpected(failure("truncated end of central directory", *eocd));
    }
    if (*disk != 0 || *directory_disk != 0) {
        return std::unexpected(error{errc::unsupported, "multi-disk zip is unsupported", "zip"});
    }
    const auto needs_zip64 = *entries == std::numeric_limits<std::uint16_t>::max() ||
                             *size == std::numeric_limits<std::uint32_t>::max() ||
                             *offset == std::numeric_limits<std::uint32_t>::max();
    if (!needs_zip64) {
        return directory_location{*offset, *size, *entries};
    }
    if (*eocd < 20) {
        return std::unexpected(failure("zip64 locator is absent", *eocd));
    }
    auto locator_signature = little<std::uint32_t>(input, *eocd - 20);
    auto zip64_offset = little<std::uint64_t>(input, *eocd - 12);
    if (!locator_signature || *locator_signature != zip64_locator_signature || !zip64_offset) {
        return std::unexpected(failure("invalid zip64 locator", *eocd - 20));
    }
    auto zip64_signature = little<std::uint32_t>(input, static_cast<std::size_t>(*zip64_offset));
    if (!zip64_signature || *zip64_signature != zip64_eocd_signature) {
        return std::unexpected(failure("invalid zip64 end record", *zip64_offset));
    }
    auto zip64_entries = little<std::uint64_t>(input, *zip64_offset + 32);
    auto zip64_size = little<std::uint64_t>(input, *zip64_offset + 40);
    auto zip64_directory = little<std::uint64_t>(input, *zip64_offset + 48);
    if (!zip64_entries || !zip64_size || !zip64_directory) {
        return std::unexpected(failure("truncated zip64 end record", *zip64_offset));
    }
    return directory_location{*zip64_directory, *zip64_size, *zip64_entries};
}

struct central_entry {
    std::string name;
    std::uint16_t flags{};
    std::uint16_t method{};
    std::uint16_t time{};
    std::uint16_t date{};
    std::uint32_t crc{};
    std::uint32_t external_attributes{};
    std::uint64_t compressed_size{};
    std::uint64_t uncompressed_size{};
    std::uint64_t local_offset{};
};

auto zip64_extra(std::span<const std::byte> extra,
                 std::uint32_t size32,
                 std::uint32_t compressed32,
                 std::uint32_t offset32,
                 central_entry& output) -> result<void> {
    auto position = std::size_t{};
    while (position < extra.size()) {
        auto id = little<std::uint16_t>(extra, position);
        auto length = little<std::uint16_t>(extra, position + 2);
        if (!id || !length || position + 4 + *length > extra.size()) {
            return std::unexpected(failure("truncated extra field", position));
        }
        if (*id != 0x0001) {
            position += 4 + *length;
            continue;
        }
        auto value_offset = position + 4;
        const auto end = value_offset + *length;
        const auto take = [&](std::uint64_t& value) -> result<void> {
            if (value_offset + 8 > end) {
                return std::unexpected(failure("truncated zip64 extra field", value_offset));
            }
            auto decoded = little<std::uint64_t>(extra, value_offset);
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            value = *decoded;
            value_offset += 8;
            return {};
        };
        if (size32 == std::numeric_limits<std::uint32_t>::max()) {
            auto read = take(output.uncompressed_size);
            if (!read) {
                return read;
            }
        }
        if (compressed32 == std::numeric_limits<std::uint32_t>::max()) {
            auto read = take(output.compressed_size);
            if (!read) {
                return read;
            }
        }
        if (offset32 == std::numeric_limits<std::uint32_t>::max()) {
            auto read = take(output.local_offset);
            if (!read) {
                return read;
            }
        }
        return {};
    }
    if (size32 == std::numeric_limits<std::uint32_t>::max() ||
        compressed32 == std::numeric_limits<std::uint32_t>::max() ||
        offset32 == std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(failure("zip64 extra field is absent"));
    }
    return {};
}

auto central_entries(std::span<const std::byte> input) -> result<std::vector<central_entry>> {
    auto location = directory(input);
    if (!location) {
        return std::unexpected(location.error());
    }
    if (location->offset > input.size() || location->size > input.size() - location->offset) {
        return std::unexpected(
            failure("central directory range exceeds archive", location->offset));
    }
    if (location->entries > 1'000'000) {
        return std::unexpected(
            error{errc::unsupported, "zip entry count exceeds codec limit", "zip"});
    }
    auto output = std::vector<central_entry>{};
    output.reserve(static_cast<std::size_t>(location->entries));
    auto position = static_cast<std::size_t>(location->offset);
    const auto end = position + static_cast<std::size_t>(location->size);
    for (auto index = std::uint64_t{}; index < location->entries; ++index) {
        auto signature = signature_at(input, position, central_signature);
        if (!signature) {
            return std::unexpected(signature.error());
        }
        auto flags = little<std::uint16_t>(input, position + 8);
        auto method = little<std::uint16_t>(input, position + 10);
        auto time = little<std::uint16_t>(input, position + 12);
        auto date = little<std::uint16_t>(input, position + 14);
        auto crc = little<std::uint32_t>(input, position + 16);
        auto compressed = little<std::uint32_t>(input, position + 20);
        auto size = little<std::uint32_t>(input, position + 24);
        auto name_size = little<std::uint16_t>(input, position + 28);
        auto extra_size = little<std::uint16_t>(input, position + 30);
        auto comment_size = little<std::uint16_t>(input, position + 32);
        auto external = little<std::uint32_t>(input, position + 38);
        auto local_offset = little<std::uint32_t>(input, position + 42);
        if (!flags || !method || !time || !date || !crc || !compressed || !size || !name_size ||
            !extra_size || !comment_size || !external || !local_offset) {
            return std::unexpected(failure("truncated central directory entry", position));
        }
        const auto total = std::uint64_t{46} + *name_size + *extra_size + *comment_size;
        if (position > end || total > end - position) {
            return std::unexpected(failure("central directory entry exceeds directory", position));
        }
        const auto name_begin = position + 46;
        const auto* characters = reinterpret_cast<const char*>(input.data() + name_begin);
        auto entry = central_entry{.name = std::string{characters, *name_size},
                                   .flags = *flags,
                                   .method = *method,
                                   .time = *time,
                                   .date = *date,
                                   .crc = *crc,
                                   .external_attributes = *external,
                                   .compressed_size = *compressed,
                                   .uncompressed_size = *size,
                                   .local_offset = *local_offset};
        const auto extra = input.subspan(name_begin + *name_size, *extra_size);
        auto decoded_zip64 = zip64_extra(extra, *size, *compressed, *local_offset, entry);
        if (!decoded_zip64) {
            return std::unexpected(decoded_zip64.error());
        }
        output.push_back(std::move(entry));
        position += static_cast<std::size_t>(total);
    }
    if (position != end) {
        return std::unexpected(failure("central directory size mismatch", position));
    }
    return output;
}

auto inflate_raw(std::span<const std::byte> input, std::uint64_t expected)
    -> result<std::vector<std::byte>> {
    if (expected > std::numeric_limits<std::size_t>::max() ||
        input.size() > std::numeric_limits<uInt>::max() ||
        expected > std::numeric_limits<uInt>::max()) {
        return std::unexpected(error{errc::unsupported, "zip member exceeds zlib limits", "zip"});
    }
    auto output = std::vector<std::byte>(static_cast<std::size_t>(expected));
    auto stream = z_stream{};
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        return std::unexpected(error{errc::internal, "cannot initialize zip inflater", "zip"});
    }
    const auto status = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);
    if (status != Z_STREAM_END || stream.total_out != expected || stream.total_in != input.size()) {
        return std::unexpected(failure("invalid deflate stream"));
    }
    return output;
}

auto member_bytes(std::span<const std::byte> input, const central_entry& entry)
    -> result<std::pair<std::vector<std::byte>, format_byte_range>> {
    auto signature =
        signature_at(input, static_cast<std::size_t>(entry.local_offset), local_signature);
    if (!signature) {
        return std::unexpected(signature.error());
    }
    auto local_name_size = little<std::uint16_t>(input, entry.local_offset + 26);
    auto local_extra_size = little<std::uint16_t>(input, entry.local_offset + 28);
    if (!local_name_size || !local_extra_size) {
        return std::unexpected(failure("truncated local header", entry.local_offset));
    }
    const auto data_offset = entry.local_offset + 30 + *local_name_size + *local_extra_size;
    auto compressed = slice(input, data_offset, entry.compressed_size);
    if (!compressed) {
        return std::unexpected(compressed.error());
    }
    auto output = std::vector<std::byte>{};
    if (entry.method == 0) {
        output.assign(compressed->begin(), compressed->end());
    } else if (entry.method == 8) {
        auto inflated = inflate_raw(*compressed, entry.uncompressed_size);
        if (!inflated) {
            return std::unexpected(inflated.error());
        }
        output = std::move(*inflated);
    } else {
        return std::unexpected(
            error{errc::unsupported,
                  "zip compression method " + std::to_string(entry.method) + " is unsupported",
                  entry.name});
    }
    if (output.size() != entry.uncompressed_size) {
        return std::unexpected(failure("zip member size mismatch", data_offset));
    }
    const auto crc =
        crc32(0, reinterpret_cast<const Bytef*>(output.data()), static_cast<uInt>(output.size()));
    if (crc != entry.crc) {
        return std::unexpected(failure("zip member CRC mismatch", data_offset));
    }
    return std::pair{std::move(output), format_byte_range{data_offset, entry.compressed_size}};
}

class zip_backend final : public codec_backend {
public:
    auto recognise(std::span<const std::byte> blob, std::string_view)
        -> result<std::optional<format_recognition>> override {
        if (blob.size() < 4) {
            return std::optional<format_recognition>{};
        }
        auto signature = little<std::uint32_t>(blob, 0);
        if (!signature || (*signature != local_signature && *signature != eocd_signature)) {
            return std::optional<format_recognition>{};
        }
        auto entries = central_entries(blob);
        if (!entries) {
            return std::unexpected(entries.error());
        }
        const auto stored = std::ranges::all_of(
            *entries, [](const central_entry& entry) { return entry.method == 0; });
        return std::optional<format_recognition>{format_recognition{
            stored ? format_tier::canonical_round_trip : format_tier::semantic_verbatim,
            stored ? "all members are STORED" : "DEFLATE compressor settings cannot be recovered"}};
    }

    auto unpack(std::span<const std::byte> blob, format_limits limits)
        -> result<format_tree> override {
        auto central = central_entries(blob);
        if (!central) {
            return std::unexpected(central.error());
        }
        auto output = format_tree{.codec = "zip", .original = {blob.begin(), blob.end()}};
        output.tier = std::ranges::all_of(
                          *central, [](const central_entry& entry) { return entry.method == 0; })
                          ? format_tier::canonical_round_trip
                          : format_tier::semantic_verbatim;
        if (central->size() > limits.maximum_entries) {
            return std::unexpected(error{errc::unsupported, "zip entry limit exceeded", "zip"});
        }
        auto remaining = limits.byte_budget;
        for (const auto& source : *central) {
            if ((source.flags & 0x0001U) != 0) {
                return std::unexpected(
                    error{errc::unsupported, "encrypted zip is unsupported", source.name});
            }
            if (source.uncompressed_size > remaining) {
                output.truncated = true;
                output.limits.push_back("byte budget reached at " + source.name);
                return output;
            }
            remaining -= source.uncompressed_size;
            auto contents = member_bytes(blob, source);
            if (!contents) {
                return std::unexpected(contents.error());
            }
            auto metadata =
                attributes({{"compression_method", std::to_string(source.method)},
                            {"crc32", std::to_string(source.crc)},
                            {"external_attributes", std::to_string(source.external_attributes)},
                            {"extra_timestamps", "declared volatile"},
                            {"member_mtime",
                             std::to_string(source.date) + ":" + std::to_string(source.time)}});
            if (!metadata) {
                return std::unexpected(metadata.error());
            }
            const auto kind = source.name.ends_with('/') ? format_entry_kind::directory
                                                         : format_entry_kind::member;
            auto entry = make_entry(source.name,
                                    kind,
                                    std::move(contents->first),
                                    std::move(*metadata),
                                    contents->second);
            if (!entry) {
                return std::unexpected(entry.error());
            }
            output.entries.push_back(std::move(*entry));
        }
        return output;
    }

    auto repack(const format_tree& tree) -> result<std::vector<std::byte>> override {
        if (tree.tier != format_tier::canonical_round_trip || tree.original.empty()) {
            return std::unexpected(
                error{errc::unsupported, "zip repack requires a Tier 1 unchanged archive", "zip"});
        }
        return tree.original;
    }

    auto volatile_fields() const -> std::span<const std::string_view> override {
        return volatile_fields_;
    }

private:
    static constexpr auto volatile_fields_ =
        std::array<std::string_view, 3>{"member_mtime", "external_attributes", "extra_timestamps"};
};

constexpr auto extensions = std::array<const char*, 4>{".zip", ".jar", ".docx", ".odt"};
constexpr auto magic_bytes = std::array<std::uint8_t, 4>{0x50, 0x4b, 0x03, 0x04};
constexpr auto empty_magic_bytes = std::array<std::uint8_t, 4>{0x50, 0x4b, 0x05, 0x06};
constexpr auto magic = std::array<liquid_format_magic_v1, 2>{
    liquid_format_magic_v1{magic_bytes.data(), magic_bytes.size()},
    liquid_format_magic_v1{empty_magic_bytes.data(), empty_magic_bytes.size()}};
constexpr auto volatile_names =
    std::array<const char*, 3>{"member_mtime", "external_attributes", "extra_timestamps"};

} // namespace

auto zip_module() -> const liquid_format_module_v1* {
    static auto backend = zip_backend{};
    static const auto module =
        liquid_format_module_v1{.abi_version = LIQUID_FORMAT_ABI_VERSION,
                                .struct_size = sizeof(liquid_format_module_v1),
                                .descriptor = {.name = "zip",
                                               .tier = 1,
                                               .extensions = extensions.data(),
                                               .extension_count = extensions.size(),
                                               .magic = magic.data(),
                                               .magic_count = magic.size(),
                                               .volatile_fields = volatile_names.data(),
                                               .volatile_field_count = volatile_names.size(),
                                               .limits = {.maximum_depth = 8,
                                                          .byte_budget = 256ULL * 1024ULL * 1024ULL,
                                                          .maximum_entries = 100'000}},
                                .module_context = &backend,
                                .recognise = recognise_operation,
                                .unpack = unpack_operation,
                                .canonicalise = canonicalise_operation,
                                .diff = diff_operation,
                                .merge = merge_operation,
                                .repack = repack_operation,
                                .destroy = nullptr};
    return &module;
}

} // namespace liquid::formats::detail
