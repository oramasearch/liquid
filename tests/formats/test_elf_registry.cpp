// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "elf_fixture.hpp"
#include "liquid/abi.hpp"
#include "liquid/format.hpp"

namespace {

TEST(ElfRegistry, supplies_the_callers_budget_to_inspection_and_entry_paths) {
    const auto codecs = liquid::format_registry::global().find_by_extension("fixture.elf");
    ASSERT_EQ(codecs.size(), 1);
    auto fixture = elf_test::elf_fixture{};
    fixture.symbols(64, 4096);
    for (const auto budget : {0ULL, 32768ULL, 1048576ULL}) {
        SCOPED_TRACE(budget);
        const auto tree = codecs.front()->unpack(fixture.bytes(), {.byte_budget = budget});
        ASSERT_TRUE(tree) << tree.error().message;
        EXPECT_EQ(tree->truncated, budget != 1048576);
        const auto exports = std::ranges::count_if(
            tree->entries, [](const auto& entry) { return entry.path.starts_with("exports/"); });
        EXPECT_EQ(exports == 64, budget == 1048576);
        if (budget == 0) {
            EXPECT_TRUE(tree->entries.empty());
        }
    }
}

TEST(ElfRegistry, charges_section_entry_name_copies_after_inspection) {
    auto fixture = elf_test::elf_fixture{};
    for (auto index = 0; index < 64; ++index) {
        fixture.add(std::string(128, 'a'), {});
    }
    const auto codecs = liquid::format_registry::global().find_by_extension("fixture.elf");
    ASSERT_EQ(codecs.size(), 1);
    const auto tree = codecs.front()->unpack(fixture.bytes(), {.byte_budget = 32768});
    ASSERT_TRUE(tree) << tree.error().message;
    EXPECT_TRUE(tree->truncated);
    EXPECT_GT(tree->entries.size(), 0);
    EXPECT_LT(tree->entries.size(), fixture.sections.size());
}

TEST(ElfRegistry, name_truncation_reaches_the_tree_and_blocks_abi_compatibility) {
    auto fixture = elf_test::elf_fixture{};
    fixture.symbols(1, 65537);
    const auto bytes = fixture.bytes();
    const auto codecs = liquid::format_registry::global().find_by_extension("fixture.elf");
    ASSERT_EQ(codecs.size(), 1);
    const auto tree = codecs.front()->unpack(bytes, {});
    ASSERT_TRUE(tree) << tree.error().message;
    EXPECT_TRUE(tree->truncated);
    EXPECT_FALSE(tree->limits.empty());
    const auto descriptor = liquid::extract_abi(bytes);
    ASSERT_TRUE(descriptor) << descriptor.error().message;
    EXPECT_TRUE(descriptor->truncated);
    const auto report = liquid::diff_abi(*descriptor, *descriptor);
    EXPECT_EQ(report.verdict, liquid::abi_verdict::incompatible);
}

} // namespace
