#include "formats/builtin.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <limits>
#include <map>
#include <string>
#include <utility>

#include <zlib.h>
#include <zstd.h>

namespace liquid::formats::detail {
namespace {

constexpr auto block_size = std::size_t{512};

auto failure(std::string message, std::size_t offset = 0) -> error {
    return {errc::corrupt, std::move(message), "tar byte " + std::to_string(offset)};
}

auto zero_block(std::span<const std::byte> block) -> bool {
    return std::ranges::all_of(block, [](std::byte value) { return value == std::byte{}; });
}

auto text_field(std::span<const std::byte> header, std::size_t offset, std::size_t length)
    -> std::string {
    const auto* begin = reinterpret_cast<const char*>(header.data() + offset);
    auto value = std::string{begin, length};
    const auto end = value.find('\0');
    if (end != std::string::npos) {
        value.resize(end);
    }
    return value;
}

auto tar_number(std::span<const std::byte> header, std::size_t offset, std::size_t length)
    -> result<std::uint64_t> {
    if (offset > header.size() || length > header.size() - offset || length == 0) {
        return std::unexpected(failure("truncated numeric field", offset));
    }
    if ((std::to_integer<std::uint8_t>(header[offset]) & 0x80U) != 0) {
        auto output = std::uint64_t{};
        for (auto index = std::size_t{}; index < length; ++index) {
            auto value = std::to_integer<std::uint8_t>(header[offset + index]);
            if (index == 0) {
                value &= 0x7fU;
            }
            if (output > (std::numeric_limits<std::uint64_t>::max() >> 8U)) {
                return std::unexpected(failure("tar numeric field overflows", offset));
            }
            output = (output << 8U) | value;
        }
        return output;
    }
    auto output = std::uint64_t{};
    auto seen = false;
    for (auto index = std::size_t{}; index < length; ++index) {
        const auto value = std::to_integer<unsigned char>(header[offset + index]);
        if (value == 0 || value == ' ') {
            if (seen) {
                break;
            }
            continue;
        }
        if (value < '0' || value > '7') {
            return std::unexpected(failure("invalid octal field", offset + index));
        }
        seen = true;
        if (output > (std::numeric_limits<std::uint64_t>::max() - 7) / 8) {
            return std::unexpected(failure("tar numeric field overflows", offset));
        }
        output = output * 8 + value - '0';
    }
    return output;
}

auto valid_checksum(std::span<const std::byte> header) -> result<void> {
    auto expected = tar_number(header, 148, 8);
    if (!expected) {
        return std::unexpected(expected.error());
    }
    auto sum = std::uint64_t{};
    for (auto index = std::size_t{}; index < header.size(); ++index) {
        sum += index >= 148 && index < 156 ? static_cast<unsigned char>(' ')
                                           : std::to_integer<unsigned char>(header[index]);
    }
    if (sum != *expected) {
        return std::unexpected(failure("header checksum mismatch"));
    }
    return {};
}

auto rounded_blocks(std::uint64_t size) -> result<std::uint64_t> {
    if (size > std::numeric_limits<std::uint64_t>::max() - (block_size - 1)) {
        return std::unexpected(failure("member size overflows"));
    }
    return (size + block_size - 1) / block_size * block_size;
}

auto pax_fields(std::span<const std::byte> input) -> result<std::map<std::string, std::string>> {
    auto output = std::map<std::string, std::string>{};
    auto offset = std::size_t{};
    while (offset < input.size()) {
        const auto* begin = reinterpret_cast<const char*>(input.data() + offset);
        const auto available = std::string_view{begin, input.size() - offset};
        const auto space = available.find(' ');
        if (space == std::string_view::npos) {
            return std::unexpected(failure("invalid pax record", offset));
        }
        auto length = std::size_t{};
        const auto [end, code] =
            std::from_chars(available.data(), available.data() + space, length);
        if (code != std::errc{} || end != available.data() + space || length == 0 ||
            length > available.size()) {
            return std::unexpected(failure("invalid pax record length", offset));
        }
        const auto record = available.substr(space + 1, length - space - 1);
        if (record.empty() || record.back() != '\n') {
            return std::unexpected(failure("unterminated pax record", offset));
        }
        const auto equals = record.find('=');
        if (equals == std::string_view::npos) {
            return std::unexpected(failure("invalid pax field", offset));
        }
        output[std::string{record.substr(0, equals)}] =
            std::string{record.substr(equals + 1, record.size() - equals - 2)};
        offset += length;
    }
    return output;
}

auto decompress_gzip(std::span<const std::byte> input, std::uint64_t budget)
    -> result<std::vector<std::byte>> {
    if (input.size() > std::numeric_limits<uInt>::max()) {
        return std::unexpected(error{errc::unsupported, "gzip tar exceeds zlib limits", "tar"});
    }
    auto stream = z_stream{};
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
        return std::unexpected(error{errc::internal, "cannot initialize gzip decoder", "tar"});
    }
    auto output = std::vector<std::byte>{};
    auto chunk = std::array<std::byte, 64 * 1024>{};
    auto status = Z_OK;
    while (status == Z_OK) {
        stream.next_out = reinterpret_cast<Bytef*>(chunk.data());
        stream.avail_out = static_cast<uInt>(chunk.size());
        status = inflate(&stream, Z_NO_FLUSH);
        const auto produced = chunk.size() - stream.avail_out;
        if (produced > budget - std::min<std::uint64_t>(budget, output.size())) {
            inflateEnd(&stream);
            return std::unexpected(error{errc::unsupported, "gzip tar exceeds byte budget", "tar"});
        }
        output.insert(output.end(), chunk.begin(), chunk.begin() + produced);
    }
    inflateEnd(&stream);
    if (status != Z_STREAM_END) {
        return std::unexpected(failure("invalid gzip wrapper"));
    }
    return output;
}

auto decompress_zstd(std::span<const std::byte> input, std::uint64_t budget)
    -> result<std::vector<std::byte>> {
    const auto size = ZSTD_getFrameContentSize(input.data(), input.size());
    if (size == ZSTD_CONTENTSIZE_ERROR) {
        return std::unexpected(failure("invalid zstd wrapper"));
    }
    if (size == ZSTD_CONTENTSIZE_UNKNOWN) {
        return std::unexpected(
            error{errc::unsupported, "zstd tar has no declared content size", "tar"});
    }
    if (size > budget || size > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected(error{errc::unsupported, "zstd tar exceeds byte budget", "tar"});
    }
    auto output = std::vector<std::byte>(static_cast<std::size_t>(size));
    const auto produced = ZSTD_decompress(output.data(), output.size(), input.data(), input.size());
    if (ZSTD_isError(produced) || produced != output.size()) {
        return std::unexpected(failure("invalid zstd wrapper"));
    }
    return output;
}

enum class wrapper { none, gzip, zstd };

auto unwrap(std::span<const std::byte> input, std::uint64_t budget)
    -> result<std::pair<std::vector<std::byte>, wrapper>> {
    if (input.size() >= 2 && input[0] == std::byte{0x1f} && input[1] == std::byte{0x8b}) {
        auto output = decompress_gzip(input, budget);
        if (!output) {
            return std::unexpected(output.error());
        }
        return std::pair{std::move(*output), wrapper::gzip};
    }
    if (input.size() >= 4 && input[0] == std::byte{0x28} && input[1] == std::byte{0xb5} &&
        input[2] == std::byte{0x2f} && input[3] == std::byte{0xfd}) {
        auto output = decompress_zstd(input, budget);
        if (!output) {
            return std::unexpected(output.error());
        }
        return std::pair{std::move(*output), wrapper::zstd};
    }
    if (input.size() > budget) {
        return std::unexpected(
            error{errc::unsupported, "uncompressed tar exceeds byte budget", "tar"});
    }
    return std::pair{std::vector<std::byte>{input.begin(), input.end()}, wrapper::none};
}

auto parse_tar(std::span<const std::byte> archive,
               std::span<const std::byte> original,
               format_tier tier,
               format_limits limits) -> result<format_tree> {
    auto output =
        format_tree{.codec = "tar", .tier = tier, .original = {original.begin(), original.end()}};
    auto offset = std::size_t{};
    auto long_name = std::string{};
    auto long_link = std::string{};
    auto global_pax = std::map<std::string, std::string>{};
    auto local_pax = std::map<std::string, std::string>{};
    while (offset < archive.size()) {
        if (archive.size() - offset < block_size) {
            return std::unexpected(failure("truncated header", offset));
        }
        const auto header = archive.subspan(offset, block_size);
        if (zero_block(header)) {
            if (archive.size() - offset < block_size * 2 ||
                !zero_block(archive.subspan(offset + block_size, block_size))) {
                return std::unexpected(failure("single zero end block", offset));
            }
            return output;
        }
        auto checksum = valid_checksum(header);
        if (!checksum) {
            return std::unexpected(checksum.error());
        }
        auto size = tar_number(header, 124, 12);
        auto mode = tar_number(header, 100, 8);
        auto uid = tar_number(header, 108, 8);
        auto gid = tar_number(header, 116, 8);
        auto mtime = tar_number(header, 136, 12);
        if (!size || !mode || !uid || !gid || !mtime) {
            return std::unexpected(failure("invalid tar header", offset));
        }
        auto padded = rounded_blocks(*size);
        if (!padded || *padded > archive.size() - offset - block_size) {
            return std::unexpected(failure("truncated member", offset));
        }
        const auto body = archive.subspan(offset + block_size, static_cast<std::size_t>(*size));
        auto name = text_field(header, 0, 100);
        const auto prefix = text_field(header, 345, 155);
        if (!prefix.empty()) {
            name = prefix + "/" + name;
        }
        auto link = text_field(header, 157, 100);
        const auto type = static_cast<char>(std::to_integer<unsigned char>(header[156]));
        if (type == 'L' || type == 'K') {
            auto value = std::string{reinterpret_cast<const char*>(body.data()), body.size()};
            if (!value.empty() && value.back() == '\0') {
                value.pop_back();
            }
            if (type == 'L') {
                long_name = std::move(value);
            } else {
                long_link = std::move(value);
            }
            offset += block_size + static_cast<std::size_t>(*padded);
            continue;
        }
        if (type == 'x' || type == 'g') {
            auto parsed = pax_fields(body);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            if (type == 'g') {
                global_pax.insert(parsed->begin(), parsed->end());
            } else {
                local_pax = std::move(*parsed);
            }
            offset += block_size + static_cast<std::size_t>(*padded);
            continue;
        }
        auto effective = global_pax;
        effective.insert(local_pax.begin(), local_pax.end());
        if (!long_name.empty()) {
            name = std::move(long_name);
            long_name.clear();
        }
        if (!long_link.empty()) {
            link = std::move(long_link);
            long_link.clear();
        }
        if (effective.contains("path")) {
            name = effective.at("path");
        }
        if (effective.contains("linkpath")) {
            link = effective.at("linkpath");
        }
        local_pax.clear();
        if (output.entries.size() >= limits.maximum_entries) {
            output.truncated = true;
            output.limits.push_back("entry limit reached at " + name);
            return output;
        }
        auto contents = std::vector<std::byte>{};
        auto kind = format_entry_kind::member;
        if (type == '0' || type == '\0' || type == '7') {
            contents.assign(body.begin(), body.end());
        } else if (type == '5') {
            kind = format_entry_kind::directory;
        }
        auto metadata = attributes(
            {{"gid", std::to_string(*gid)},
             {"link", link},
             {"mode", std::to_string(*mode)},
             {"mtime",
              effective.contains("mtime") ? effective.at("mtime") : std::to_string(*mtime)},
             {"type", std::string{type}},
             {"uid", std::to_string(*uid)}});
        if (!metadata) {
            return std::unexpected(metadata.error());
        }
        auto entry = make_entry(std::move(name),
                                kind,
                                std::move(contents),
                                std::move(*metadata),
                                format_byte_range{offset + block_size, *size});
        if (!entry) {
            return std::unexpected(entry.error());
        }
        output.entries.push_back(std::move(*entry));
        offset += block_size + static_cast<std::size_t>(*padded);
    }
    return std::unexpected(failure("end blocks are absent", offset));
}

void write_text(std::span<std::byte> field, std::string_view value) {
    const auto length = std::min(field.size(), value.size());
    for (auto index = std::size_t{}; index < length; ++index) {
        field[index] = std::byte{static_cast<unsigned char>(value[index])};
    }
}

auto write_octal(std::span<std::byte> field, std::uint64_t value) -> result<void> {
    auto text = std::array<char, 32>{};
    const auto [end, code] = std::to_chars(text.data(), text.data() + text.size(), value, 8);
    if (code != std::errc{} || static_cast<std::size_t>(end - text.data()) + 1 > field.size()) {
        return std::unexpected(error{errc::unsupported, "tar numeric field is too large", "tar"});
    }
    std::ranges::fill(field, std::byte{'0'});
    const auto digits = static_cast<std::size_t>(end - text.data());
    for (auto index = std::size_t{}; index < digits; ++index) {
        field[field.size() - digits - 1 + index] =
            std::byte{static_cast<unsigned char>(text[index])};
    }
    field.back() = std::byte{};
    return {};
}

auto source_date_epoch() -> std::uint64_t {
    const auto* value = std::getenv("SOURCE_DATE_EPOCH");
    if (!value) {
        return 0;
    }
    auto output = std::uint64_t{};
    const auto text = std::string_view{value};
    const auto [end, code] = std::from_chars(text.data(), text.data() + text.size(), output);
    return code == std::errc{} && end == text.data() + text.size() ? output : 0;
}

auto pax_record(std::string_view key, std::string_view value) -> std::string {
    auto length = key.size() + value.size() + 3;
    while (true) {
        const auto digits = std::to_string(length).size();
        const auto actual = digits + 1 + key.size() + 1 + value.size() + 1;
        if (actual == length) {
            return std::to_string(length) + " " + std::string{key} + "=" + std::string{value} +
                   "\n";
        }
        length = actual;
    }
}

auto append_member(std::vector<std::byte>& output,
                   std::string_view name,
                   char type,
                   std::span<const std::byte> contents,
                   std::uint64_t mode,
                   std::uint64_t mtime) -> result<void> {
    auto header = std::array<std::byte, block_size>{};
    write_text(std::span{header}.subspan(0, 100), name);
    auto mode_result = write_octal(std::span{header}.subspan(100, 8), mode);
    auto uid_result = write_octal(std::span{header}.subspan(108, 8), 0);
    auto gid_result = write_octal(std::span{header}.subspan(116, 8), 0);
    auto size_result = write_octal(std::span{header}.subspan(124, 12), contents.size());
    auto time_result = write_octal(std::span{header}.subspan(136, 12), mtime);
    if (!mode_result || !uid_result || !gid_result || !size_result || !time_result) {
        return std::unexpected(
            error{errc::unsupported, "tar header value is unsupported", std::string{name}});
    }
    std::ranges::fill(std::span{header}.subspan(148, 8), std::byte{' '});
    header[156] = std::byte{static_cast<unsigned char>(type)};
    write_text(std::span{header}.subspan(257, 6), "ustar");
    write_text(std::span{header}.subspan(263, 2), "00");
    auto checksum = std::uint64_t{};
    for (const auto value : header) {
        checksum += std::to_integer<unsigned char>(value);
    }
    auto checksum_result = write_octal(std::span{header}.subspan(148, 7), checksum);
    if (!checksum_result) {
        return std::unexpected(checksum_result.error());
    }
    header[154] = std::byte{};
    header[155] = std::byte{' '};
    output.insert(output.end(), header.begin(), header.end());
    output.insert(output.end(), contents.begin(), contents.end());
    const auto remainder = contents.size() % block_size;
    if (remainder != 0) {
        output.resize(output.size() + block_size - remainder);
    }
    return {};
}

auto deterministic_tar(const format_tree& tree) -> result<std::vector<std::byte>> {
    auto entries = tree.entries;
    std::ranges::sort(entries, {}, &format_entry::path);
    auto output = std::vector<std::byte>{};
    const auto mtime = source_date_epoch();
    for (const auto& entry : entries) {
        auto name = entry.path;
        const auto directory = entry.kind == format_entry_kind::directory;
        if (directory && !name.ends_with('/')) {
            name.push_back('/');
        }
        if (name.size() > 100) {
            const auto record = pax_record("path", name);
            const auto record_bytes =
                std::span{reinterpret_cast<const std::byte*>(record.data()), record.size()};
            auto pax = append_member(output, "PaxHeaders/liquid", 'x', record_bytes, 0644, mtime);
            if (!pax) {
                return std::unexpected(pax.error());
            }
        }
        const auto header_name = name.size() <= 100 ? name : name.substr(0, 100);
        const auto mode = directory ? 0755U : 0644U;
        auto appended = append_member(output,
                                      header_name,
                                      directory ? '5' : '0',
                                      directory ? std::span<const std::byte>{}
                                                : std::span<const std::byte>{entry.bytes},
                                      mode,
                                      mtime);
        if (!appended) {
            return std::unexpected(appended.error());
        }
    }
    output.resize(output.size() + block_size * 2);
    return output;
}

class tar_backend final : public codec_backend {
public:
    auto recognise(std::span<const std::byte> blob, std::string_view)
        -> result<std::optional<format_recognition>> override {
        if (blob.size() >= 2 && blob[0] == std::byte{0x1f} && blob[1] == std::byte{0x8b}) {
            return std::optional<format_recognition>{format_recognition{
                format_tier::semantic_verbatim, "gzip compressor settings cannot be recovered"}};
        }
        if (blob.size() >= 4 && blob[0] == std::byte{0x28} && blob[1] == std::byte{0xb5} &&
            blob[2] == std::byte{0x2f} && blob[3] == std::byte{0xfd}) {
            return std::optional<format_recognition>{format_recognition{
                format_tier::semantic_verbatim, "zstd compressor settings cannot be recovered"}};
        }
        if (blob.size() < block_size) {
            return std::optional<format_recognition>{};
        }
        const auto ustar = text_field(blob.first(block_size), 257, 6).starts_with("ustar");
        auto checksum = valid_checksum(blob.first(block_size));
        if (!ustar && !checksum) {
            return std::optional<format_recognition>{};
        }
        if (!checksum) {
            return std::unexpected(checksum.error());
        }
        return std::optional<format_recognition>{
            format_recognition{format_tier::canonical_round_trip, "uncompressed tar"}};
    }

    auto unpack(std::span<const std::byte> blob, format_limits limits)
        -> result<format_tree> override {
        auto unwrapped = unwrap(blob, limits.byte_budget);
        if (!unwrapped) {
            return std::unexpected(unwrapped.error());
        }
        const auto tier = unwrapped->second == wrapper::none ? format_tier::canonical_round_trip
                                                             : format_tier::semantic_verbatim;
        return parse_tar(unwrapped->first, blob, tier, limits);
    }

    auto repack(const format_tree& tree) -> result<std::vector<std::byte>> override {
        if (tree.tier != format_tier::canonical_round_trip) {
            return std::unexpected(
                error{errc::unsupported, "compressed tar repack is unsupported", "tar"});
        }
        if (!tree.original.empty()) {
            return tree.original;
        }
        return deterministic_tar(tree);
    }

    auto volatile_fields() const -> std::span<const std::string_view> override {
        return volatile_fields_;
    }

private:
    static constexpr auto volatile_fields_ = std::array<std::string_view, 3>{"gid", "mtime", "uid"};
};

constexpr auto extensions =
    std::array<const char*, 6>{".tar", ".tar.gz", ".tgz", ".tar.zst", ".tzst", ".tar.zstd"};
constexpr auto gzip_magic_bytes = std::array<std::uint8_t, 2>{0x1f, 0x8b};
constexpr auto zstd_magic_bytes = std::array<std::uint8_t, 4>{0x28, 0xb5, 0x2f, 0xfd};
constexpr auto magic = std::array<liquid_format_magic_v1, 2>{
    liquid_format_magic_v1{gzip_magic_bytes.data(), gzip_magic_bytes.size()},
    liquid_format_magic_v1{zstd_magic_bytes.data(), zstd_magic_bytes.size()}};
constexpr auto volatile_names = std::array<const char*, 3>{"gid", "mtime", "uid"};

} // namespace

auto tar_module() -> const liquid_format_module_v1* {
    static auto backend = tar_backend{};
    static const auto module =
        liquid_format_module_v1{.abi_version = LIQUID_FORMAT_ABI_VERSION,
                                .struct_size = sizeof(liquid_format_module_v1),
                                .descriptor = {.name = "tar",
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
