#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "liquid/frontend.hpp"
#include "liquid/tiers.hpp"

#if defined(LIQUID_TREE_SITTER_FRONTEND)
#include "common/grammar_registry.hpp"
#endif

namespace {

#if defined(LIQUID_TREE_SITTER_FRONTEND)
auto load_frontend(std::string path) -> void {
    auto loaded = liquid::frontend_registry::global().load(std::move(path));
    if (!loaded) {
        ASSERT_EQ(loaded.error().code, liquid::errc::already_exists) << loaded.error().message;
    }
}
#endif

TEST(SupportMatrix, ContainsEveryRegisteredCodec) {
    const auto matrix = liquid::support_matrix();
    for (const auto* codec : liquid::format_registry::global().codecs()) {
        const auto found = std::ranges::find(
            matrix.formats, codec->descriptor().name, &liquid::format_support::format);
        ASSERT_NE(found, matrix.formats.end()) << codec->descriptor().name;
        EXPECT_EQ(found->tier, static_cast<std::uint8_t>(codec->descriptor().tier));
        EXPECT_EQ(found->codec, codec->descriptor().name);
        EXPECT_EQ(found->limits.maximum_depth, codec->descriptor().limits.maximum_depth);
        EXPECT_EQ(found->limits.byte_budget, codec->descriptor().limits.byte_budget);
        EXPECT_EQ(found->limits.maximum_entries, codec->descriptor().limits.maximum_entries);
    }
}

#if defined(LIQUID_TREE_SITTER_FRONTEND)
TEST(SupportMatrix, ContainsEveryLockedGrammarAndPrefersCompilerFrontends) {
    load_frontend(LIQUID_TREE_SITTER_FRONTEND);
    load_frontend(LIQUID_CLANG_FRONTEND);
    const auto matrix = liquid::support_matrix();
    for (const auto& grammar : liquid_frontend::available_grammars()) {
        const auto found = std::ranges::find(
            matrix.languages, grammar.language, &liquid::language_support::language);
        ASSERT_NE(found, matrix.languages.end()) << grammar.language;
        if (grammar.language != "c++") {
            EXPECT_EQ(found->tier, 2);
            EXPECT_EQ(found->mechanism,
                      "grammar(" + std::string{liquid_frontend::grammar_lock_digest()} + ")");
        }
    }
    const auto cpp =
        std::ranges::find(matrix.languages, "c++", &liquid::language_support::language);
    ASSERT_NE(cpp, matrix.languages.end());
    EXPECT_EQ(cpp->tier, 3);
    EXPECT_EQ(cpp->mechanism, "frontend(clang-cpp)");
}
#endif

} // namespace
