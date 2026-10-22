#include "liquid/abi.hpp"

#include <algorithm>
#include <ranges>
#include <string_view>

#include "formats/binary/elf.hpp"
#include "formats/binary/macho.hpp"
#include "formats/binary/pe.hpp"

namespace liquid {
namespace {

auto binary_name(abi_binary_kind value) -> std::string_view {
    switch (value) {
    case abi_binary_kind::elf:
        return "elf";
    case abi_binary_kind::macho:
        return "mach-o";
    case abi_binary_kind::pe:
        return "pe";
    }
    return "unknown";
}

auto symbol_name(abi_symbol_kind value) -> std::string_view {
    return value == abi_symbol_kind::function ? "function" : "object";
}

auto verdict_name(abi_verdict value) -> std::string_view {
    switch (value) {
    case abi_verdict::compatible:
        return "compatible";
    case abi_verdict::compatible_additions:
        return "compatible_additions";
    case abi_verdict::incompatible:
        return "incompatible";
    }
    return "incompatible";
}

auto change_name(abi_change_kind value) -> std::string_view {
    switch (value) {
    case abi_change_kind::added:
        return "added";
    case abi_change_kind::removed:
        return "removed";
    case abi_change_kind::type_changed:
        return "type_changed";
    case abi_change_kind::size_changed:
        return "size_changed";
    case abi_change_kind::version_changed:
        return "version_changed";
    case abi_change_kind::inspection_truncated:
        return "inspection_truncated";
    }
    return "type_changed";
}

auto quoted(std::string_view value) -> std::string {
    auto output = std::string{"\""};
    for (const auto character : value) {
        switch (character) {
        case '\"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20U) {
                constexpr auto digits = std::string_view{"0123456789abcdef"};
                output += "\\u00";
                output.push_back(digits[static_cast<unsigned char>(character) >> 4U]);
                output.push_back(digits[static_cast<unsigned char>(character) & 0x0fU]);
            } else {
                output.push_back(character);
            }
        }
    }
    output.push_back('\"');
    return output;
}

auto convert(formats::binary::image parsed, digest subject) -> abi_descriptor {
    auto output = abi_descriptor{
        .subject = subject,
        .binary = parsed.kind == formats::binary::image_kind::elf     ? abi_binary_kind::elf
                  : parsed.kind == formats::binary::image_kind::macho ? abi_binary_kind::macho
                                                                      : abi_binary_kind::pe,
        .architecture = std::move(parsed.architecture),
        .install_name = parsed.shared_library ? std::move(parsed.install_name) : std::nullopt,
        .needed_libraries = std::move(parsed.needed_libraries),
        .truncated = parsed.truncated};
    output.exported_symbols.reserve(parsed.exported_symbols.size());
    for (auto& symbol : parsed.exported_symbols) {
        output.exported_symbols.push_back(
            {.name = std::move(symbol.name),
             .version = std::move(symbol.version),
             .kind = symbol.kind == formats::binary::symbol_kind::function
                         ? abi_symbol_kind::function
                         : abi_symbol_kind::object,
             .size = symbol.size,
             .binding = std::move(symbol.binding),
             .visibility = std::move(symbol.visibility),
             .signature = std::move(symbol.signature)});
    }
    return output;
}

auto version_text(const std::optional<std::string>& value) -> std::string {
    return value.value_or("<none>");
}

} // namespace

result<abi_descriptor> extract_abi(std::span<const std::byte> binary) {
    auto subject = hash(algorithm::blake3, binary);
    if (!subject) {
        return std::unexpected(subject.error());
    }
    if (binary.size() >= 4 && binary[0] == std::byte{0x7f} && binary[1] == std::byte{'E'} &&
        binary[2] == std::byte{'L'} && binary[3] == std::byte{'F'}) {
        auto parsed = formats::binary::inspect_elf(binary);
        return parsed ? result<abi_descriptor>{convert(std::move(*parsed), *subject)}
                      : result<abi_descriptor>{std::unexpected(parsed.error())};
    }
    if (binary.size() >= 4) {
        const auto a = std::to_integer<unsigned char>(binary[0]);
        const auto b = std::to_integer<unsigned char>(binary[1]);
        const auto c = std::to_integer<unsigned char>(binary[2]);
        const auto d = std::to_integer<unsigned char>(binary[3]);
        const auto magic = static_cast<std::uint32_t>(a) | (static_cast<std::uint32_t>(b) << 8U) |
                           (static_cast<std::uint32_t>(c) << 16U) |
                           (static_cast<std::uint32_t>(d) << 24U);
        if (magic == 0xfeedfaceU || magic == 0xfeedfacfU || magic == 0xcefaedfeU ||
            magic == 0xcffaedfeU) {
            auto parsed = formats::binary::inspect_macho(binary);
            return parsed ? result<abi_descriptor>{convert(std::move(*parsed), *subject)}
                          : result<abi_descriptor>{std::unexpected(parsed.error())};
        }
    }
    if (binary.size() >= 2 && binary[0] == std::byte{'M'} && binary[1] == std::byte{'Z'}) {
        auto parsed = formats::binary::inspect_pe(binary);
        return parsed ? result<abi_descriptor>{convert(std::move(*parsed), *subject)}
                      : result<abi_descriptor>{std::unexpected(parsed.error())};
    }
    return std::unexpected(error{errc::unsupported, "artifact is not ELF, Mach-O, or PE", "abi/1"});
}

abi_report diff_abi(const abi_descriptor& old_descriptor, const abi_descriptor& new_descriptor) {
    auto report = abi_report{};
    if (old_descriptor.truncated || new_descriptor.truncated) {
        report.verdict = abi_verdict::incompatible;
        report.changes.push_back({{},
                                  abi_change_kind::inspection_truncated,
                                  "symbol inspection was truncated; compatibility is unknown"});
        return report;
    }
    auto incompatible = false;
    auto additions = false;
    auto matched = std::vector<bool>(new_descriptor.exported_symbols.size());
    for (const auto& old_symbol : old_descriptor.exported_symbols) {
        auto selected = new_descriptor.exported_symbols.end();
        auto selected_index = std::size_t{};
        for (auto index = std::size_t{}; index < new_descriptor.exported_symbols.size(); ++index) {
            const auto& candidate = new_descriptor.exported_symbols[index];
            if (!matched[index] && candidate.name == old_symbol.name) {
                selected =
                    new_descriptor.exported_symbols.begin() + static_cast<std::ptrdiff_t>(index);
                selected_index = index;
                if (candidate.version == old_symbol.version) {
                    break;
                }
            }
        }
        if (selected == new_descriptor.exported_symbols.end()) {
            report.changes.push_back(
                {old_symbol.name, abi_change_kind::removed, "exported symbol was removed"});
            incompatible = true;
            continue;
        }
        matched[selected_index] = true;
        if (old_symbol.version != selected->version) {
            report.changes.push_back({old_symbol.name,
                                      abi_change_kind::version_changed,
                                      "symbol version changed from " +
                                          version_text(old_symbol.version) + " to " +
                                          version_text(selected->version)});
            incompatible = true;
        }
        if (old_symbol.kind != selected->kind) {
            report.changes.push_back({old_symbol.name,
                                      abi_change_kind::type_changed,
                                      "symbol kind changed from " +
                                          std::string{symbol_name(old_symbol.kind)} + " to " +
                                          std::string{symbol_name(selected->kind)}});
            incompatible = true;
        } else if (old_symbol.signature && selected->signature &&
                   old_symbol.signature != selected->signature) {
            report.changes.push_back({old_symbol.name,
                                      abi_change_kind::type_changed,
                                      "signature changed from " + *old_symbol.signature + " to " +
                                          *selected->signature});
            incompatible = true;
        }
        if (old_symbol.kind == abi_symbol_kind::object && old_symbol.size != 0 &&
            selected->size != 0 && old_symbol.size != selected->size) {
            report.changes.push_back({old_symbol.name,
                                      abi_change_kind::size_changed,
                                      "object size changed from " +
                                          std::to_string(old_symbol.size) + " to " +
                                          std::to_string(selected->size)});
            incompatible = true;
        }
    }
    for (auto index = std::size_t{}; index < new_descriptor.exported_symbols.size(); ++index) {
        if (!matched[index]) {
            report.changes.push_back({new_descriptor.exported_symbols[index].name,
                                      abi_change_kind::added,
                                      "exported symbol was added"});
            additions = true;
        }
    }
    std::ranges::stable_sort(report.changes, [](const auto& left, const auto& right) {
        return std::pair{left.symbol, static_cast<unsigned>(left.kind)} <
               std::pair{right.symbol, static_cast<unsigned>(right.kind)};
    });
    report.verdict = incompatible ? abi_verdict::incompatible
                     : additions  ? abi_verdict::compatible_additions
                                  : abi_verdict::compatible;
    return report;
}

result<std::string> abi_json(const abi_descriptor& descriptor) {
    auto output = std::string{"{\"version\":1,\"subject\":"} +
                  quoted(to_string(descriptor.subject)) +
                  ",\"binary\":" + quoted(binary_name(descriptor.binary)) +
                  ",\"architecture\":" + quoted(descriptor.architecture) + ",\"install_name\":" +
                  (descriptor.install_name ? quoted(*descriptor.install_name) : "null") +
                  ",\"exported_symbols\":[";
    for (auto index = std::size_t{}; index < descriptor.exported_symbols.size(); ++index) {
        const auto& symbol = descriptor.exported_symbols[index];
        if (index != 0) {
            output.push_back(',');
        }
        output += "{\"name\":" + quoted(symbol.name) +
                  ",\"version\":" + (symbol.version ? quoted(*symbol.version) : "null") +
                  ",\"kind\":" + quoted(symbol_name(symbol.kind)) +
                  ",\"size\":" + std::to_string(symbol.size) +
                  ",\"binding\":" + quoted(symbol.binding) +
                  ",\"visibility\":" + quoted(symbol.visibility) +
                  ",\"signature\":" + (symbol.signature ? quoted(*symbol.signature) : "null") + "}";
    }
    output += "],\"needed_libraries\":[";
    for (auto index = std::size_t{}; index < descriptor.needed_libraries.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        output += quoted(descriptor.needed_libraries[index]);
    }
    output += descriptor.truncated ? "],\"truncated\":true}" : "],\"truncated\":false}";
    return output;
}

result<std::string> abi_json(const abi_report& report) {
    auto output = std::string{"{\"version\":1,\"verdict\":"} +
                  quoted(verdict_name(report.verdict)) + ",\"changes\":[";
    for (auto index = std::size_t{}; index < report.changes.size(); ++index) {
        const auto& change = report.changes[index];
        if (index != 0) {
            output.push_back(',');
        }
        output += "{\"symbol\":" + quoted(change.symbol) +
                  ",\"kind\":" + quoted(change_name(change.kind)) +
                  ",\"detail\":" + quoted(change.detail) + "}";
    }
    output += "]}";
    return output;
}

} // namespace liquid
