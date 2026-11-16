// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <array>

#include "elf_fixture.hpp"
#include "formats/binary/elf.hpp"

namespace {

using elf_test::elf_fixture;
using liquid::formats::binary::inspect_elf;

TEST(ElfInspector, stops_before_copying_an_oversized_name_on_every_path) {
    for (const auto is64 : {false, true}) {
        for (const auto little : {false, true}) {
            for (auto path = 0; path < 16; ++path) {
                SCOPED_TRACE(::testing::Message() << is64 << '/' << little << '/' << path);
                auto fixture = elf_fixture{.is64 = is64, .little = little};
                if (path == 0) {
                    fixture.add(std::string(65537, 'a'), {});
                } else if (path == 1) {
                    fixture.symbols(1, 65537);
                } else if (path < 6) {
                    fixture.dynamic(path == 2 ? 1 : path == 3 ? 14 : path == 4 ? 15 : 29, 1, 65537);
                } else if (path < 8) {
                    fixture.versions(path == 7, 1, 65537);
                } else {
                    const auto forms = std::array{0x08, 0x0e, 0x1f, 0x1a, 0x16, 0x25, 0x26, 0x28};
                    fixture.dwarf(forms[path - 8], 1, 65537);
                }
                const auto parsed = inspect_elf(fixture.bytes());
                ASSERT_TRUE(parsed) << parsed.error().message;
                EXPECT_TRUE(parsed->truncated);
                EXPECT_TRUE(parsed->exported_symbols.empty());
                EXPECT_TRUE(parsed->path_references.empty());
                for (const auto& section : parsed->sections) {
                    EXPECT_LE(section.name.size(), 65536);
                }
            }
        }
    }
}

TEST(ElfInspector, charges_repeated_and_overlapping_symbol_offsets_to_one_default_budget) {
    for (const auto suffixes : {false, true}) {
        auto fixture = elf_fixture{};
        fixture.symbols(10000, 4096, suffixes);
        const auto parsed = inspect_elf(fixture.bytes());
        ASSERT_TRUE(parsed) << parsed.error().message;
        EXPECT_TRUE(parsed->truncated);
        EXPECT_GT(parsed->exported_symbols.size(), 0);
        EXPECT_LT(parsed->exported_symbols.size(), 10000);
        EXPECT_LE(parsed->symbol_bytes, 16U * 1024U * 1024U);
    }
}

TEST(ElfInspector, charges_repeated_section_name_copies_including_the_output_copy) {
    auto fixture = elf_fixture{};
    for (auto index = 0; index < 3000; ++index) {
        fixture.add(std::string(4096, 'a'), {});
    }
    const auto parsed = inspect_elf(fixture.bytes());
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_TRUE(parsed->truncated);
    EXPECT_GT(parsed->sections.size(), 0);
    EXPECT_LT(parsed->sections.size(), fixture.sections.size());
    EXPECT_LE(parsed->symbol_bytes, 16U * 1024U * 1024U);
}

TEST(ElfInspector, all_repeated_metadata_paths_share_the_callers_budget) {
    for (auto path = 0; path < 9; ++path) {
        auto fixture = elf_fixture{};
        if (path == 0) {
            fixture.symbols(64, 4096);
        } else if (path < 5) {
            fixture.dynamic(path == 1 ? 1 : path == 2 ? 14 : path == 3 ? 15 : 29, 64, 4096);
        } else if (path < 7) {
            fixture.versions(path == 6, 64, 4096);
        } else {
            fixture.dwarf(path == 7 ? 0x0e : 0x1a, 64, 4096);
        }
        const auto bytes = fixture.bytes();
        for (const auto budget : {0ULL, 32768ULL, 1048576ULL}) {
            SCOPED_TRACE(::testing::Message() << path << '/' << budget);
            const auto parsed = inspect_elf(bytes, budget);
            ASSERT_TRUE(parsed) << parsed.error().message;
            EXPECT_LE(parsed->symbol_bytes, budget);
            EXPECT_EQ(parsed->truncated, budget != 1048576);
            if (budget == 0) {
                EXPECT_TRUE(parsed->sections.empty());
                EXPECT_TRUE(parsed->exported_symbols.empty());
                EXPECT_TRUE(parsed->path_references.empty());
            }
        }
    }
}

TEST(ElfInspector, accepts_exact_length_names_and_stops_on_unterminated_long_names) {
    auto fixture = elf_fixture{};
    fixture.symbols(1, 65536);
    auto parsed = inspect_elf(fixture.bytes());
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_FALSE(parsed->truncated);
    ASSERT_EQ(parsed->exported_symbols.size(), 1);
    EXPECT_EQ(parsed->exported_symbols.front().name.size(), 65536);
    fixture.sections[2].data.back() = std::byte{'a'};
    parsed = inspect_elf(fixture.bytes());
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_TRUE(parsed->truncated);
    EXPECT_TRUE(parsed->exported_symbols.empty());
}

TEST(ElfInspector, retains_complete_symbols_and_dynamic_references_within_budget) {
    auto fixture = elf_fixture{};
    fixture.symbols(2, 16);
    fixture.dynamic(1, 1, 8);
    const auto parsed = inspect_elf(fixture.bytes(), 32768);
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_FALSE(parsed->truncated);
    ASSERT_EQ(parsed->exported_symbols.size(), 2);
    EXPECT_EQ(parsed->needed_libraries, std::vector{std::string(8, 'a')});
    ASSERT_EQ(parsed->path_references.size(), 1);
    EXPECT_EQ(parsed->path_references.front().value, parsed->needed_libraries.front());
    EXPECT_GT(parsed->symbol_bytes, 0);
    EXPECT_LE(parsed->symbol_bytes, 32768);
}

TEST(ElfInspector, charges_version_and_signature_copies_for_each_export) {
    for (const auto signatures : {false, true}) {
        auto fixture = elf_fixture{};
        fixture.symbols(64, 1);
        if (signatures) {
            fixture.dwarf_signature(0, 4096);
        } else {
            auto& strings = fixture.sections[2].data;
            const auto name = elf_fixture::name(4096);
            strings.insert(strings.end(), name.begin(), name.end());
            fixture.versions(false, 1, 1);
            fixture.put(fixture.sections.back().data, 20, 2, 4);
            auto versions = std::vector<std::byte>(128);
            for (auto index = 0; index < 64; ++index) {
                fixture.put(versions, index * 2, 2, 2);
            }
            fixture.add(".gnu.version", std::move(versions));
        }
        const auto bytes = fixture.bytes();
        for (const auto budget : {65536ULL, 1048576ULL}) {
            SCOPED_TRACE(::testing::Message() << signatures << '/' << budget);
            const auto parsed = inspect_elf(bytes, budget);
            ASSERT_TRUE(parsed) << parsed.error().message;
            EXPECT_EQ(parsed->truncated, budget == 65536);
            EXPECT_GT(parsed->exported_symbols.size(), 0);
            EXPECT_EQ(parsed->exported_symbols.size() == 64, budget == 1048576);
            EXPECT_LE(parsed->symbol_bytes, budget);
            const auto& symbol = parsed->exported_symbols.front();
            const auto& metadata = signatures ? symbol.signature : symbol.version;
            ASSERT_TRUE(metadata);
            EXPECT_GE(metadata->size(), 4096);
        }
    }
}

TEST(ElfInspector, caps_generated_dwarf_signatures_before_they_can_expand) {
    auto fixture = elf_fixture{};
    fixture.symbols(1, 1);
    fixture.dwarf_signature(32, 4096);
    const auto parsed = inspect_elf(fixture.bytes());
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_TRUE(parsed->truncated);
    EXPECT_TRUE(parsed->exported_symbols.empty());
    EXPECT_LE(parsed->symbol_bytes, 16U * 1024U * 1024U);
}

TEST(ElfInspector, section_inspection_leaves_a_budget_for_registry_paths) {
    auto fixture = elf_fixture{};
    for (auto index = 0; index < 64; ++index) {
        fixture.add(std::string(128, 'a'), {});
    }
    const auto parsed = inspect_elf(fixture.bytes(), 32768);
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_FALSE(parsed->truncated);
    EXPECT_EQ(parsed->sections.size(), 66);
    EXPECT_LT(parsed->symbol_bytes, 32768);
}

TEST(ElfInspector, validates_headers_before_budget_exhaustion) {
    auto fixture = elf_fixture{};
    auto bytes = fixture.bytes();
    bytes[0] = std::byte{};
    EXPECT_FALSE(inspect_elf(bytes, 0));
}

} // namespace
