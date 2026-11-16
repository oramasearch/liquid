// SPDX-License-Identifier: MIT

#include "formats/binary/pe.hpp"

#include <algorithm>
#include <array>
#include <ranges>
#include <set>

namespace liquid::formats::binary {
namespace {

auto malformed(std::string message) -> error {
    return {errc::corrupt, std::move(message), "PE"};
}

template <class Integer>
auto integer(std::span<const std::byte> bytes, std::uint64_t offset) -> std::optional<Integer> {
    if (offset > bytes.size() ||
        bytes.size() - static_cast<std::size_t>(offset) < sizeof(Integer)) {
        return std::nullopt;
    }
    auto output = Integer{};
    for (auto index = std::size_t{}; index < sizeof(Integer); ++index) {
        output |= static_cast<Integer>(std::to_integer<unsigned char>(
                      bytes[static_cast<std::size_t>(offset) + index]))
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

enum class name_kind : std::size_t { install, exported, imported, coff };

// Deduplicate before scanning or copying, and keep names and their output/cache records
// charged for the whole inspection. Capped names and exhausted budgets preserve partial facts.
class name_reader {
public:
    name_reader(std::span<const std::byte> bytes, image& output, std::uint64_t budget)
        : bytes_(bytes), output_(output), remaining_(budget) {}

    auto read(std::uint64_t offset, name_kind kind) -> std::optional<std::string> {
        auto& offsets = offsets_[static_cast<std::size_t>(kind)];
        if (exhausted_ || offsets.contains(offset)) {
            return std::nullopt;
        }
        if (offset >= bytes_.size()) {
            output_.truncated = true;
            return std::nullopt;
        }
        const auto input = bytes_.subspan(static_cast<std::size_t>(offset));
        const auto bounded = input.first(std::min(input.size(), maximum_length + 1));
        const auto end = std::ranges::find(bounded, std::byte{});
        auto length = std::min(static_cast<std::size_t>(end - bounded.begin()), maximum_length);
        if (end == bounded.end()) {
            output_.truncated = true;
            // Preserve valid UTF-8 when the byte cap falls inside a code point.
            if (input.size() > maximum_length) {
                while (length != 0 &&
                       (std::to_integer<unsigned char>(input[length]) & 0xc0U) == 0x80U) {
                    --length;
                }
            }
        }
        return keep(offset, kind, bounded.first(length));
    }

    // COFF evidence keeps its existing fixed-width/160-byte bounds and shares the allowance.
    auto read_fixed(std::uint64_t offset, std::size_t width) -> std::optional<std::string> {
        if (exhausted_ || offsets_[static_cast<std::size_t>(name_kind::coff)].contains(offset)) {
            return std::nullopt;
        }
        if (offset > bytes_.size() || width > bytes_.size() - offset) {
            output_.truncated = true;
            return std::nullopt;
        }
        const auto input = bytes_.subspan(static_cast<std::size_t>(offset), width);
        const auto end = std::ranges::find(input, std::byte{});
        return keep(
            offset, name_kind::coff, input.first(static_cast<std::size_t>(end - input.begin())));
    }

    auto exhausted() const -> bool {
        return exhausted_;
    }

private:
    auto keep(std::uint64_t offset, name_kind kind, std::span<const std::byte> name)
        -> std::optional<std::string> {
        // Charge the output record and conservative tree-node storage before either allocation.
        const auto record = kind == name_kind::exported ? sizeof(symbol) : sizeof(std::string);
        const auto charge = name.size() + record + sizeof(std::uint64_t) + 4 * sizeof(void*);
        if (charge > remaining_) {
            output_.truncated = true;
            exhausted_ = true;
            return std::nullopt;
        }
        remaining_ -= charge;
        output_.symbol_bytes += charge;
        offsets_[static_cast<std::size_t>(kind)].insert(offset);
        return std::string{reinterpret_cast<const char*>(name.data()), name.size()};
    }

    static constexpr auto maximum_length = std::size_t{64} * 1024;
    std::span<const std::byte> bytes_;
    image& output_;
    std::array<std::set<std::uint64_t>, 4> offsets_;
    std::uint64_t remaining_;
    bool exhausted_{};
};

auto architecture(std::uint16_t machine) -> std::string {
    switch (machine) {
    case 0x014c:
        return "x86";
    case 0x8664:
        return "x86_64";
    case 0x01c0:
    case 0x01c4:
        return "arm";
    case 0xaa64:
        return "arm64";
    default:
        return "unknown-" + std::to_string(machine);
    }
}

struct pe_section : section {
    std::uint32_t virtual_size{};
    std::uint32_t raw_size{};
    std::uint32_t characteristics{};
};

auto rva_offset(std::span<const pe_section> sections, std::uint32_t rva)
    -> std::optional<std::uint64_t> {
    for (const auto& value : sections) {
        const auto span = std::max(value.virtual_size, value.raw_size);
        if (rva >= value.address && rva - value.address < span) {
            const auto delta = static_cast<std::uint64_t>(rva) - value.address;
            if (delta < value.raw_size) {
                return value.offset + delta;
            }
        }
    }
    return std::nullopt;
}

auto code_rva(std::span<const pe_section> sections, std::uint32_t rva) -> bool {
    return std::ranges::any_of(sections, [&](const auto& value) {
        return rva >= value.address && rva - value.address < value.virtual_size &&
               (value.characteristics & 0x00000020U) != 0;
    });
}

} // namespace

auto inspect_pe(std::span<const std::byte> bytes) -> result<image> {
    return inspect_pe(bytes, 16U * 1024U * 1024U);
}

auto inspect_pe(std::span<const std::byte> bytes, std::uint64_t symbol_budget) -> result<image> {
    if (bytes.size() < 0x40 || bytes[0] != std::byte{'M'} || bytes[1] != std::byte{'Z'}) {
        return std::unexpected(malformed("invalid DOS header"));
    }
    const auto pe_offset = integer<std::uint32_t>(bytes, 0x3c);
    if (!pe_offset || *pe_offset > bytes.size() || bytes.size() - *pe_offset < 24 ||
        fixed_text(bytes, *pe_offset, 4) != std::optional<std::string>{"PE"}) {
        return std::unexpected(malformed("invalid PE signature"));
    }
    const auto machine = integer<std::uint16_t>(bytes, *pe_offset + 4);
    const auto section_count = integer<std::uint16_t>(bytes, *pe_offset + 6);
    const auto optional_size = integer<std::uint16_t>(bytes, *pe_offset + 20);
    const auto characteristics = integer<std::uint16_t>(bytes, *pe_offset + 22);
    const auto optional = static_cast<std::uint64_t>(*pe_offset) + 24;
    if (!machine || !section_count || !optional_size || !characteristics ||
        optional > bytes.size() || *optional_size > bytes.size() - optional) {
        return std::unexpected(malformed("invalid PE file header"));
    }
    const auto magic = integer<std::uint16_t>(bytes, optional);
    if (!magic || (*magic != 0x10b && *magic != 0x20b)) {
        return std::unexpected(malformed("unsupported PE optional header"));
    }
    const auto pe64 = *magic == 0x20b;
    const auto directories = optional + (pe64 ? 112 : 96);
    const auto section_table = optional + *optional_size;
    if (section_table > bytes.size() ||
        static_cast<std::uint64_t>(*section_count) * 40 > bytes.size() - section_table) {
        return std::unexpected(malformed("invalid PE section table"));
    }
    auto sections = std::vector<pe_section>{};
    for (auto index = std::uint16_t{}; index < *section_count; ++index) {
        const auto at = section_table + static_cast<std::uint64_t>(index) * 40;
        const auto name = fixed_text(bytes, at, 8);
        const auto virtual_size = integer<std::uint32_t>(bytes, at + 8);
        const auto virtual_address = integer<std::uint32_t>(bytes, at + 12);
        const auto raw_size = integer<std::uint32_t>(bytes, at + 16);
        const auto raw_offset = integer<std::uint32_t>(bytes, at + 20);
        const auto flags = integer<std::uint32_t>(bytes, at + 36);
        if (!name || !virtual_size || !virtual_address || !raw_size || !raw_offset || !flags ||
            (*raw_size != 0 &&
             (*raw_offset > bytes.size() || *raw_size > bytes.size() - *raw_offset))) {
            return std::unexpected(malformed("invalid PE section"));
        }
        sections.push_back({section{*name, *raw_offset, *raw_size, *virtual_address, *flags, 0},
                            *virtual_size,
                            *raw_size,
                            *flags});
    }
    auto output = image{.kind = image_kind::pe,
                        .architecture = architecture(*machine),
                        .shared_library = (*characteristics & 0x2000U) != 0};
    for (const auto& value : sections) {
        output.sections.push_back(static_cast<const section&>(value));
    }
    auto name_bytes = name_reader{bytes, output, symbol_budget};
    const auto symbol_offset = integer<std::uint32_t>(bytes, *pe_offset + 12).value_or(0);
    const auto symbol_count = integer<std::uint32_t>(bytes, *pe_offset + 16).value_or(0);
    const auto string_table = symbol_offset + static_cast<std::uint64_t>(symbol_count) * 18U;
    for (auto index = std::uint64_t{};
         index < symbol_count && !name_bytes.exhausted() && output.symbol_names.size() < 256 &&
         symbol_offset + (static_cast<std::uint64_t>(index) + 1) * 18U <= bytes.size();
         ++index) {
        const auto at = symbol_offset + static_cast<std::uint64_t>(index) * 18U;
        auto name = std::optional<std::string>{};
        if (integer<std::uint32_t>(bytes, at).value_or(1) == 0) {
            const auto name_offset = integer<std::uint32_t>(bytes, at + 4).value_or(0);
            name = name_bytes.read_fixed(string_table + name_offset, 160);
        } else {
            name = name_bytes.read_fixed(at, 8);
        }
        if (name && !name->empty()) {
            output.symbol_names.push_back(std::move(*name));
        }
        index += std::to_integer<unsigned char>(bytes[static_cast<std::size_t>(at + 17)]);
    }
    const auto directory_count =
        integer<std::uint32_t>(bytes, optional + (pe64 ? 108 : 92)).value_or(0);
    auto directory = [&](std::uint32_t index) -> std::pair<std::uint32_t, std::uint32_t> {
        if (index >= directory_count ||
            directories + static_cast<std::uint64_t>(index + 1) * 8 > optional + *optional_size) {
            return {};
        }
        return {integer<std::uint32_t>(bytes, directories + index * 8).value_or(0),
                integer<std::uint32_t>(bytes, directories + index * 8 + 4).value_or(0)};
    };
    const auto [export_rva, export_size] = directory(0);
    if (export_rva != 0 && export_size != 0) {
        const auto export_offset = rva_offset(sections, export_rva);
        if (!export_offset || *export_offset > bytes.size() || bytes.size() - *export_offset < 40) {
            return std::unexpected(malformed("invalid PE export directory"));
        }
        const auto name_rva = integer<std::uint32_t>(bytes, *export_offset + 12).value_or(0);
        const auto function_count = integer<std::uint32_t>(bytes, *export_offset + 20).value_or(0);
        const auto name_count = integer<std::uint32_t>(bytes, *export_offset + 24).value_or(0);
        const auto functions_rva = integer<std::uint32_t>(bytes, *export_offset + 28).value_or(0);
        const auto names_rva = integer<std::uint32_t>(bytes, *export_offset + 32).value_or(0);
        const auto ordinals_rva = integer<std::uint32_t>(bytes, *export_offset + 36).value_or(0);
        if (const auto name_offset = rva_offset(sections, name_rva)) {
            output.install_name = name_bytes.read(*name_offset, name_kind::install);
        }
        const auto functions = rva_offset(sections, functions_rva);
        const auto names = rva_offset(sections, names_rva);
        const auto ordinals = rva_offset(sections, ordinals_rva);
        if (name_count != 0 &&
            (!functions || !names || !ordinals || *functions > bytes.size() ||
             function_count * 4ULL > bytes.size() - *functions || *names > bytes.size() ||
             name_count * 4ULL > bytes.size() - *names || *ordinals > bytes.size() ||
             name_count * 2ULL > bytes.size() - *ordinals)) {
            return std::unexpected(malformed("invalid PE export tables"));
        }
        for (auto index = std::uint64_t{}; index < name_count && !name_bytes.exhausted(); ++index) {
            const auto symbol_name_rva =
                integer<std::uint32_t>(bytes, *names + index * 4).value_or(0);
            const auto ordinal = integer<std::uint16_t>(bytes, *ordinals + index * 2).value_or(0);
            if (ordinal >= function_count) {
                continue;
            }
            const auto function_rva =
                integer<std::uint32_t>(bytes, *functions + ordinal * 4).value_or(0);
            const auto name_offset = rva_offset(sections, symbol_name_rva);
            if (!name_offset) {
                continue;
            }
            auto name = name_bytes.read(*name_offset, name_kind::exported);
            if (!name || name->empty()) {
                continue;
            }
            const auto code = code_rva(sections, function_rva);
            output.exported_symbols.push_back({std::move(*name),
                                               {},
                                               code ? symbol_kind::function : symbol_kind::object,
                                               0,
                                               "global",
                                               "default",
                                               {}});
        }
    }
    const auto [import_rva, import_size] = directory(1);
    if (import_rva != 0 && import_size != 0) {
        auto import_offset = rva_offset(sections, import_rva);
        while (import_offset && !name_bytes.exhausted() && *import_offset + 20 <= bytes.size()) {
            const auto original_thunk = integer<std::uint32_t>(bytes, *import_offset).value_or(0);
            const auto name_rva = integer<std::uint32_t>(bytes, *import_offset + 12).value_or(0);
            const auto first_thunk = integer<std::uint32_t>(bytes, *import_offset + 16).value_or(0);
            if (original_thunk == 0 && name_rva == 0 && first_thunk == 0) {
                break;
            }
            if (const auto name_offset = rva_offset(sections, name_rva)) {
                auto name = name_bytes.read(*name_offset, name_kind::imported);
                if (name) {
                    output.needed_libraries.push_back(std::move(*name));
                }
            }
            *import_offset += 20;
        }
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
