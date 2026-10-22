// SPDX-License-Identifier: MIT

#include "formats/binary/elf.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <ranges>
#include <string_view>
#include <unordered_map>

namespace liquid::formats::binary {
namespace {

constexpr auto sht_dynamic = std::uint32_t{6};
constexpr auto sht_dynsym = std::uint32_t{11};
constexpr auto shn_undef = std::uint16_t{0};
constexpr auto dt_needed = std::uint64_t{1};
constexpr auto dt_soname = std::uint64_t{14};
constexpr auto dt_rpath = std::uint64_t{15};
constexpr auto dt_runpath = std::uint64_t{29};

auto malformed(std::string message) -> error {
    return {errc::corrupt, std::move(message), "ELF"};
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

// A local limit signal unwinds nested version/DWARF parsing to the one inspector boundary.
// Charges are cumulative, including temporary and repeated copies, and are never refunded.
struct name_limit {};

struct name_budget {
    image& output;
    std::uint64_t remaining;
    static constexpr auto maximum_length = std::size_t{64} * 1024;

    auto charge(std::uint64_t bytes) -> void {
        if (bytes > remaining) {
            throw name_limit{};
        }
        remaining -= bytes;
        output.symbol_bytes += bytes;
    }

    auto copy(std::string_view value) -> std::string {
        if (value.size() > maximum_length) {
            throw name_limit{};
        }
        charge(sizeof(std::string) + value.size());
        return std::string{value};
    }

    auto append(std::string& target, std::string_view value) -> void {
        if (value.size() > maximum_length - target.size()) {
            throw name_limit{};
        }
        charge(value.size());
        target.append(value);
    }
};

auto text(std::span<const std::byte> bytes,
          std::uint64_t offset,
          std::uint64_t limit,
          name_budget& budget) -> std::optional<std::string> {
    if (offset >= bytes.size()) {
        return std::nullopt;
    }
    const auto available = std::min<std::uint64_t>(limit, bytes.size() - offset);
    // Scan only what could be retained, plus one byte for an exact-limit terminator.
    const auto maximum = std::min<std::uint64_t>(name_budget::maximum_length, budget.remaining);
    const auto input = bytes.subspan(static_cast<std::size_t>(offset),
                                     static_cast<std::size_t>(std::min(available, maximum + 1)));
    const auto end = std::ranges::find(input, std::byte{});
    if (end == input.end()) {
        if (available > maximum) {
            throw name_limit{};
        }
        return std::nullopt;
    }
    return budget.copy({reinterpret_cast<const char*>(input.data()),
                        static_cast<std::size_t>(end - input.begin())});
}

auto slice(std::span<const std::byte> bytes, std::uint64_t offset, std::uint64_t size)
    -> std::optional<std::span<const std::byte>> {
    if (offset > bytes.size() || size > bytes.size() - offset) {
        return std::nullopt;
    }
    return bytes.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(size));
}

struct elf_section : section {
    std::uint32_t link{};
    std::uint64_t entry_size{};
};

auto architecture(std::uint16_t machine) -> std::string {
    switch (machine) {
    case 3:
        return "x86";
    case 8:
        return "mips";
    case 20:
        return "ppc";
    case 21:
        return "ppc64";
    case 40:
        return "arm";
    case 62:
        return "x86_64";
    case 183:
        return "aarch64";
    case 243:
        return "riscv";
    default:
        return "unknown-" + std::to_string(machine);
    }
}

auto binding_name(unsigned value) -> std::string {
    switch (value) {
    case 1:
        return "global";
    case 2:
        return "weak";
    case 10:
        return "gnu_unique";
    default:
        return "local";
    }
}

auto visibility_name(unsigned value) -> std::string {
    switch (value & 3U) {
    case 1:
        return "internal";
    case 2:
        return "hidden";
    case 3:
        return "protected";
    default:
        return "default";
    }
}

auto named_section(std::span<const elf_section> sections, std::string_view name)
    -> const elf_section* {
    const auto found = std::ranges::find(sections, name, &elf_section::name);
    return found == sections.end() ? nullptr : &*found;
}

auto section_bytes(std::span<const std::byte> bytes, const elf_section* value)
    -> std::span<const std::byte> {
    if (!value) {
        return {};
    }
    return slice(bytes, value->offset, value->size).value_or(std::span<const std::byte>{});
}

auto version_names(std::span<const std::byte> bytes,
                   std::span<const elf_section> sections,
                   bool little,
                   name_budget& budget) -> std::map<std::uint16_t, std::string> {
    auto output = std::map<std::uint16_t, std::string>{};
    const auto* dynstr_section = named_section(sections, ".dynstr");
    const auto dynstr = section_bytes(bytes, dynstr_section);
    if (dynstr.empty()) {
        return output;
    }
    const auto* definitions = named_section(sections, ".gnu.version_d");
    const auto definition_bytes = section_bytes(bytes, definitions);
    auto offset = std::uint64_t{};
    while (offset + 20 <= definition_bytes.size()) {
        const auto index = integer<std::uint16_t>(definition_bytes, offset + 4, little);
        const auto auxiliary = integer<std::uint32_t>(definition_bytes, offset + 12, little);
        const auto next = integer<std::uint32_t>(definition_bytes, offset + 16, little);
        if (!index || !auxiliary || !next || offset + *auxiliary + 8 > definition_bytes.size()) {
            break;
        }
        const auto name_offset =
            integer<std::uint32_t>(definition_bytes, offset + *auxiliary, little);
        if (name_offset) {
            if (auto name = text(dynstr, *name_offset, dynstr.size(), budget)) {
                budget.charge(sizeof(decltype(output)::value_type) + 4 * sizeof(void*));
                output[*index & 0x7fffU] = std::move(*name);
            }
        }
        if (*next == 0) {
            break;
        }
        offset += *next;
    }
    const auto* requirements = named_section(sections, ".gnu.version_r");
    const auto requirement_bytes = section_bytes(bytes, requirements);
    offset = 0;
    while (offset + 16 <= requirement_bytes.size()) {
        const auto count = integer<std::uint16_t>(requirement_bytes, offset + 2, little);
        const auto auxiliary = integer<std::uint32_t>(requirement_bytes, offset + 8, little);
        const auto next = integer<std::uint32_t>(requirement_bytes, offset + 12, little);
        if (!count || !auxiliary || !next) {
            break;
        }
        auto entry = offset + *auxiliary;
        for (auto index = std::uint16_t{}; index < *count && entry + 16 <= requirement_bytes.size();
             ++index) {
            const auto version_index = integer<std::uint16_t>(requirement_bytes, entry + 6, little);
            const auto name_offset = integer<std::uint32_t>(requirement_bytes, entry + 8, little);
            const auto entry_next = integer<std::uint32_t>(requirement_bytes, entry + 12, little);
            if (!version_index || !name_offset || !entry_next) {
                break;
            }
            if (auto name = text(dynstr, *name_offset, dynstr.size(), budget)) {
                budget.charge(sizeof(decltype(output)::value_type) + 4 * sizeof(void*));
                output[*version_index & 0x7fffU] = std::move(*name);
            }
            if (*entry_next == 0) {
                break;
            }
            entry += *entry_next;
        }
        if (*next == 0) {
            break;
        }
        offset += *next;
    }
    return output;
}

struct dwarf_cursor {
    std::span<const std::byte> bytes;
    std::uint64_t offset{};
    bool little{true};

    template <class Integer> auto take() -> std::optional<Integer> {
        auto value = integer<Integer>(bytes, offset, little);
        if (value) {
            offset += sizeof(Integer);
        }
        return value;
    }

    auto uleb() -> std::optional<std::uint64_t> {
        auto value = std::uint64_t{};
        auto shift = unsigned{};
        while (offset < bytes.size() && shift < 64) {
            const auto byte =
                std::to_integer<unsigned char>(bytes[static_cast<std::size_t>(offset++)]);
            value |= static_cast<std::uint64_t>(byte & 0x7fU) << shift;
            if ((byte & 0x80U) == 0) {
                return value;
            }
            shift += 7;
        }
        return std::nullopt;
    }

    auto sleb() -> std::optional<std::int64_t> {
        auto value = std::uint64_t{};
        auto shift = unsigned{};
        auto byte = unsigned{};
        do {
            if (offset >= bytes.size() || shift >= 64) {
                return std::nullopt;
            }
            byte = std::to_integer<unsigned char>(bytes[static_cast<std::size_t>(offset++)]);
            value |= static_cast<std::uint64_t>(byte & 0x7fU) << shift;
            shift += 7;
        } while ((byte & 0x80U) != 0);
        if (shift < 64 && (byte & 0x40U) != 0) {
            value |= std::numeric_limits<std::uint64_t>::max() << shift;
        }
        return static_cast<std::int64_t>(value);
    }
};

struct dwarf_attribute {
    std::uint64_t name{};
    std::uint64_t form{};
    std::int64_t implicit{};
};

struct dwarf_abbreviation {
    std::uint64_t tag{};
    bool children{};
    std::vector<dwarf_attribute> attributes;
};

using abbreviation_table = std::map<std::uint64_t, dwarf_abbreviation>;

auto abbreviations(std::span<const std::byte> bytes,
                   std::uint64_t start,
                   bool little,
                   name_budget& budget) -> abbreviation_table {
    auto cursor = dwarf_cursor{bytes, start, little};
    auto output = abbreviation_table{};
    while (cursor.offset < bytes.size()) {
        const auto code = cursor.uleb();
        if (!code || *code == 0) {
            break;
        }
        const auto tag = cursor.uleb();
        const auto children = cursor.take<std::uint8_t>();
        if (!tag || !children) {
            break;
        }
        auto value = dwarf_abbreviation{*tag, *children != 0, {}};
        while (cursor.offset < bytes.size()) {
            const auto name = cursor.uleb();
            const auto form = cursor.uleb();
            if (!name || !form) {
                return {};
            }
            if (*name == 0 && *form == 0) {
                break;
            }
            auto implicit = std::int64_t{};
            if (*form == 0x21) {
                auto parsed = cursor.sleb();
                if (!parsed) {
                    return {};
                }
                implicit = *parsed;
            }
            budget.charge(sizeof(dwarf_attribute));
            value.attributes.push_back({*name, *form, implicit});
        }
        budget.charge(sizeof(abbreviation_table::value_type) + 4 * sizeof(void*));
        output.emplace(*code, std::move(value));
    }
    return output;
}

struct dwarf_value {
    std::optional<std::uint64_t> number;
    std::optional<std::string> string;
};

auto dwarf_string(std::span<const std::byte> section, std::uint64_t offset, name_budget& budget)
    -> std::optional<std::string> {
    return text(section, offset, section.size(), budget);
}

auto form_value(dwarf_cursor& cursor,
                std::uint64_t form,
                std::uint8_t address_size,
                std::uint8_t offset_size,
                std::uint64_t unit_start,
                std::span<const std::byte> debug_str,
                std::span<const std::byte> line_str,
                std::span<const std::byte> str_offsets,
                std::uint64_t str_offsets_base,
                std::int64_t implicit,
                name_budget& budget) -> std::optional<dwarf_value> {
    if (form == 0x16) {
        const auto actual = cursor.uleb();
        return actual ? form_value(cursor,
                                   *actual,
                                   address_size,
                                   offset_size,
                                   unit_start,
                                   debug_str,
                                   line_str,
                                   str_offsets,
                                   str_offsets_base,
                                   implicit,
                                   budget)
                      : std::nullopt;
    }
    auto fixed = [&](std::uint8_t size) -> std::optional<std::uint64_t> {
        switch (size) {
        case 1:
            return cursor.take<std::uint8_t>();
        case 2:
            return cursor.take<std::uint16_t>();
        case 4:
            return cursor.take<std::uint32_t>();
        case 8:
            return cursor.take<std::uint64_t>();
        default:
            return std::nullopt;
        }
    };
    if (form == 0x08) {
        auto value = text(cursor.bytes, cursor.offset, cursor.bytes.size() - cursor.offset, budget);
        if (!value) {
            return std::nullopt;
        }
        cursor.offset += value->size() + 1;
        return dwarf_value{{}, std::move(*value)};
    }
    if (form == 0x0e || form == 0x1f) {
        const auto offset = fixed(offset_size);
        auto value = offset ? dwarf_string(form == 0x0e ? debug_str : line_str, *offset, budget)
                            : std::nullopt;
        return value ? std::optional<dwarf_value>{dwarf_value{{}, std::move(*value)}}
                     : std::nullopt;
    }
    if (form == 0x1a || (form >= 0x25 && form <= 0x28)) {
        const auto index =
            form == 0x1a ? cursor.uleb() : fixed(static_cast<std::uint8_t>(form - 0x24));
        const auto at = index ? str_offsets_base + *index * offset_size : 0;
        const auto string_offset =
            index ? (offset_size == 8 ? integer<std::uint64_t>(str_offsets, at, cursor.little)
                                      : std::optional<std::uint64_t>{integer<std::uint32_t>(
                                            str_offsets, at, cursor.little)})
                  : std::nullopt;
        auto value = string_offset ? dwarf_string(debug_str, *string_offset, budget) : std::nullopt;
        return value ? std::optional<dwarf_value>{dwarf_value{{}, std::move(*value)}}
                     : std::nullopt;
    }
    if (form == 0x21) {
        return dwarf_value{static_cast<std::uint64_t>(implicit), {}};
    }
    if (form == 0x19) {
        return dwarf_value{1, {}};
    }
    if (form == 0x0d) {
        auto value = cursor.sleb();
        return value
                   ? std::optional<dwarf_value>{dwarf_value{static_cast<std::uint64_t>(*value), {}}}
                   : std::nullopt;
    }
    if (form == 0x0f || form == 0x15 || form == 0x22 || form == 0x23 || form == 0x1b) {
        auto value = cursor.uleb();
        return value ? std::optional<dwarf_value>{dwarf_value{*value, {}}} : std::nullopt;
    }
    if (form == 0x01) {
        auto value = fixed(address_size);
        return value ? std::optional<dwarf_value>{dwarf_value{*value, {}}} : std::nullopt;
    }
    if (form == 0x0b || form == 0x0c || form == 0x11 || form == 0x25 || form == 0x29) {
        auto value = fixed(1);
        return value ? std::optional<dwarf_value>{dwarf_value{
                           form == 0x11 ? unit_start + *value : *value, {}}}
                     : std::nullopt;
    }
    if (form == 0x05 || form == 0x12 || form == 0x26 || form == 0x2a) {
        auto value = fixed(2);
        return value ? std::optional<dwarf_value>{dwarf_value{
                           form == 0x12 ? unit_start + *value : *value, {}}}
                     : std::nullopt;
    }
    if (form == 0x06 || form == 0x13 || form == 0x17 || form == 0x1c || form == 0x27 ||
        form == 0x2b) {
        auto value = fixed(4);
        return value ? std::optional<dwarf_value>{dwarf_value{
                           form == 0x13 ? unit_start + *value : *value, {}}}
                     : std::nullopt;
    }
    if (form == 0x07 || form == 0x10 || form == 0x14 || form == 0x20 || form == 0x24 ||
        form == 0x28 || form == 0x2c) {
        auto value = fixed(8);
        return value ? std::optional<dwarf_value>{dwarf_value{
                           form == 0x14 ? unit_start + *value : *value, {}}}
                     : std::nullopt;
    }
    if (form == 0x03 || form == 0x04 || form == 0x09 || form == 0x0a || form == 0x18) {
        auto length = std::optional<std::uint64_t>{};
        if (form == 0x03) {
            length = fixed(2);
        } else if (form == 0x04) {
            length = fixed(4);
        } else if (form == 0x0a) {
            length = fixed(1);
        } else {
            length = cursor.uleb();
        }
        if (!length || *length > cursor.bytes.size() - cursor.offset) {
            return std::nullopt;
        }
        cursor.offset += *length;
        return dwarf_value{};
    }
    if (form == 0x1e) {
        if (cursor.bytes.size() - cursor.offset < 16) {
            return std::nullopt;
        }
        cursor.offset += 16;
        return dwarf_value{};
    }
    return std::nullopt;
}

struct dwarf_node {
    std::uint64_t tag{};
    std::optional<std::string> name;
    std::optional<std::string> linkage_name;
    std::optional<std::uint64_t> type;
    std::optional<std::uint64_t> byte_size;
    std::vector<std::uint64_t> children;
};

auto type_name(std::uint64_t id,
               const std::unordered_map<std::uint64_t, dwarf_node>& nodes,
               name_budget& budget,
               std::size_t depth = 0) -> std::string {
    if (depth > 16) {
        return budget.copy("?");
    }
    const auto found = nodes.find(id);
    if (found == nodes.end()) {
        return budget.copy("?");
    }
    const auto& node = found->second;
    auto nested = node.type ? type_name(*node.type, nodes, budget, depth + 1) : budget.copy("void");
    switch (node.tag) {
    case 0x0f:
        budget.append(nested, "*");
        return nested;
    case 0x10:
        budget.append(nested, "&");
        return nested;
    case 0x26:
    case 0x35: {
        auto output = budget.copy(node.tag == 0x26 ? "const " : "volatile ");
        budget.append(output, nested);
        return output;
    }
    case 0x16:
        if (node.name) {
            auto output = budget.copy(*node.name);
            budget.append(output, "=");
            budget.append(output, nested);
            return output;
        }
        return nested;
    case 0x13:
    case 0x02:
    case 0x17:
    case 0x04: {
        auto output = budget.copy(node.tag == 0x13   ? "struct "
                                  : node.tag == 0x02 ? "class "
                                  : node.tag == 0x17 ? "union "
                                                     : "enum ");
        budget.append(output, node.name ? std::string_view{*node.name} : "<anonymous>");
        if (node.byte_size) {
            budget.append(output, "[");
            budget.append(output, std::to_string(*node.byte_size));
            budget.append(output, "]");
        }
        return output;
    }
    default:
        return node.name ? budget.copy(*node.name) : std::move(nested);
    }
}

auto type_size(std::uint64_t id,
               const std::unordered_map<std::uint64_t, dwarf_node>& nodes,
               std::size_t depth = 0) -> std::optional<std::uint64_t> {
    if (depth > 16) {
        return std::nullopt;
    }
    const auto found = nodes.find(id);
    if (found == nodes.end()) {
        return std::nullopt;
    }
    if (found->second.byte_size) {
        return found->second.byte_size;
    }
    return found->second.type ? type_size(*found->second.type, nodes, depth + 1) : std::nullopt;
}

struct dwarf_metadata {
    std::unordered_map<std::string, std::string> signatures;
    std::unordered_map<std::string, std::uint64_t> object_sizes;
};

auto dwarf_records(std::span<const std::byte> bytes,
                   std::span<const elf_section> sections,
                   bool little,
                   name_budget& budget) -> dwarf_metadata {
    const auto info = section_bytes(bytes, named_section(sections, ".debug_info"));
    const auto abbrev = section_bytes(bytes, named_section(sections, ".debug_abbrev"));
    const auto debug_str = section_bytes(bytes, named_section(sections, ".debug_str"));
    const auto line_str = section_bytes(bytes, named_section(sections, ".debug_line_str"));
    const auto str_offsets = section_bytes(bytes, named_section(sections, ".debug_str_offsets"));
    auto nodes = std::unordered_map<std::uint64_t, dwarf_node>{};
    auto functions = std::vector<std::uint64_t>{};
    auto objects = std::vector<std::uint64_t>{};
    auto unit_at = std::uint64_t{};
    while (unit_at + 11 <= info.size()) {
        auto cursor = dwarf_cursor{info, unit_at, little};
        const auto length32 = cursor.take<std::uint32_t>();
        if (!length32 || *length32 == 0) {
            break;
        }
        const auto dwarf64 = *length32 == 0xffffffffU;
        const auto length =
            dwarf64 ? cursor.take<std::uint64_t>() : std::optional<std::uint64_t>{*length32};
        if (!length || *length > info.size() - cursor.offset) {
            break;
        }
        const auto unit_end = cursor.offset + *length;
        const auto version = cursor.take<std::uint16_t>();
        if (!version) {
            break;
        }
        auto address_size = std::uint8_t{};
        auto abbrev_offset = std::optional<std::uint64_t>{};
        if (*version >= 5) {
            if (!cursor.take<std::uint8_t>()) {
                break;
            }
            const auto size = cursor.take<std::uint8_t>();
            abbrev_offset = dwarf64 ? cursor.take<std::uint64_t>()
                                    : std::optional<std::uint64_t>{cursor.take<std::uint32_t>()};
            if (!size) {
                break;
            }
            address_size = *size;
        } else {
            abbrev_offset = dwarf64 ? cursor.take<std::uint64_t>()
                                    : std::optional<std::uint64_t>{cursor.take<std::uint32_t>()};
            const auto size = cursor.take<std::uint8_t>();
            if (!size) {
                break;
            }
            address_size = *size;
        }
        if (!abbrev_offset) {
            break;
        }
        const auto table = abbreviations(abbrev, *abbrev_offset, little, budget);
        auto parents = std::vector<std::uint64_t>{};
        auto str_offsets_base = str_offsets.size() >= 8 ? std::uint64_t{8} : std::uint64_t{};
        while (cursor.offset < unit_end) {
            const auto node_offset = cursor.offset;
            const auto code = cursor.uleb();
            if (!code) {
                break;
            }
            if (*code == 0) {
                if (!parents.empty()) {
                    parents.pop_back();
                }
                continue;
            }
            const auto definition = table.find(*code);
            if (definition == table.end()) {
                cursor.offset = unit_end;
                break;
            }
            auto node = dwarf_node{.tag = definition->second.tag};
            auto valid = true;
            for (const auto& attribute : definition->second.attributes) {
                auto value = form_value(cursor,
                                        attribute.form,
                                        address_size,
                                        dwarf64 ? 8 : 4,
                                        unit_at,
                                        debug_str,
                                        line_str,
                                        str_offsets,
                                        str_offsets_base,
                                        attribute.implicit,
                                        budget);
                if (!value) {
                    valid = false;
                    break;
                }
                if (attribute.name == 0x03) {
                    node.name = std::move(value->string);
                } else if (attribute.name == 0x6e || attribute.name == 0x2007) {
                    node.linkage_name = std::move(value->string);
                } else if (attribute.name == 0x49) {
                    node.type = value->number;
                } else if (attribute.name == 0x0b) {
                    node.byte_size = value->number;
                } else if (attribute.name == 0x72 && value->number) {
                    str_offsets_base = *value->number;
                }
            }
            if (!valid) {
                cursor.offset = unit_end;
                break;
            }
            budget.charge(sizeof(decltype(nodes)::value_type) + 4 * sizeof(void*));
            if (!parents.empty()) {
                budget.charge(sizeof(std::uint64_t));
                nodes[parents.back()].children.push_back(node_offset);
            }
            if (node.tag == 0x2e) {
                budget.charge(sizeof(std::uint64_t));
                functions.push_back(node_offset);
            } else if (node.tag == 0x34) {
                budget.charge(sizeof(std::uint64_t));
                objects.push_back(node_offset);
            }
            nodes[node_offset] = std::move(node);
            if (definition->second.children) {
                budget.charge(sizeof(std::uint64_t));
                parents.push_back(node_offset);
            }
        }
        unit_at = unit_end;
    }
    auto output = dwarf_metadata{};
    for (const auto id : functions) {
        const auto& node = nodes[id];
        const auto& name = node.linkage_name ? node.linkage_name : node.name;
        if (!name) {
            continue;
        }
        auto signature = node.type ? type_name(*node.type, nodes, budget) : budget.copy("void");
        budget.append(signature, " (");
        auto first = true;
        for (const auto child_id : node.children) {
            const auto& child = nodes[child_id];
            if (child.tag != 0x05 && child.tag != 0x18) {
                continue;
            }
            if (!first) {
                budget.append(signature, ", ");
            }
            if (child.tag == 0x18 || !child.type) {
                budget.append(signature, child.tag == 0x18 ? "..." : "?");
            } else {
                budget.append(signature, type_name(*child.type, nodes, budget));
            }
            first = false;
        }
        budget.append(signature, ")");
        budget.charge(sizeof(decltype(output.signatures)::value_type) + 4 * sizeof(void*));
        output.signatures[budget.copy(*name)] = std::move(signature);
    }
    for (const auto id : objects) {
        const auto& node = nodes[id];
        const auto& name = node.linkage_name ? node.linkage_name : node.name;
        const auto size = node.type ? type_size(*node.type, nodes) : std::nullopt;
        if (name && size) {
            budget.charge(sizeof(decltype(output.object_sizes)::value_type) + 4 * sizeof(void*));
            output.object_sizes[budget.copy(*name)] = *size;
        }
    }
    return output;
}

auto inspect_elf_impl(std::span<const std::byte> bytes, image& output, name_budget& budget)
    -> result<void> {
    if (bytes.size() < 52 || bytes[0] != std::byte{0x7f} || bytes[1] != std::byte{'E'} ||
        bytes[2] != std::byte{'L'} || bytes[3] != std::byte{'F'}) {
        return std::unexpected(malformed("invalid ELF header"));
    }
    const auto elf_class = std::to_integer<unsigned char>(bytes[4]);
    const auto encoding = std::to_integer<unsigned char>(bytes[5]);
    if ((elf_class != 1 && elf_class != 2) || (encoding != 1 && encoding != 2)) {
        return std::unexpected(malformed("unsupported ELF class or byte order"));
    }
    const auto is64 = elf_class == 2;
    const auto little = encoding == 1;
    const auto type = integer<std::uint16_t>(bytes, 16, little);
    const auto machine = integer<std::uint16_t>(bytes, 18, little);
    const auto section_offset =
        is64 ? integer<std::uint64_t>(bytes, 40, little)
             : std::optional<std::uint64_t>{integer<std::uint32_t>(bytes, 32, little)};
    const auto entry_size = integer<std::uint16_t>(bytes, is64 ? 58 : 46, little);
    const auto count = integer<std::uint16_t>(bytes, is64 ? 60 : 48, little);
    const auto names_index = integer<std::uint16_t>(bytes, is64 ? 62 : 50, little);
    if (!type || !machine || !section_offset || !entry_size || !count || !names_index ||
        *entry_size < (is64 ? 64 : 40) || *names_index >= *count ||
        *section_offset > bytes.size() ||
        static_cast<std::uint64_t>(*entry_size) * *count > bytes.size() - *section_offset) {
        return std::unexpected(malformed("invalid ELF section table"));
    }
    const auto names_header =
        *section_offset + static_cast<std::uint64_t>(*entry_size) * *names_index;
    const auto names_offset = is64 ? integer<std::uint64_t>(bytes, names_header + 24, little)
                                   : std::optional<std::uint64_t>{
                                         integer<std::uint32_t>(bytes, names_header + 16, little)};
    const auto names_size = is64 ? integer<std::uint64_t>(bytes, names_header + 32, little)
                                 : std::optional<std::uint64_t>{
                                       integer<std::uint32_t>(bytes, names_header + 20, little)};
    const auto names =
        names_offset && names_size ? slice(bytes, *names_offset, *names_size) : std::nullopt;
    if (!names) {
        return std::unexpected(malformed("invalid ELF section-name table"));
    }
    output.architecture = architecture(*machine);
    output.shared_library = *type == 3;
    auto sections = std::vector<elf_section>{};
    budget.charge(sizeof(elf_section) * *count);
    sections.reserve(*count);
    for (auto index = std::uint16_t{}; index < *count; ++index) {
        const auto at = *section_offset + static_cast<std::uint64_t>(*entry_size) * index;
        const auto name_offset = integer<std::uint32_t>(bytes, at, little);
        const auto section_type = integer<std::uint32_t>(bytes, at + 4, little);
        const auto flags =
            is64 ? integer<std::uint64_t>(bytes, at + 8, little)
                 : std::optional<std::uint64_t>{integer<std::uint32_t>(bytes, at + 8, little)};
        const auto address =
            is64 ? integer<std::uint64_t>(bytes, at + 16, little)
                 : std::optional<std::uint64_t>{integer<std::uint32_t>(bytes, at + 12, little)};
        const auto offset =
            is64 ? integer<std::uint64_t>(bytes, at + 24, little)
                 : std::optional<std::uint64_t>{integer<std::uint32_t>(bytes, at + 16, little)};
        const auto size =
            is64 ? integer<std::uint64_t>(bytes, at + 32, little)
                 : std::optional<std::uint64_t>{integer<std::uint32_t>(bytes, at + 20, little)};
        const auto link = integer<std::uint32_t>(bytes, at + (is64 ? 40 : 24), little);
        const auto entry =
            is64 ? integer<std::uint64_t>(bytes, at + 56, little)
                 : std::optional<std::uint64_t>{integer<std::uint32_t>(bytes, at + 36, little)};
        if (!name_offset || !section_type || !flags || !address || !offset || !size || !link ||
            !entry || *name_offset >= names->size()) {
            return std::unexpected(malformed("invalid ELF section header"));
        }
        auto name = text(*names, *name_offset, names->size() - *name_offset, budget);
        if (!name || (*section_type != 8 && !slice(bytes, *offset, *size))) {
            return std::unexpected(malformed("ELF section lies outside the file"));
        }
        sections.push_back(
            {section{std::move(*name), *offset, *size, *address, *flags, *section_type},
             *link,
             *entry});
    }
    for (const auto& value : sections) {
        budget.charge(sizeof(section) + value.name.size());
        output.sections.push_back(static_cast<const section&>(value));
    }
    auto printable = std::string{};
    const auto finish_printable = [&] {
        if (printable.size() >= 4 && output.symbol_names.size() < 256 &&
            (printable.starts_with('_') || printable.find("::") != std::string::npos ||
             printable.starts_with("GLIBC"))) {
            budget.charge(sizeof(std::string) + printable.size());
            output.symbol_names.push_back(printable);
        }
        printable.clear();
    };
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned char>(byte);
        if (value >= 0x20 && value <= 0x7e) {
            if (printable.size() < 512) {
                printable.push_back(static_cast<char>(value));
            }
        } else {
            finish_printable();
        }
    }
    finish_printable();
    const auto versions = version_names(bytes, sections, little, budget);
    const auto version_table = section_bytes(bytes, named_section(sections, ".gnu.version"));
    const auto dwarf = dwarf_records(bytes, sections, little, budget);
    const auto dynamic_symbols = std::ranges::find(sections, sht_dynsym, &elf_section::type);
    if (dynamic_symbols != sections.end() && dynamic_symbols->link < sections.size()) {
        const auto symbols = section_bytes(bytes, &*dynamic_symbols);
        const auto strings = section_bytes(bytes, &sections[dynamic_symbols->link]);
        const auto stride =
            dynamic_symbols->entry_size ? dynamic_symbols->entry_size : (is64 ? 24U : 16U);
        for (auto index = std::uint64_t{}; stride != 0 && index < symbols.size() / stride;
             ++index) {
            const auto at = index * stride;
            const auto name_offset = integer<std::uint32_t>(symbols, at, little);
            const auto info = integer<std::uint8_t>(symbols, at + (is64 ? 4 : 12), little);
            const auto other = integer<std::uint8_t>(symbols, at + (is64 ? 5 : 13), little);
            const auto section_index =
                integer<std::uint16_t>(symbols, at + (is64 ? 6 : 14), little);
            const auto size =
                is64
                    ? integer<std::uint64_t>(symbols, at + 16, little)
                    : std::optional<std::uint64_t>{integer<std::uint32_t>(symbols, at + 8, little)};
            if (!name_offset || !info || !other || !section_index || !size ||
                *section_index == shn_undef) {
                continue;
            }
            const auto symbol_type = *info & 0x0fU;
            const auto binding = *info >> 4U;
            if ((symbol_type != 1 && symbol_type != 2 && symbol_type != 10) ||
                (binding != 1 && binding != 2 && binding != 10) || (*other & 3U) == 2) {
                continue;
            }
            auto name = text(strings, *name_offset, strings.size(), budget);
            if (!name || name->empty()) {
                continue;
            }
            auto version = std::optional<std::string>{};
            if (index * 2 + 2 <= version_table.size()) {
                const auto version_index = integer<std::uint16_t>(version_table, index * 2, little);
                if (version_index) {
                    const auto found = versions.find(*version_index & 0x7fffU);
                    if (found != versions.end()) {
                        version = budget.copy(found->second);
                    }
                }
            }
            auto signature = std::optional<std::string>{};
            if (const auto found = dwarf.signatures.find(*name); found != dwarf.signatures.end()) {
                signature = budget.copy(found->second);
            }
            auto symbol_size = *size;
            if (symbol_type == 1 && symbol_size == 0) {
                if (const auto found = dwarf.object_sizes.find(*name);
                    found != dwarf.object_sizes.end()) {
                    symbol_size = found->second;
                }
            }
            budget.charge(sizeof(symbol) + 32);
            output.exported_symbols.push_back(
                {std::move(*name),
                 std::move(version),
                 symbol_type == 1 ? symbol_kind::object : symbol_kind::function,
                 symbol_size,
                 binding_name(binding),
                 visibility_name(*other),
                 std::move(signature)});
        }
    }
    const auto dynamic = std::ranges::find(sections, sht_dynamic, &elf_section::type);
    if (dynamic != sections.end() && dynamic->link < sections.size()) {
        const auto entries = section_bytes(bytes, &*dynamic);
        const auto strings = section_bytes(bytes, &sections[dynamic->link]);
        const auto stride = dynamic->entry_size ? dynamic->entry_size : (is64 ? 16U : 8U);
        for (auto index = std::uint64_t{}; stride != 0 && index < entries.size() / stride;
             ++index) {
            const auto at = index * stride;
            const auto tag =
                is64 ? integer<std::uint64_t>(entries, at, little)
                     : std::optional<std::uint64_t>{integer<std::uint32_t>(entries, at, little)};
            const auto value =
                is64
                    ? integer<std::uint64_t>(entries, at + 8, little)
                    : std::optional<std::uint64_t>{integer<std::uint32_t>(entries, at + 4, little)};
            if (!tag || !value || *tag == 0) {
                break;
            }
            if (*tag != dt_needed && *tag != dt_soname && *tag != dt_rpath && *tag != dt_runpath) {
                continue;
            }
            auto name = text(strings, *value, strings.size(), budget);
            if (!name) {
                continue;
            }
            budget.charge(sizeof(path_reference));
            const auto string_offset = sections[dynamic->link].offset + *value;
            if (*tag == dt_soname) {
                output.path_references.push_back(
                    {path_kind::install_name, budget.copy(*name), string_offset});
                output.install_name = std::move(*name);
            } else if (*tag == dt_needed) {
                output.path_references.push_back(
                    {path_kind::needed_library, budget.copy(*name), string_offset});
                budget.charge(sizeof(std::string));
                output.needed_libraries.push_back(std::move(*name));
            } else {
                output.path_references.push_back(
                    {path_kind::runtime_search_path, std::move(*name), string_offset});
            }
        }
    }
    return {};
}

} // namespace

auto inspect_elf(std::span<const std::byte> bytes) -> result<image> {
    return inspect_elf(bytes, 16ULL << 20U);
}

auto inspect_elf(std::span<const std::byte> bytes, std::uint64_t byte_budget) -> result<image> {
    auto output = image{.kind = image_kind::elf};
    auto budget = name_budget{output, byte_budget};
    try {
        auto parsed = inspect_elf_impl(bytes, output, budget);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
    } catch (const name_limit&) {
        output.truncated = true;
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
