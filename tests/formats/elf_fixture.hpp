// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace elf_test {

struct elf_fixture {
    struct section {
        std::string name;
        std::vector<std::byte> data;
        std::uint32_t type{1};
        std::uint32_t link{};
        std::uint64_t stride{};
    };

    bool is64{true};
    bool little{true};
    std::vector<section> sections{{"", {}, 0}, {".shstrtab", {}, 3}};

    auto
    put(std::vector<std::byte>& bytes, std::size_t at, std::uint64_t value, std::size_t width) const
        -> void {
        for (auto index = std::size_t{}; index < width; ++index) {
            bytes[at + (little ? index : width - index - 1)] =
                static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
        }
    }

    auto add(std::string name,
             std::vector<std::byte> data,
             std::uint32_t type = 1,
             std::uint32_t link = 0,
             std::uint64_t stride = 0) -> std::uint32_t {
        sections.push_back({std::move(name), std::move(data), type, link, stride});
        return static_cast<std::uint32_t>(sections.size() - 1);
    }

    static auto name(std::size_t length) -> std::vector<std::byte> {
        auto bytes = std::vector<std::byte>(length + 1, std::byte{'a'});
        bytes.back() = std::byte{};
        return bytes;
    }

    auto symbols(std::size_t count, std::size_t length, bool suffixes = false) -> void {
        const auto strings = add(".dynstr", name(length), 3);
        const auto stride = is64 ? 24U : 16U;
        auto table = std::vector<std::byte>(count * stride);
        for (auto index = std::size_t{}; index < count; ++index) {
            put(table, index * stride, suffixes ? index % length : 0, 4);
            put(table, index * stride + (is64 ? 4 : 12), 0x12, 1);
            put(table, index * stride + (is64 ? 6 : 14), 1, 2);
        }
        add(".dynsym", std::move(table), 11, strings, stride);
    }

    auto dynamic(std::uint64_t tag, std::size_t count, std::size_t length) -> void {
        const auto strings = add(".dynstr", name(length), 3);
        const auto stride = is64 ? 16U : 8U;
        auto table = std::vector<std::byte>((count + 1) * stride);
        for (auto index = std::size_t{}; index < count; ++index) {
            put(table, index * stride, tag, stride / 2);
        }
        add(".dynamic", std::move(table), 6, strings, stride);
    }

    auto versions(bool requirements, std::size_t count, std::size_t length) -> void {
        add(".dynstr", name(length), 3);
        const auto stride = requirements ? 32U : 28U;
        auto table = std::vector<std::byte>(count * stride);
        for (auto index = std::size_t{}; index < count; ++index) {
            const auto at = index * stride;
            if (requirements) {
                put(table, at + 2, 1, 2);
                put(table, at + 8, 16, 4);
                put(table, at + 12, index + 1 < count ? stride : 0, 4);
                put(table, at + 16 + 6, index + 2, 2);
            } else {
                put(table, at + 4, index + 2, 2);
                put(table, at + 12, 20, 4);
                put(table, at + 16, index + 1 < count ? stride : 0, 4);
            }
        }
        add(requirements ? ".gnu.version_r" : ".gnu.version_d", std::move(table));
    }

    auto dwarf(std::uint8_t form, std::size_t count, std::size_t length) -> void {
        add(form == 0x1f ? ".debug_line_str" : ".debug_str", name(length));
        // Abbreviation 1: a subprogram with a name; abbreviation 2: a base type.
        add(".debug_abbrev",
            {std::byte{1},
             std::byte{0x2e},
             std::byte{},
             std::byte{3},
             std::byte{form},
             std::byte{},
             std::byte{},
             std::byte{}});
        if (form == 0x1a || (form >= 0x25 && form <= 0x28)) {
            add(".debug_str_offsets", std::vector<std::byte>(12));
        }
        auto info = std::vector<std::byte>(11);
        put(info, 4, 4, 2);
        put(info, 10, 8, 1);
        for (auto index = std::size_t{}; index < count; ++index) {
            info.push_back(std::byte{1});
            if (form == 0x08) {
                auto value = name(length);
                info.insert(info.end(), value.begin(), value.end());
            } else {
                if (form == 0x16) {
                    info.push_back(std::byte{0x0e});
                }
                info.resize(info.size() + (form == 0x1a                   ? 1
                                           : form >= 0x25 && form <= 0x28 ? form - 0x24
                                                                          : 4));
            }
        }
        put(info, 0, info.size() - 4, 4);
        add(".debug_info", std::move(info));
    }

    // A named function whose return and parameter types reuse one long DWARF string.
    auto dwarf_signature(std::size_t parameters, std::size_t type_length) -> void {
        auto strings = std::vector<std::byte>{std::byte{'a'}, std::byte{}};
        const auto type_name = name(type_length);
        strings.insert(strings.end(), type_name.begin(), type_name.end());
        add(".debug_str", std::move(strings));
        add(".debug_abbrev",
            {std::byte{1},    std::byte{0x2e}, std::byte{1},    std::byte{3},    std::byte{0x0e},
             std::byte{0x49}, std::byte{0x13}, std::byte{},     std::byte{},     std::byte{2},
             std::byte{5},    std::byte{},     std::byte{0x49}, std::byte{0x13}, std::byte{},
             std::byte{},     std::byte{3},    std::byte{0x24}, std::byte{},     std::byte{3},
             std::byte{0x0e}, std::byte{},     std::byte{},     std::byte{}});
        const auto type_at = 11 + 9 + parameters * 5 + 1;
        auto info = std::vector<std::byte>(type_at + 5);
        put(info, 0, info.size() - 4, 4);
        put(info, 4, 4, 2);
        put(info, 10, 8, 1);
        put(info, 11, 1, 1);
        put(info, 16, type_at, 4);
        for (auto index = std::size_t{}; index < parameters; ++index) {
            put(info, 20 + index * 5, 2, 1);
            put(info, 21 + index * 5, type_at, 4);
        }
        put(info, type_at, 3, 1);
        put(info, type_at + 1, 2, 4);
        add(".debug_info", std::move(info));
    }

    auto bytes() -> std::vector<std::byte> {
        auto names = std::map<std::string, std::uint32_t>{};
        auto& table = sections[1].data;
        table.clear();
        for (const auto& section : sections) {
            if (!names.contains(section.name)) {
                names[section.name] = static_cast<std::uint32_t>(table.size());
                for (const auto character : section.name) {
                    table.push_back(static_cast<std::byte>(character));
                }
                table.push_back(std::byte{});
            }
        }
        const auto stride = is64 ? 64U : 40U;
        auto output = std::vector<std::byte>(64 + sections.size() * stride);
        output[0] = std::byte{0x7f};
        output[1] = std::byte{'E'};
        output[2] = std::byte{'L'};
        output[3] = std::byte{'F'};
        output[4] = is64 ? std::byte{2} : std::byte{1};
        output[5] = little ? std::byte{1} : std::byte{2};
        put(output, 16, 3, 2);
        put(output, 18, is64 ? 62 : 3, 2);
        put(output, is64 ? 40 : 32, 64, is64 ? 8 : 4);
        put(output, is64 ? 58 : 46, stride, 2);
        put(output, is64 ? 60 : 48, sections.size(), 2);
        put(output, is64 ? 62 : 50, 1, 2);
        for (auto index = std::size_t{}; index < sections.size(); ++index) {
            const auto& section = sections[index];
            const auto at = 64 + index * stride;
            put(output, at, names.at(section.name), 4);
            put(output, at + 4, section.type, 4);
            put(output, at + (is64 ? 24 : 16), output.size(), is64 ? 8 : 4);
            put(output, at + (is64 ? 32 : 20), section.data.size(), is64 ? 8 : 4);
            put(output, at + (is64 ? 40 : 24), section.link, 4);
            put(output, at + (is64 ? 56 : 36), section.stride, is64 ? 8 : 4);
            output.insert(output.end(), section.data.begin(), section.data.end());
        }
        return output;
    }
};

} // namespace elf_test
