// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "formats/binary/pe.hpp"
#include "pe_fixture.hpp"

namespace {

using pe_test::pe_names_fixture;

TEST(PeInspector, rejects_oversized_name_copies_on_every_path) {
    for (const auto path : {0, 1, 2}) {
        for (const auto terminated : {true, false}) {
            SCOPED_TRACE(::testing::Message() << path << "/" << terminated);
            auto fixture = pe_names_fixture{
                path == 1 ? 1U : 0U, path == 2 ? 1U : 0U, terminated ? 65538U : 65537U};
            fixture.fill_name(65537);
            if (path == 0) {
                fixture.put32(0x200 + 12, pe_names_fixture::rva(fixture.strings));
            }
            const auto inspected = liquid::formats::binary::inspect_pe(fixture.bytes);
            ASSERT_TRUE(inspected) << inspected.error().message;
            EXPECT_TRUE(inspected->truncated);
            EXPECT_GT(inspected->symbol_bytes, 65536);
            EXPECT_LE(inspected->symbol_bytes, 16U * 1024U * 1024U);
            if (path == 0) {
                ASSERT_TRUE(inspected->install_name);
                EXPECT_EQ(inspected->install_name->size(), 65536);
            } else if (path == 1) {
                ASSERT_EQ(inspected->exported_symbols.size(), 1);
                EXPECT_EQ(inspected->exported_symbols.front().name.size(), 65536);
            } else {
                ASSERT_EQ(inspected->needed_libraries.size(), 1);
                EXPECT_EQ(inspected->needed_libraries.front().size(), 65536);
            }
        }
    }
}

TEST(PeInspector, deduplicates_export_and_import_offsets_before_copying) {
    auto fixture = pe_names_fixture{100'000, 100'000, 65537};
    fixture.fill_name(65536);
    auto inspected = liquid::formats::binary::inspect_pe(fixture.bytes);
    ASSERT_TRUE(inspected) << inspected.error().message;
    ASSERT_EQ(inspected->exported_symbols.size(), 1);
    ASSERT_EQ(inspected->needed_libraries.size(), 1);
    EXPECT_EQ(inspected->exported_symbols.front().name.size(), 65536);
    EXPECT_EQ(inspected->needed_libraries.front().size(), 65536);
    EXPECT_FALSE(inspected->truncated);
    EXPECT_GT(inspected->symbol_bytes, 2U * 65536U);
    EXPECT_LT(inspected->symbol_bytes, 140U * 1024U);
}

TEST(PeInspector, bounds_the_reported_repeated_megabyte_name) {
    auto fixture = pe_names_fixture{100'000, 0, 1024 * 1024 + 1};
    fixture.fill_name(1024 * 1024);
    const auto inspected = liquid::formats::binary::inspect_pe(fixture.bytes);
    ASSERT_TRUE(inspected) << inspected.error().message;
    ASSERT_EQ(inspected->exported_symbols.size(), 1);
    EXPECT_EQ(inspected->exported_symbols.front().name.size(), 65536);
    EXPECT_TRUE(inspected->truncated);
    EXPECT_LT(inspected->symbol_bytes, 70U * 1024U);
}

TEST(PeInspector, charges_distinct_offsets_to_one_budget) {
    // Distinct suffix offsets bypass offset deduplication. Each name fits the length cap, but
    // the 8192 suffix copies plus their records exceed the default 16 MiB allowance.
    for (const auto imports : {false, true}) {
        SCOPED_TRACE(imports);
        auto fixture = pe_names_fixture{imports ? 0U : 8192U, imports ? 8192U : 0U, 8194};
        fixture.fill_name(8193);
        fixture.bytes[fixture.strings + 4096] = std::byte{};
        fixture.bytes[fixture.strings + 8193] = std::byte{};
        std::fill_n(fixture.bytes.begin() + static_cast<std::ptrdiff_t>(fixture.strings + 4097),
                    4096,
                    std::byte{'b'});
        for (auto index = 0U; index < 8192; ++index) {
            const auto offset = fixture.strings + index + index / 4096;
            fixture.put32(imports ? fixture.imports + index * 20 + 12 : fixture.names + index * 4,
                          pe_names_fixture::rva(offset));
        }
        const auto inspected = liquid::formats::binary::inspect_pe(fixture.bytes);
        ASSERT_TRUE(inspected) << inspected.error().message;
        const auto count =
            imports ? inspected->needed_libraries.size() : inspected->exported_symbols.size();
        EXPECT_GT(count, 0);
        EXPECT_LT(count, 8192);
        EXPECT_TRUE(inspected->truncated);
        EXPECT_LE(inspected->symbol_bytes, 16U * 1024U * 1024U);
    }
}

TEST(PeInspector, shares_the_callers_name_budget_between_exports_and_imports) {
    auto fixture = pe_names_fixture{32, 32, 4097};
    fixture.fill_name(4096);
    for (auto index = 0U; index < 32; ++index) {
        const auto offset = pe_names_fixture::rva(fixture.strings + index);
        fixture.put32(fixture.names + index * 4, offset);
        fixture.put32(fixture.imports + index * 20 + 12, offset);
    }
    for (const auto budget : {0ULL, 65536ULL, 1048576ULL}) {
        SCOPED_TRACE(budget);
        const auto inspected = liquid::formats::binary::inspect_pe(fixture.bytes, budget);
        ASSERT_TRUE(inspected) << inspected.error().message;
        const auto names = inspected->exported_symbols.size() + inspected->needed_libraries.size();
        EXPECT_LE(inspected->symbol_bytes, budget);
        if (budget == 1048576) {
            EXPECT_FALSE(inspected->truncated);
            EXPECT_EQ(names, 64);
        } else {
            EXPECT_TRUE(inspected->truncated);
            EXPECT_LT(names, 64);
            if (budget == 0) {
                EXPECT_EQ(names, 0);
            } else {
                EXPECT_GT(names, 0);
            }
        }
    }
}

TEST(PeInspector, preserves_utf8_at_the_name_cap) {
    auto fixture = pe_names_fixture{1, 0, 65539};
    fixture.fill_name(65535);
    fixture.bytes[fixture.strings + 65535] = std::byte{0xe2};
    fixture.bytes[fixture.strings + 65536] = std::byte{0x82};
    fixture.bytes[fixture.strings + 65537] = std::byte{0xac};
    const auto inspected = liquid::formats::binary::inspect_pe(fixture.bytes);
    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_TRUE(inspected->truncated);
    ASSERT_EQ(inspected->exported_symbols.size(), 1);
    EXPECT_EQ(inspected->exported_symbols.front().name.size(), 65535);
}

TEST(PeInspector, bounds_pe32_export_and_import_names) {
    auto fixture = pe_names_fixture{1, 1, 65538};
    fixture.fill_name(65537);
    fixture.bytes[0x84] = std::byte{0x4c};
    fixture.bytes[0x85] = std::byte{0x01};
    fixture.bytes[0x99] = std::byte{0x01};
    fixture.put32(0x98 + 92, 16);
    fixture.put32(0x98 + 96, 0x1000);
    fixture.put32(0x98 + 100, 40);
    fixture.put32(0x98 + 104, pe_names_fixture::rva(fixture.imports));
    fixture.put32(0x98 + 108, 40);
    const auto inspected = liquid::formats::binary::inspect_pe(fixture.bytes);
    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_EQ(inspected->architecture, "x86");
    EXPECT_TRUE(inspected->truncated);
    ASSERT_EQ(inspected->exported_symbols.size(), 1);
    ASSERT_EQ(inspected->needed_libraries.size(), 1);
    EXPECT_EQ(inspected->exported_symbols.front().name.size(), 65536);
    EXPECT_EQ(inspected->needed_libraries.front().size(), 65536);
}

TEST(PeInspector, charges_coff_evidence_before_copying) {
    auto fixture = pe_names_fixture{0, 0, 512};
    fixture.put32(0x80 + 12, static_cast<std::uint32_t>(fixture.strings));
    fixture.put32(0x80 + 16, 2);
    const auto short_name = std::string{"short"};
    const auto long_name = std::string{"coff-long-name"};
    for (auto index = std::size_t{}; index < short_name.size(); ++index) {
        fixture.bytes[fixture.strings + index] = static_cast<std::byte>(short_name[index]);
    }
    fixture.put32(fixture.strings + 18 + 4, 4);
    for (auto index = std::size_t{}; index < long_name.size(); ++index) {
        fixture.bytes[fixture.strings + 36 + 4 + index] = static_cast<std::byte>(long_name[index]);
    }
    for (const auto budget : {0ULL, 80ULL, 1024ULL}) {
        SCOPED_TRACE(budget);
        const auto inspected = liquid::formats::binary::inspect_pe(fixture.bytes, budget);
        ASSERT_TRUE(inspected) << inspected.error().message;
        EXPECT_LE(inspected->symbol_bytes, budget);
        EXPECT_EQ(inspected->symbol_names.size(), budget == 0 ? 0U : budget == 80 ? 1U : 2U);
        EXPECT_EQ(inspected->truncated, budget != 1024);
        if (budget == 1024) {
            EXPECT_EQ(inspected->symbol_names, (std::vector{short_name, long_name}));
            EXPECT_GT(inspected->symbol_bytes, short_name.size() + long_name.size());
        }
    }
}

} // namespace
