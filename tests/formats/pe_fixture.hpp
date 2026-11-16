#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace pe_test {

inline auto read_pe_fixture() -> std::vector<std::byte> {
    auto input = std::ifstream{std::filesystem::path{LIQUID_PE_FIXTURES} / "pe.hex"};
    EXPECT_TRUE(input);
    auto output = std::vector<std::byte>{};
    auto high = -1;
    for (char character; input.get(character);) {
        const auto digit = character >= '0' && character <= '9'   ? character - '0'
                           : character >= 'a' && character <= 'f' ? character - 'a' + 10
                                                                  : -1;
        if (digit < 0) {
            continue;
        }
        if (high < 0) {
            high = digit;
        } else {
            output.push_back(std::byte{static_cast<unsigned char>((high << 4) | digit)});
            high = -1;
        }
    }
    EXPECT_EQ(high, -1);
    return output;
}

// One section containing export/import tables and caller-controlled name storage.
struct pe_names_fixture {
    std::vector<std::byte> bytes = read_pe_fixture();
    std::size_t names;
    std::size_t imports;
    std::size_t strings;

    auto put32(std::size_t offset, std::uint32_t value) -> void {
        for (auto index = 0U; index < 4; ++index) {
            bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
        }
    }

    static auto rva(std::size_t offset) -> std::uint32_t {
        return static_cast<std::uint32_t>(offset + 0xe00);
    }

    pe_names_fixture(std::size_t exports, std::size_t libraries, std::size_t string_bytes)
        : names(0x280), imports(names + exports * 6), strings(imports + (libraries + 1) * 20) {
        bytes.resize(strings + string_bytes);
        std::fill(bytes.begin() + 0x200, bytes.end(), std::byte{});
        put32(0x188 + 8, static_cast<std::uint32_t>(bytes.size() - 0x200));
        put32(0x188 + 16, static_cast<std::uint32_t>(bytes.size() - 0x200));
        put32(0x98 + 112, 0x1000);
        put32(0x98 + 116, 40);
        put32(0x98 + 120, rva(imports));
        put32(0x98 + 124, static_cast<std::uint32_t>((libraries + 1) * 20));
        put32(0x200 + 20, 1);
        put32(0x200 + 24, static_cast<std::uint32_t>(exports));
        put32(0x200 + 28, rva(0x240));
        put32(0x200 + 32, rva(names));
        put32(0x200 + 36, rva(names + exports * 4));
        put32(0x240, 0x1000);
        for (auto index = std::size_t{}; index < exports; ++index) {
            put32(names + index * 4, rva(strings));
        }
        for (auto index = std::size_t{}; index < libraries; ++index) {
            put32(imports + index * 20, 1);
            put32(imports + index * 20 + 12, rva(strings));
        }
    }

    auto fill_name(std::size_t length) -> void {
        std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(strings), length, std::byte{'a'});
    }
};

} // namespace pe_test
