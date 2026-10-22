#include <gtest/gtest.h>

#include <algorithm>

#include "liquid/format.hpp"
#include "pe_fixture.hpp"

namespace {

TEST(PeRegistry, propagates_capped_name_truncation) {
    const auto codecs = liquid::format_registry::global().find_by_extension("fixture.dll");
    ASSERT_EQ(codecs.size(), 1);
    auto fixture = pe_test::pe_names_fixture{1, 1, 65538};
    fixture.fill_name(65537);
    const auto tree = codecs.front()->unpack(fixture.bytes, {.byte_budget = 1048576});
    ASSERT_TRUE(tree) << tree.error().message;
    EXPECT_TRUE(tree->truncated);
    EXPECT_FALSE(tree->limits.empty());
    EXPECT_EQ(std::ranges::count_if(tree->entries,
                                    [](const auto& entry) {
                                        return entry.path.starts_with("exports/") ||
                                               entry.path.starts_with("imports/");
                                    }),
              2);
}

TEST(PeRegistry, supplies_the_callers_budget_to_pe) {
    const auto codecs = liquid::format_registry::global().find_by_extension("fixture.dll");
    ASSERT_EQ(codecs.size(), 1);
    auto fixture = pe_test::pe_names_fixture{32, 32, 4097};
    fixture.fill_name(4096);
    for (auto index = 0U; index < 32; ++index) {
        const auto offset = pe_test::pe_names_fixture::rva(fixture.strings + index);
        fixture.put32(fixture.names + index * 4, offset);
        fixture.put32(fixture.imports + index * 20 + 12, offset);
    }
    for (const auto budget : {0ULL, 65536ULL, 1048576ULL}) {
        SCOPED_TRACE(budget);
        const auto tree = codecs.front()->unpack(fixture.bytes, {.byte_budget = budget});
        ASSERT_TRUE(tree) << tree.error().message;
        const auto names = std::ranges::count_if(tree->entries, [](const auto& entry) {
            return entry.path.starts_with("exports/") || entry.path.starts_with("imports/");
        });
        EXPECT_EQ(tree->truncated, budget != 1048576);
        if (budget == 1048576) {
            EXPECT_EQ(names, 64);
        } else {
            EXPECT_LT(names, 64);
        }
    }
}

} // namespace
