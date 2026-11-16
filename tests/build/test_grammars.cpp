// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tree_sitter/api.h>

#include "common/grammar_registry.hpp"
#include "common/syntax.hpp"
#include "liquid/error.hpp"
#include "liquid/frontend.hpp"

namespace {

auto sample(std::string_view language) -> std::vector<std::uint8_t> {
    const auto path = std::filesystem::path{LIQUID_GRAMMAR_FIXTURES} / language / "sample";
    auto stream = std::ifstream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

TEST(Grammars, EveryLockedSampleParsesWithoutErrorNodes) {
    const auto grammars = liquid_frontend::available_grammars();
    ASSERT_FALSE(grammars.empty());
    for (const auto& grammar : grammars) {
        SCOPED_TRACE(grammar.language);
        const auto source = sample(grammar.language);
        ASSERT_FALSE(source.empty());
        auto has_error = liquid_frontend::parse_has_error(grammar.language, source);
        ASSERT_TRUE(has_error) << has_error.error().message;
        EXPECT_FALSE(*has_error);
    }
}

TEST(Grammars, RequiredTagsQueriesDiscoverUnits) {
    constexpr auto required = std::array{
        std::string_view{"python"},
        std::string_view{"go"},
        std::string_view{"rust"},
        std::string_view{"javascript"},
        std::string_view{"typescript"},
        std::string_view{"java"},
        std::string_view{"ruby"},
    };
    for (const auto language : required) {
        if (!liquid_frontend::find_grammar(language)) {
            continue;
        }
        SCOPED_TRACE(language);
        auto units = liquid_frontend::parse_units(language, sample(language));
        ASSERT_TRUE(units) << units.error().message;
        EXPECT_FALSE(units->empty());
    }
}

TEST(Grammars, MissingTagsQueryReportsUnitsUnsupported) {
    if (!liquid_frontend::find_grammar("json")) {
        GTEST_SKIP() << "json was excluded by LIQUID_GRAMMARS";
    }
    auto units = liquid_frontend::parse_units("json", sample("json"));
    ASSERT_FALSE(units);
    EXPECT_EQ(units.error().code, liquid_frontend::syntax_error_code::unsupported);
    EXPECT_NE(units.error().message.find("no pinned tags.scm"), std::string::npos);
}

TEST(Grammars, RuntimeRejectsUnsupportedLanguageAbi) {
    EXPECT_FALSE(
        liquid_frontend::grammar_abi_supported(TREE_SITTER_MIN_COMPATIBLE_LANGUAGE_VERSION - 1));
    EXPECT_FALSE(liquid_frontend::grammar_abi_supported(TREE_SITTER_LANGUAGE_VERSION + 1));

    const auto grammars = liquid_frontend::available_grammars();
    ASSERT_FALSE(grammars.empty());
    auto incompatible = grammars.front();
    incompatible.abi_version = TREE_SITTER_LANGUAGE_VERSION + 1;
    auto parsed = liquid_frontend::parse_has_error(incompatible, sample(incompatible.language));
    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error().code, liquid_frontend::syntax_error_code::incompatible_abi);
}

TEST(Grammars, PublicRegistryReportsGrammarMetadata) {
    auto module = liquid::frontend_module::load(LIQUID_TREE_SITTER_FRONTEND);
    ASSERT_TRUE(module) << module.error().message;
    auto registry = liquid::frontend_registry{};
    ASSERT_TRUE(registry.add(std::move(*module)));
    const auto languages = registry.languages();
    const auto python =
        std::ranges::find(languages, "python", &liquid::frontend_language::language);
    if (liquid_frontend::find_grammar("python")) {
        ASSERT_NE(python, languages.end());
        EXPECT_EQ(python->tier_mechanism, liquid::frontend_tier_mechanism::grammar);
        EXPECT_EQ(python->extensions, std::vector<std::string>{".py"});
        EXPECT_EQ(python->grammar_digest, liquid_frontend::grammar_lock_digest());
    }
}

} // namespace
