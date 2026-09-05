#include "formats/builtin.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace liquid::formats::detail {
namespace {

constexpr auto signature = std::string_view{"SQLite format 3\0", 16};

auto failure(std::string message, std::uint64_t page = 0) -> error {
    return {errc::corrupt,
            std::move(message),
            page == 0 ? "sqlite" : "sqlite page " + std::to_string(page)};
}

template <class Integer>
auto big(std::span<const std::byte> input, std::size_t offset) -> result<Integer> {
    if (offset > input.size() || input.size() - offset < sizeof(Integer)) {
        return std::unexpected(failure("truncated integer"));
    }
    auto output = Integer{};
    for (auto index = std::size_t{}; index < sizeof(Integer); ++index) {
        output = static_cast<Integer>((output << 8U) |
                                      std::to_integer<std::uint8_t>(input[offset + index]));
    }
    return output;
}

auto varint(std::span<const std::byte> input, std::size_t& offset) -> result<std::uint64_t> {
    auto output = std::uint64_t{};
    for (auto index = 0; index < 9; ++index) {
        if (offset >= input.size()) {
            return std::unexpected(failure("truncated varint"));
        }
        const auto value = std::to_integer<std::uint8_t>(input[offset++]);
        if (index == 8) {
            output = (output << 8U) | value;
            return output;
        }
        output = (output << 7U) | (value & 0x7fU);
        if ((value & 0x80U) == 0) {
            return output;
        }
    }
    return std::unexpected(failure("invalid varint"));
}

struct value {
    std::string text;
    bool null{};
};

auto signed_integer(std::span<const std::byte> input, std::size_t offset, std::size_t size)
    -> result<std::int64_t> {
    if (size == 0 || size > 8 || offset > input.size() || size > input.size() - offset) {
        return std::unexpected(failure("truncated record integer"));
    }
    auto raw = std::uint64_t{};
    for (auto index = std::size_t{}; index < size; ++index) {
        raw = (raw << 8U) | std::to_integer<std::uint8_t>(input[offset + index]);
    }
    if (size < 8 && (raw & (std::uint64_t{1} << (size * 8U - 1U))) != 0) {
        raw |= std::numeric_limits<std::uint64_t>::max() << (size * 8U);
    }
    return std::bit_cast<std::int64_t>(raw);
}

auto hex(std::span<const std::byte> input) -> std::string {
    constexpr auto digits = std::string_view{"0123456789abcdef"};
    auto output = std::string{};
    output.reserve(input.size() * 2);
    for (const auto byte : input) {
        const auto value = std::to_integer<std::uint8_t>(byte);
        output.push_back(digits[value >> 4U]);
        output.push_back(digits[value & 0x0fU]);
    }
    return output;
}

auto record_values(std::span<const std::byte> payload) -> result<std::vector<value>> {
    auto offset = std::size_t{};
    auto header_size_value = varint(payload, offset);
    if (!header_size_value || *header_size_value < offset || *header_size_value > payload.size()) {
        return std::unexpected(failure("invalid record header"));
    }
    const auto header_size = static_cast<std::size_t>(*header_size_value);
    auto serials = std::vector<std::uint64_t>{};
    while (offset < header_size) {
        auto serial = varint(payload.first(header_size), offset);
        if (!serial) {
            return std::unexpected(serial.error());
        }
        serials.push_back(*serial);
    }
    auto body = header_size;
    auto output = std::vector<value>{};
    output.reserve(serials.size());
    for (const auto serial : serials) {
        if (serial == 0) {
            output.push_back({"NULL", true});
            continue;
        }
        if (serial == 8 || serial == 9) {
            output.push_back({serial == 8 ? "0" : "1", false});
            continue;
        }
        if (serial >= 1 && serial <= 6) {
            constexpr auto sizes = std::array<std::size_t, 6>{1, 2, 3, 4, 6, 8};
            const auto size = sizes[serial - 1];
            auto integer = signed_integer(payload, body, size);
            if (!integer) {
                return std::unexpected(integer.error());
            }
            body += size;
            output.push_back({std::to_string(*integer), false});
            continue;
        }
        if (serial == 7) {
            auto raw = big<std::uint64_t>(payload, body);
            if (!raw) {
                return std::unexpected(raw.error());
            }
            body += 8;
            output.push_back({std::to_string(std::bit_cast<double>(*raw)), false});
            continue;
        }
        if (serial == 10 || serial == 11) {
            return std::unexpected(failure("reserved record serial type"));
        }
        const auto size = static_cast<std::uint64_t>((serial - (serial % 2 == 0 ? 12 : 13)) / 2);
        if (size > payload.size() - std::min(payload.size(), body)) {
            return std::unexpected(failure("truncated record value"));
        }
        const auto raw = payload.subspan(body, static_cast<std::size_t>(size));
        body += static_cast<std::size_t>(size);
        if (serial % 2 == 0) {
            output.push_back({"x'" + hex(raw) + "'", false});
        } else {
            output.push_back(
                {std::string{reinterpret_cast<const char*>(raw.data()), raw.size()}, false});
        }
    }
    if (body != payload.size()) {
        return std::unexpected(failure("record payload size mismatch"));
    }
    return output;
}

auto join(std::span<const value> values) -> std::string {
    auto output = std::string{};
    for (auto index = std::size_t{}; index < values.size(); ++index) {
        if (index != 0) {
            output.push_back('|');
        }
        output += values[index].text;
    }
    return output;
}

struct row {
    std::uint64_t rowid{};
    std::vector<value> values;
};

class database {
public:
    database(std::span<const std::byte> bytes, format_limits limits)
        : bytes_(bytes), limits_(limits) {}

    auto open() -> result<void> {
        if (bytes_.size() < 100 || std::string_view{reinterpret_cast<const char*>(bytes_.data()),
                                                    signature.size()} != signature) {
            return std::unexpected(failure("invalid database header"));
        }
        auto size = big<std::uint16_t>(bytes_, 16);
        auto page_count = big<std::uint32_t>(bytes_, 28);
        auto encoding = big<std::uint32_t>(bytes_, 56);
        if (!size || !page_count || !encoding) {
            return std::unexpected(failure("truncated database header"));
        }
        page_size_ = *size == 1 ? 65'536 : *size;
        if (page_size_ < 512 || page_size_ > 65'536 || (page_size_ & (page_size_ - 1)) != 0) {
            return std::unexpected(failure("invalid page size"));
        }
        page_count_ = *page_count == 0 ? bytes_.size() / page_size_ : *page_count;
        if (page_count_ == 0 || page_count_ > bytes_.size() / page_size_) {
            return std::unexpected(failure("invalid page count"));
        }
        if (*encoding != 1) {
            return std::unexpected(
                error{errc::unsupported, "SQLite UTF-16 text encoding is unsupported", "sqlite"});
        }
        reserved_ = std::to_integer<std::uint8_t>(bytes_[20]);
        if (reserved_ >= page_size_) {
            return std::unexpected(failure("invalid reserved byte count"));
        }
        usable_ = page_size_ - reserved_;
        return {};
    }

    auto rows(std::uint32_t root) -> result<std::vector<row>> {
        visited_.clear();
        auto output = std::vector<row>{};
        auto traversed = traverse(root, output);
        if (!traversed) {
            return std::unexpected(traversed.error());
        }
        std::ranges::sort(output, {}, &row::rowid);
        return output;
    }

private:
    auto page(std::uint32_t number) const -> result<std::span<const std::byte>> {
        if (number == 0 || number > page_count_) {
            return std::unexpected(failure("page number is outside database", number));
        }
        const auto offset = static_cast<std::uint64_t>(number - 1) * page_size_;
        return bytes_.subspan(static_cast<std::size_t>(offset), page_size_);
    }

    auto payload(std::span<const std::byte> page_bytes,
                 std::size_t payload_offset,
                 std::uint64_t payload_size) -> result<std::vector<std::byte>> {
        const auto max_local = usable_ - 35;
        const auto min_local = ((usable_ - 12) * 32 / 255) - 23;
        auto local = payload_size;
        if (payload_size > max_local) {
            local = min_local + (payload_size - min_local) % (usable_ - 4);
            if (local > max_local) {
                local = min_local;
            }
        }
        if (payload_offset > page_bytes.size() || local > page_bytes.size() - payload_offset) {
            return std::unexpected(failure("truncated local payload"));
        }
        auto output = std::vector<std::byte>{page_bytes.begin() + payload_offset,
                                             page_bytes.begin() + payload_offset + local};
        if (local == payload_size) {
            return output;
        }
        auto overflow =
            big<std::uint32_t>(page_bytes, payload_offset + static_cast<std::size_t>(local));
        if (!overflow) {
            return std::unexpected(overflow.error());
        }
        auto seen = std::set<std::uint32_t>{};
        while (output.size() < payload_size) {
            if (*overflow == 0 || !seen.insert(*overflow).second) {
                return std::unexpected(failure("invalid overflow chain", *overflow));
            }
            auto overflow_page = page(*overflow);
            if (!overflow_page) {
                return std::unexpected(overflow_page.error());
            }
            auto next = big<std::uint32_t>(*overflow_page, 0);
            if (!next) {
                return std::unexpected(next.error());
            }
            const auto remaining = payload_size - output.size();
            const auto take = std::min<std::uint64_t>(remaining, usable_ - 4);
            output.insert(
                output.end(), overflow_page->begin() + 4, overflow_page->begin() + 4 + take);
            overflow = next;
        }
        return output;
    }

    auto leaf_cell(std::span<const std::byte> page_bytes, std::size_t offset) -> result<row> {
        auto cursor = offset;
        auto payload_size = varint(page_bytes, cursor);
        auto rowid = varint(page_bytes, cursor);
        if (!payload_size || !rowid) {
            return std::unexpected(failure("invalid table leaf cell"));
        }
        if (*payload_size > remaining_bytes_) {
            return std::unexpected(
                error{errc::unsupported, "SQLite byte budget reached", "sqlite"});
        }
        auto payload_bytes = payload(page_bytes, cursor, *payload_size);
        if (!payload_bytes) {
            return std::unexpected(payload_bytes.error());
        }
        auto values = record_values(*payload_bytes);
        if (!values) {
            return std::unexpected(values.error());
        }
        remaining_bytes_ -= *payload_size;
        return row{*rowid, std::move(*values)};
    }

    auto traverse(std::uint32_t number, std::vector<row>& output) -> result<void> {
        if (!visited_.insert(number).second) {
            return std::unexpected(failure("b-tree page cycle", number));
        }
        auto page_bytes = page(number);
        if (!page_bytes) {
            return std::unexpected(page_bytes.error());
        }
        const auto header = number == 1 ? std::size_t{100} : std::size_t{};
        if (header >= page_bytes->size()) {
            return std::unexpected(failure("b-tree header is absent", number));
        }
        const auto type = std::to_integer<std::uint8_t>((*page_bytes)[header]);
        if (type != 0x05 && type != 0x0d) {
            return std::unexpected(error{errc::unsupported,
                                         "only table b-tree pages are supported",
                                         "sqlite page " + std::to_string(number)});
        }
        auto cells = big<std::uint16_t>(*page_bytes, header + 3);
        if (!cells || *cells > limits_.maximum_entries) {
            return std::unexpected(failure("invalid b-tree cell count", number));
        }
        const auto header_size = type == 0x05 ? 12U : 8U;
        if (header + header_size + static_cast<std::size_t>(*cells) * 2 > page_bytes->size()) {
            return std::unexpected(failure("truncated cell pointer array", number));
        }
        for (auto index = std::uint16_t{}; index < *cells; ++index) {
            auto cell_offset = big<std::uint16_t>(*page_bytes, header + header_size + index * 2);
            if (!cell_offset || *cell_offset >= page_bytes->size()) {
                return std::unexpected(failure("invalid cell offset", number));
            }
            if (type == 0x05) {
                auto child = big<std::uint32_t>(*page_bytes, *cell_offset);
                if (!child) {
                    return std::unexpected(child.error());
                }
                auto traversed = traverse(*child, output);
                if (!traversed) {
                    return traversed;
                }
            } else {
                auto decoded = leaf_cell(*page_bytes, *cell_offset);
                if (!decoded) {
                    return std::unexpected(decoded.error());
                }
                output.push_back(std::move(*decoded));
                if (output.size() > limits_.maximum_entries) {
                    return std::unexpected(
                        error{errc::unsupported, "SQLite row limit exceeded", "sqlite"});
                }
            }
        }
        if (type == 0x05) {
            auto right = big<std::uint32_t>(*page_bytes, header + 8);
            if (!right) {
                return std::unexpected(right.error());
            }
            return traverse(*right, output);
        }
        return {};
    }

    std::span<const std::byte> bytes_;
    format_limits limits_;
    std::uint32_t page_size_{};
    std::uint32_t usable_{};
    std::uint32_t page_count_{};
    std::uint8_t reserved_{};
    std::uint64_t remaining_bytes_{limits_.byte_budget};
    std::set<std::uint32_t> visited_;
};

struct schema_entry {
    std::string type;
    std::string name;
    std::string table;
    std::uint32_t root{};
    std::string sql;
};

auto schemas(std::span<const row> rows) -> result<std::vector<schema_entry>> {
    auto output = std::vector<schema_entry>{};
    for (const auto& row : rows) {
        if (row.values.size() < 5) {
            return std::unexpected(failure("invalid sqlite_schema record"));
        }
        auto root = std::uint32_t{};
        const auto text = std::string_view{row.values[3].text};
        const auto [end, code] = std::from_chars(text.data(), text.data() + text.size(), root);
        if (code != std::errc{} || end != text.data() + text.size()) {
            return std::unexpected(failure("invalid sqlite_schema root page"));
        }
        output.push_back({row.values[0].text,
                          row.values[1].text,
                          row.values[2].text,
                          root,
                          row.values[4].null ? "" : row.values[4].text});
    }
    return output;
}

class sqlite_backend final : public codec_backend {
public:
    auto recognise(std::span<const std::byte> blob, std::string_view)
        -> result<std::optional<format_recognition>> override {
        if (blob.size() < signature.size()) {
            return std::optional<format_recognition>{};
        }
        const auto header =
            std::string_view{reinterpret_cast<const char*>(blob.data()), signature.size()};
        if (header != signature) {
            return std::optional<format_recognition>{};
        }
        if (blob.size() < 100) {
            return std::unexpected(failure("truncated database header"));
        }
        return std::optional<format_recognition>{format_recognition{
            format_tier::semantic_verbatim, "page allocation and freelist are not canonical"}};
    }

    auto unpack(std::span<const std::byte> blob, format_limits limits)
        -> result<format_tree> override {
        auto source = database{blob, limits};
        auto opened = source.open();
        if (!opened) {
            return std::unexpected(opened.error());
        }
        auto schema_rows = source.rows(1);
        if (!schema_rows) {
            return std::unexpected(schema_rows.error());
        }
        auto schema = schemas(*schema_rows);
        if (!schema) {
            return std::unexpected(schema.error());
        }
        auto output = format_tree{.codec = "sqlite",
                                  .tier = format_tier::semantic_verbatim,
                                  .original = {blob.begin(), blob.end()}};
        for (const auto& item : *schema) {
            if (item.type != "table" || item.name.starts_with("sqlite_")) {
                continue;
            }
            auto metadata = attributes({{"schema", item.sql}});
            if (!metadata) {
                return std::unexpected(metadata.error());
            }
            auto table_entry = make_entry(
                item.name,
                format_entry_kind::table,
                std::vector<std::byte>{reinterpret_cast<const std::byte*>(item.sql.data()),
                                       reinterpret_cast<const std::byte*>(item.sql.data()) +
                                           item.sql.size()},
                std::move(*metadata));
            if (!table_entry) {
                return std::unexpected(table_entry.error());
            }
            output.entries.push_back(std::move(*table_entry));
            if (item.root == 0) {
                continue;
            }
            auto rows = source.rows(item.root);
            if (!rows) {
                return std::unexpected(rows.error());
            }
            for (auto& row : *rows) {
                const auto value = join(row.values);
                auto row_entry = make_entry(
                    item.name + "/rowid=" + std::to_string(row.rowid),
                    format_entry_kind::member,
                    std::vector<std::byte>{reinterpret_cast<const std::byte*>(value.data()),
                                           reinterpret_cast<const std::byte*>(value.data()) +
                                               value.size()});
                if (!row_entry) {
                    return std::unexpected(row_entry.error());
                }
                output.entries.push_back(std::move(*row_entry));
            }
        }
        return output;
    }

    auto volatile_fields() const -> std::span<const std::string_view> override {
        return volatile_fields_;
    }

private:
    static constexpr auto volatile_fields_ = std::array<std::string_view, 1>{"freelist"};
};

constexpr auto extensions = std::array<const char*, 3>{".sqlite", ".sqlite3", ".db"};
constexpr auto magic_bytes = std::array<std::uint8_t, 16>{
    'S', 'Q', 'L', 'i', 't', 'e', ' ', 'f', 'o', 'r', 'm', 'a', 't', ' ', '3', 0};
constexpr auto magic = std::array<liquid_format_magic_v1, 1>{
    liquid_format_magic_v1{magic_bytes.data(), magic_bytes.size()}};
constexpr auto volatile_names = std::array<const char*, 1>{"freelist"};

} // namespace

auto sqlite_module() -> const liquid_format_module_v1* {
    static auto backend = sqlite_backend{};
    static const auto module =
        liquid_format_module_v1{.abi_version = LIQUID_FORMAT_ABI_VERSION,
                                .struct_size = sizeof(liquid_format_module_v1),
                                .descriptor = {.name = "sqlite",
                                               .tier = 2,
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
                                .repack = nullptr,
                                .destroy = nullptr};
    return &module;
}

} // namespace liquid::formats::detail
