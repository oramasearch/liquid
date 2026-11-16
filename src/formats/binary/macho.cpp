// SPDX-License-Identifier: MIT

#include "formats/binary/macho.hpp"

#include <algorithm>
#include <map>
#include <ranges>
#include <string_view>

namespace liquid::formats::binary {
namespace {

auto malformed(std::string message) -> error {
    return {errc::corrupt, std::move(message), "Mach-O"};
}

template <class Integer>
auto integer(std::span<const std::byte> bytes, std::uint64_t offset, bool little)
    -> std::optional<Integer> {
    if (offset > bytes.size() ||
        bytes.size() - static_cast<std::size_t>(offset) < sizeof(Integer)) {
        return std::nullopt;
    }
    auto output = Integer{};
    for (auto index = std::size_t{}; index < sizeof(Integer); ++index) {
        const auto source = little ? index : sizeof(Integer) - index - 1;
        output |= static_cast<Integer>(std::to_integer<unsigned char>(
                      bytes[static_cast<std::size_t>(offset) + source]))
                  << (index * 8U);
    }
    return output;
}

auto fixed_text(std::span<const std::byte> bytes, std::uint64_t offset, std::uint64_t size)
    -> std::optional<std::string> {
    if (offset > bytes.size() || size > bytes.size() - offset) {
        return std::nullopt;
    }
    auto output = std::string{};
    for (auto index = std::uint64_t{}; index < size; ++index) {
        const auto value =
            std::to_integer<unsigned char>(bytes[static_cast<std::size_t>(offset + index)]);
        if (value == 0) {
            return output;
        }
        output.push_back(static_cast<char>(value));
    }
    return output;
}

auto c_text(std::span<const std::byte> bytes, std::uint64_t offset, std::uint64_t end)
    -> std::optional<std::string> {
    if (offset >= end || end > bytes.size()) {
        return std::nullopt;
    }
    auto output = std::string{};
    while (offset < end) {
        const auto value =
            std::to_integer<unsigned char>(bytes[static_cast<std::size_t>(offset++)]);
        if (value == 0) {
            return output;
        }
        output.push_back(static_cast<char>(value));
    }
    return std::nullopt;
}

auto architecture(std::uint32_t cpu) -> std::string {
    switch (cpu) {
    case 7:
        return "x86";
    case 0x01000007U:
        return "x86_64";
    case 12:
        return "arm";
    case 0x0100000cU:
        return "arm64";
    case 18:
        return "ppc";
    case 0x01000012U:
        return "ppc64";
    default:
        return "unknown-" + std::to_string(cpu);
    }
}

struct symbol_entry {
    std::string name;
    std::uint8_t section_index{};
    std::uint64_t value{};
    std::uint16_t description{};
};

// Borrow at most 64 KiB, examining one extra byte to distinguish an exact-length name.
// Repeated string-table offsets never allocate until the retained copies have been charged.
auto symbol_text(std::span<const std::byte> bytes, bool& truncated)
    -> std::optional<std::string_view> {
    constexpr auto maximum = std::size_t{64} << 10U;
    const auto bounded = bytes.first(std::min(bytes.size(), maximum + 1));
    const auto end = std::ranges::find(bounded, std::byte{});
    const auto size = static_cast<std::size_t>(end - bounded.begin());
    if (size > maximum) {
        truncated = true;
        auto length = maximum;
        // Keep a valid UTF-8 name valid when the cutoff falls inside a code point.
        while (length != 0 && (std::to_integer<unsigned char>(bytes[length]) & 0xc0U) == 0x80U) {
            --length;
        }
        return std::string_view{reinterpret_cast<const char*>(bytes.data()), length};
    }
    if (end == bounded.end()) {
        return std::nullopt;
    }
    return std::string_view{reinterpret_cast<const char*>(bytes.data()), size};
}

} // namespace

auto inspect_macho(std::span<const std::byte> bytes) -> result<image> {
    return inspect_macho(bytes, 16ULL << 20U);
}

auto inspect_macho(std::span<const std::byte> bytes, std::uint64_t byte_budget) -> result<image> {
    const auto magic = integer<std::uint32_t>(bytes, 0, true);
    if (!magic) {
        return std::unexpected(malformed("missing Mach-O header"));
    }
    const auto is64 = *magic == 0xfeedfacfU || *magic == 0xcffaedfeU;
    const auto little = *magic == 0xfeedfaceU || *magic == 0xfeedfacfU;
    if (*magic != 0xfeedfaceU && *magic != 0xfeedfacfU && *magic != 0xcefaedfeU &&
        *magic != 0xcffaedfeU) {
        return std::unexpected(malformed("unsupported Mach-O magic"));
    }
    const auto header_size = static_cast<std::uint64_t>(is64 ? 32 : 28);
    const auto cpu = integer<std::uint32_t>(bytes, 4, little);
    const auto file_type = integer<std::uint32_t>(bytes, 12, little);
    const auto command_count = integer<std::uint32_t>(bytes, 16, little);
    const auto command_bytes = integer<std::uint32_t>(bytes, 20, little);
    if (!cpu || !file_type || !command_count || !command_bytes || header_size > bytes.size() ||
        *command_bytes > bytes.size() - header_size) {
        return std::unexpected(malformed("invalid Mach-O load-command table"));
    }
    auto output = image{.kind = image_kind::macho,
                        .architecture = architecture(*cpu),
                        .shared_library = *file_type == 6};
    auto symbol_offset = std::uint32_t{};
    auto symbol_count = std::uint32_t{};
    auto string_offset = std::uint32_t{};
    auto string_size = std::uint32_t{};
    auto section_is_code = std::vector<bool>{false};
    section_is_code.push_back(false);
    auto offset = header_size;
    for (auto index = std::uint32_t{}; index < *command_count; ++index) {
        const auto command = integer<std::uint32_t>(bytes, offset, little);
        const auto size = integer<std::uint32_t>(bytes, offset + 4, little);
        if (!command || !size || *size < 8 || offset > bytes.size() ||
            *size > bytes.size() - offset) {
            return std::unexpected(malformed("invalid Mach-O load command"));
        }
        const auto base_command = *command & 0x7fffffffU;
        if (base_command == 0x1U || base_command == 0x19U) {
            const auto segment64 = base_command == 0x19U;
            const auto section_count =
                integer<std::uint32_t>(bytes, offset + (segment64 ? 64 : 48), little);
            auto section_at = offset + (segment64 ? 72 : 56);
            const auto stride = static_cast<std::uint64_t>(segment64 ? 80 : 68);
            if (!section_count || section_at > offset + *size ||
                static_cast<std::uint64_t>(*section_count) * stride > offset + *size - section_at) {
                return std::unexpected(malformed("invalid Mach-O section table"));
            }
            for (auto item = std::uint32_t{}; item < *section_count; ++item) {
                const auto name = fixed_text(bytes, section_at, 16);
                const auto segment = fixed_text(bytes, section_at + 16, 16);
                const auto address =
                    segment64 ? integer<std::uint64_t>(bytes, section_at + 32, little)
                              : std::optional<std::uint64_t>{
                                    integer<std::uint32_t>(bytes, section_at + 32, little)};
                const auto section_size =
                    segment64 ? integer<std::uint64_t>(bytes, section_at + 40, little)
                              : std::optional<std::uint64_t>{
                                    integer<std::uint32_t>(bytes, section_at + 36, little)};
                const auto file_offset =
                    integer<std::uint32_t>(bytes, section_at + (segment64 ? 48 : 40), little);
                const auto flags =
                    integer<std::uint32_t>(bytes, section_at + (segment64 ? 64 : 56), little);
                if (!name || !segment || !address || !section_size || !file_offset || !flags ||
                    (*section_size != 0 && (*file_offset > bytes.size() ||
                                            *section_size > bytes.size() - *file_offset))) {
                    return std::unexpected(malformed("invalid Mach-O section"));
                }
                output.sections.push_back(
                    {*name, *file_offset, *section_size, *address, *flags, 0});
                section_is_code.push_back((*segment == "__TEXT" && *name == "__text") ||
                                          ((*flags & 0x80000000U) != 0));
                section_at += stride;
            }
        } else if (base_command == 0x2U) {
            symbol_offset = integer<std::uint32_t>(bytes, offset + 8, little).value_or(0);
            symbol_count = integer<std::uint32_t>(bytes, offset + 12, little).value_or(0);
            string_offset = integer<std::uint32_t>(bytes, offset + 16, little).value_or(0);
            string_size = integer<std::uint32_t>(bytes, offset + 20, little).value_or(0);
        } else if (base_command == 0x1cU) {
            const auto path_offset = integer<std::uint32_t>(bytes, offset + 8, little);
            const auto path =
                path_offset ? c_text(bytes, offset + *path_offset, offset + *size) : std::nullopt;
            if (path) {
                output.path_references.push_back(
                    {path_kind::runtime_search_path, *path, offset + *path_offset});
            }
        } else if (base_command == 0x0cU || base_command == 0x0dU || base_command == 0x18U ||
                   base_command == 0x1fU || base_command == 0x23U || base_command == 0x8000001fU) {
            const auto name_offset = integer<std::uint32_t>(bytes, offset + 8, little);
            const auto name =
                name_offset ? c_text(bytes, offset + *name_offset, offset + *size) : std::nullopt;
            if (name && base_command == 0x0dU) {
                output.path_references.push_back(
                    {path_kind::install_name, *name, offset + *name_offset});
                output.install_name = *name;
            } else if (name) {
                output.path_references.push_back(
                    {path_kind::needed_library, *name, offset + *name_offset});
                output.needed_libraries.push_back(*name);
            }
        }
        offset += *size;
    }
    const auto stride = static_cast<std::uint64_t>(is64 ? 16 : 12);
    if (symbol_count != 0 &&
        (symbol_offset > bytes.size() ||
         static_cast<std::uint64_t>(symbol_count) * stride > bytes.size() - symbol_offset ||
         string_offset > bytes.size() || string_size > bytes.size() - string_offset)) {
        return std::unexpected(malformed("invalid Mach-O symbol table"));
    }
    auto entries = std::vector<symbol_entry>{};
    for (auto index = std::uint32_t{}; index < symbol_count; ++index) {
        const auto at = symbol_offset + static_cast<std::uint64_t>(index) * stride;
        const auto name_offset = integer<std::uint32_t>(bytes, at, little);
        const auto type = integer<std::uint8_t>(bytes, at + 4, little);
        const auto section_index = integer<std::uint8_t>(bytes, at + 5, little);
        const auto description = integer<std::uint16_t>(bytes, at + 6, little);
        const auto value =
            is64 ? integer<std::uint64_t>(bytes, at + 8, little)
                 : std::optional<std::uint64_t>{integer<std::uint32_t>(bytes, at + 8, little)};
        if (!name_offset || !type || !section_index || !description || !value ||
            *name_offset >= string_size) {
            continue;
        }
        const auto exported = (*type & 0xe0U) == 0 && (*type & 0x01U) != 0 &&
                              (*type & 0x0eU) != 0 && *section_index != 0;
        const auto sampled = output.symbol_names.size() < 256;
        if (!exported && !sampled) {
            continue;
        }
        auto name =
            symbol_text(bytes.subspan(static_cast<std::uint64_t>(string_offset) + *name_offset,
                                      string_size - *name_offset),
                        output.truncated);
        if (!name || name->empty()) {
            continue;
        }
        // Charge both retained copies and both entry arrays before allocating either one.
        // Moving entries into exported_symbols below avoids another name allocation.
        const auto charge = (sampled ? sizeof(std::string) + name->size() : 0) +
                            (exported ? sizeof(symbol_entry) + sizeof(symbol) + name->size() : 0);
        if (charge > byte_budget - output.symbol_bytes) {
            output.truncated = true;
            break;
        }
        output.symbol_bytes += charge;
        if (sampled) {
            output.symbol_names.emplace_back(*name);
        }
        if (!exported) {
            continue;
        }
        if (name->starts_with('_')) {
            name->remove_prefix(1);
        }
        entries.push_back({std::string{*name}, *section_index, *value, *description});
    }
    std::ranges::sort(entries, [](const auto& left, const auto& right) {
        return std::pair{left.section_index, left.value} <
               std::pair{right.section_index, right.value};
    });
    for (auto index = std::size_t{}; index < entries.size(); ++index) {
        auto& entry = entries[index];
        const auto weak = (entry.description & 0x0080U) != 0;
        const auto indexed_code =
            entry.section_index < section_is_code.size() && section_is_code[entry.section_index];
        const auto addressed_code = std::ranges::any_of(output.sections, [&](const auto& value) {
            return (value.flags & 0x80000000U) != 0 && entry.value >= value.address &&
                   entry.value - value.address < value.size;
        });
        const auto code = indexed_code || addressed_code;
        output.exported_symbols.push_back({std::move(entry.name),
                                           {},
                                           code ? symbol_kind::function : symbol_kind::object,
                                           0,
                                           weak ? "weak" : "global",
                                           "default",
                                           {}});
    }
    std::ranges::sort(output.exported_symbols, {}, &symbol::name);
    std::ranges::sort(output.needed_libraries);
    output.needed_libraries.erase(
        std::unique(output.needed_libraries.begin(), output.needed_libraries.end()),
        output.needed_libraries.end());
    output.shared_library = output.shared_library && output.install_name.has_value();
    return output;
}

} // namespace liquid::formats::binary
