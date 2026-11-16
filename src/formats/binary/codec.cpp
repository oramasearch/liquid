// SPDX-License-Identifier: MIT

#include "formats/builtin.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <string>

#include "formats/binary/elf.hpp"
#include "formats/binary/macho.hpp"
#include "formats/binary/pe.hpp"

namespace liquid::formats::detail {
namespace {

using reader = auto (*)(std::span<const std::byte>) -> result<binary::image>;
using budgeted_reader = auto (*)(std::span<const std::byte>, std::uint64_t)
    -> result<binary::image>;

class binary_backend final : public codec_backend {
public:
    binary_backend(std::string name, reader inspect, budgeted_reader inspect_budgeted)
        : name_(std::move(name)), inspect_(inspect), inspect_budgeted_(inspect_budgeted) {}

    auto recognise(std::span<const std::byte> blob, std::string_view)
        -> result<std::optional<format_recognition>> override {
        auto image = inspect_(blob);
        if (!image) {
            return std::optional<format_recognition>{};
        }
        return std::optional<format_recognition>{
            format_recognition{format_tier::semantic_verbatim, "sections and linked symbols"}};
    }

    auto unpack(std::span<const std::byte> blob, format_limits limits)
        -> result<format_tree> override {
        auto image =
            inspect_budgeted_ ? inspect_budgeted_(blob, limits.byte_budget) : inspect_(blob);
        if (!image) {
            return std::unexpected(image.error());
        }
        auto output = format_tree{.codec = name_,
                                  .tier = format_tier::semantic_verbatim,
                                  .original = {blob.begin(), blob.end()},
                                  .truncated = image->truncated};
        if (image->truncated) {
            output.limits.push_back("linked-binary symbol name or byte limit reached");
        }
        auto remaining = limits.byte_budget - std::min(limits.byte_budget, image->symbol_bytes);
        for (const auto& section : image->sections) {
            if (section.size > remaining || section.offset > blob.size() ||
                section.size > blob.size() - section.offset) {
                output.truncated = true;
                output.limits.push_back("byte budget reached at sections/" + section.name);
                break;
            }
            remaining -= section.size;
            auto attributes = detail::attributes({{"address", std::to_string(section.address)},
                                                  {"flags", std::to_string(section.flags)},
                                                  {"type", std::to_string(section.type)}});
            if (!attributes) {
                return std::unexpected(attributes.error());
            }
            auto entry = make_entry(
                "sections/" + section.name,
                format_entry_kind::member,
                {blob.begin() + static_cast<std::ptrdiff_t>(section.offset),
                 blob.begin() + static_cast<std::ptrdiff_t>(section.offset + section.size)},
                std::move(*attributes),
                format_byte_range{section.offset, section.size});
            if (!entry) {
                return std::unexpected(entry.error());
            }
            output.entries.push_back(std::move(*entry));
        }
        for (const auto& symbol : image->exported_symbols) {
            if (inspect_budgeted_) {
                const auto charge = sizeof(format_entry) + 8U + symbol.name.size();
                if (charge > remaining) {
                    output.truncated = true;
                    output.limits.push_back("byte budget reached at exports");
                    break;
                }
                remaining -= charge;
            }
            auto attributes = detail::attributes(
                {{"binding", symbol.binding},
                 {"kind", symbol.kind == binary::symbol_kind::function ? "function" : "object"},
                 {"visibility", symbol.visibility},
                 {"version", symbol.version.value_or("")}});
            if (!attributes) {
                return std::unexpected(attributes.error());
            }
            auto entry = make_entry(
                "exports/" + symbol.name, format_entry_kind::attribute, {}, std::move(*attributes));
            if (!entry) {
                return std::unexpected(entry.error());
            }
            output.entries.push_back(std::move(*entry));
        }
        for (const auto& library : image->needed_libraries) {
            auto entry = make_entry("imports/" + library, format_entry_kind::attribute, {});
            if (!entry) {
                return std::unexpected(entry.error());
            }
            output.entries.push_back(std::move(*entry));
        }
        std::ranges::sort(output.entries, {}, &format_entry::path);
        return output;
    }

    auto volatile_fields() const -> std::span<const std::string_view> override {
        return volatile_fields_;
    }

private:
    std::string name_;
    reader inspect_;
    budgeted_reader inspect_budgeted_;
    static constexpr auto volatile_fields_ = std::array<std::string_view, 1>{"load_address"};
};

template <reader Reader, budgeted_reader BudgetedReader = nullptr>
auto module(std::string_view name,
            const char* const* extensions,
            std::size_t extension_count,
            const liquid_format_magic_v1* magic,
            std::size_t magic_count) -> const liquid_format_module_v1* {
    static auto backend = binary_backend{std::string{name}, Reader, BudgetedReader};
    static auto name_storage = std::string{name};
    static const auto volatile_names = std::array<const char*, 1>{"load_address"};
    static const auto value =
        liquid_format_module_v1{.abi_version = LIQUID_FORMAT_ABI_VERSION,
                                .struct_size = sizeof(liquid_format_module_v1),
                                .descriptor = {.name = name_storage.c_str(),
                                               .tier = 2,
                                               .extensions = extensions,
                                               .extension_count = extension_count,
                                               .magic = magic,
                                               .magic_count = magic_count,
                                               .volatile_fields = volatile_names.data(),
                                               .volatile_field_count = volatile_names.size(),
                                               .limits = {.maximum_depth = 1,
                                                          .byte_budget = 512ULL * 1024ULL * 1024ULL,
                                                          .maximum_entries = 100'000}},
                                .module_context = &backend,
                                .recognise = recognise_operation,
                                .unpack = unpack_operation,
                                .canonicalise = canonicalise_operation,
                                .diff = diff_operation,
                                .merge = nullptr,
                                .repack = nullptr,
                                .destroy = nullptr};
    return &value;
}

constexpr auto elf_extensions = std::array<const char*, 2>{".elf", ".so"};
constexpr auto macho_extensions = std::array<const char*, 2>{".dylib", ".macho"};
constexpr auto pe_extensions = std::array<const char*, 2>{".exe", ".dll"};
constexpr auto elf_bytes = std::array<std::uint8_t, 4>{0x7f, 'E', 'L', 'F'};
constexpr auto pe_bytes = std::array<std::uint8_t, 2>{'M', 'Z'};
constexpr auto macho_le_bytes = std::array<std::uint8_t, 4>{0xcf, 0xfa, 0xed, 0xfe};
constexpr auto macho_be_bytes = std::array<std::uint8_t, 4>{0xfe, 0xed, 0xfa, 0xcf};
constexpr auto elf_magic = std::array{liquid_format_magic_v1{elf_bytes.data(), elf_bytes.size()}};
constexpr auto pe_magic = std::array{liquid_format_magic_v1{pe_bytes.data(), pe_bytes.size()}};
constexpr auto macho_magic =
    std::array{liquid_format_magic_v1{macho_le_bytes.data(), macho_le_bytes.size()},
               liquid_format_magic_v1{macho_be_bytes.data(), macho_be_bytes.size()}};

} // namespace

auto elf_module() -> const liquid_format_module_v1* {
    return module<binary::inspect_elf>(
        "elf", elf_extensions.data(), elf_extensions.size(), elf_magic.data(), elf_magic.size());
}

auto macho_module() -> const liquid_format_module_v1* {
    return module<static_cast<reader>(binary::inspect_macho), binary::inspect_macho>(
        "macho",
        macho_extensions.data(),
        macho_extensions.size(),
        macho_magic.data(),
        macho_magic.size());
}

auto pe_module() -> const liquid_format_module_v1* {
    return module<binary::inspect_pe>(
        "pe", pe_extensions.data(), pe_extensions.size(), pe_magic.data(), pe_magic.size());
}

} // namespace liquid::formats::detail
