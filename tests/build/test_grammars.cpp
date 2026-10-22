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

TEST(Grammars, TraversalLimitRejectsDeepSourceOnEveryEntryPoint) {
    using namespace liquid_frontend;
    for (const auto language : {syntax_language::c, syntax_language::cpp}) {
        const auto name = language == syntax_language::c ? "c" : "c++";
        const auto* grammar = find_grammar(name);
        if (!grammar) {
            continue;
        }
        SCOPED_TRACE(name);
        // The early declaration must not escape as a partial result. Function bodies and
        // declarators exercise subtrees that unit discovery would otherwise stop before.
        const auto depth = syntax_tree_max_depth * 2;
        auto sources = std::vector<std::string>{
            "int before; int f() {" + std::string(depth, '{') + "return 0;" +
                std::string(depth, '}') + "}",
            "int before; int " + std::string(depth, '(') + "value" + std::string(depth, ')') + ";",
        };
        if (language == syntax_language::cpp) {
            auto nested = std::string{"int before;"};
            for (auto index = std::size_t{}; index < depth; ++index) {
                nested += "namespace n {";
            }
            sources.push_back(nested + "int after;" + std::string(depth, '}'));
        }
        for (const auto& text : sources) {
            const auto source = std::vector<std::uint8_t>{text.begin(), text.end()};
            const auto expect_limit = [](const auto& result) {
                ASSERT_FALSE(result);
                EXPECT_EQ(result.error().code, syntax_error_code::traversal_limit);
                EXPECT_EQ(result.error().message, "syntax tree exceeds depth limit (128)");
            };
            expect_limit(parse_units(name, source));
            expect_limit(parse_symbols(name, source));
            expect_limit(encode_syntax_tree(name, source));
            expect_limit(encode_parse_tree(name, source));
            expect_limit(parse_has_error(name, source));
            expect_limit(parse_has_error(*grammar, source));
            EXPECT_TRUE(parse_units(language, source).empty());
            EXPECT_TRUE(parse_symbols(language, source).empty());
        }
    }
}

TEST(Grammars, TraversalLimitCountsAnonymousTokensAtDepthBoundary) {
    using namespace liquid_frontend;
    if (!find_grammar("json")) {
        GTEST_SKIP() << "json was excluded by LIQUID_GRAMMARS";
    }
    // The deepest named array is one level above its anonymous brackets.
    auto text =
        std::string(syntax_tree_max_depth - 1, '[') + std::string(syntax_tree_max_depth - 1, ']');
    auto source = std::vector<std::uint8_t>{text.begin(), text.end()};
    const auto accepted = parse_has_error("json", source);
    ASSERT_TRUE(accepted) << accepted.error().message;
    EXPECT_FALSE(*accepted);
    EXPECT_TRUE(encode_syntax_tree("json", source));
    EXPECT_TRUE(encode_parse_tree("json", source));

    text = "[" + text + "]";
    source.assign(text.begin(), text.end());
    const auto rejected = encode_syntax_tree("json", source);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, syntax_error_code::traversal_limit);
}

TEST(Grammars, TraversalLimitBoundsWideTreesBeforeProducingOutput) {
    using namespace liquid_frontend;
    if (!find_grammar("json")) {
        GTEST_SKIP() << "json was excluded by LIQUID_GRAMMARS";
    }
    // document + array + brackets + N numbers + (N - 1) anonymous commas = 2N + 3.
    auto text = std::string{"[0"};
    for (auto index = std::size_t{1}; index < (syntax_tree_max_nodes - 3) / 2; ++index) {
        text += ",0";
    }
    text += "]";
    auto source = std::vector<std::uint8_t>{text.begin(), text.end()};
    const auto accepted = parse_has_error("json", source);
    ASSERT_TRUE(accepted) << accepted.error().message;
    EXPECT_FALSE(*accepted);

    text.insert(text.size() - 1, ",0");
    source.assign(text.begin(), text.end());
    const auto expect_limit = [](const auto& result) {
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, syntax_error_code::traversal_limit);
        EXPECT_EQ(result.error().message, "syntax tree exceeds node limit (100000)");
    };
    expect_limit(parse_has_error("json", source));
    expect_limit(parse_units("json", source));
    expect_limit(parse_symbols("json", source));
    expect_limit(encode_syntax_tree("json", source));
    expect_limit(encode_parse_tree("json", source));
}

TEST(Grammars, TraversalLimitPropagatesThroughFrontendModule) {
    if (!liquid_frontend::find_grammar("c++")) {
        GTEST_SKIP() << "c++ was excluded by LIQUID_GRAMMARS";
    }
    auto module = liquid::frontend_module::load(LIQUID_TREE_SITTER_FRONTEND);
    ASSERT_TRUE(module) << module.error().message;
    auto text = std::string{"int before;"};
    for (auto index = std::size_t{}; index < liquid_frontend::syntax_tree_max_depth; ++index) {
        text += "namespace n {";
    }
    text += "int after;" + std::string(liquid_frontend::syntax_tree_max_depth, '}');
    const auto source = std::as_bytes(std::span{text.data(), text.size()});
    const auto expect_limit = [](const auto& result) {
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().message, "syntax tree exceeds depth limit (128)");
    };
    expect_limit(module->units("c++", source));
    expect_limit(module->symbols("c++", source));
    expect_limit(module->syntax("c++", source));
    expect_limit(module->parse("c++", source));
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
