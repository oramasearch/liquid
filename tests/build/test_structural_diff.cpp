#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../temporary_directory.hpp"
#include "common/grammar_registry.hpp"
#include "liquid/frontend.hpp"
#include "liquid/hash.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "repository/structural_diff.hpp"

namespace {

struct language_case {
    std::string_view language;
    std::string_view original;
    std::string_view moved;
    std::string_view formatted;
    std::string_view modified;
};

constexpr auto cases = std::array{
    language_case{"c",
                  "int alpha(void) { return 1; }\nint beta(void) { return 2; }\n",
                  "int beta(void) { return 2; }\nint alpha(void) { return 1; }\n",
                  "int alpha(void)\n{\n    return 1;\n}\n\nint beta(void)\n{\n    return 2;\n}\n",
                  "int alpha(void) { return 3; }\nint beta(void) { return 2; }\n"},
    language_case{"c++",
                  "int alpha() { return 1; }\nint beta() { return 2; }\n",
                  "int beta() { return 2; }\nint alpha() { return 1; }\n",
                  "int alpha()\n{\n    return 1;\n}\n\nint beta()\n{\n    return 2;\n}\n",
                  "int alpha() { return 3; }\nint beta() { return 2; }\n"},
    language_case{"python",
                  "def alpha():\n    return 1\n\ndef beta():\n    return 2\n",
                  "def beta():\n    return 2\n\ndef alpha():\n    return 1\n",
                  "def alpha():\n        return 1\n\n\ndef beta():\n        return 2\n",
                  "def alpha():\n    return 3\n\ndef beta():\n    return 2\n"},
    language_case{"javascript",
                  "function alpha() { return 1; }\nfunction beta() { return 2; }\n",
                  "function beta() { return 2; }\nfunction alpha() { return 1; }\n",
                  "function alpha()\n{\n  return 1;\n}\nfunction beta()\n{\n  return 2;\n}\n",
                  "function alpha() { return 3; }\nfunction beta() { return 2; }\n"},
    language_case{
        "typescript",
        "function alpha(): number { return 1; }\nfunction beta(): number { return 2; }\n",
        "function beta(): number { return 2; }\nfunction alpha(): number { return 1; }\n",
        "function alpha(): number\n{\n  return 1;\n}\nfunction beta(): number\n{\n  return 2;\n}\n",
        "function alpha(): number { return 3; }\nfunction beta(): number { return 2; }\n"},
    language_case{
        "go",
        "package sample\nfunc alpha() int { return 1 }\nfunc beta() int { return 2 }\n",
        "package sample\nfunc beta() int { return 2 }\nfunc alpha() int { return 1 }\n",
        "package sample\n\nfunc alpha() int {\n\treturn 1\n}\n\nfunc beta() int {\n\treturn 2\n}\n",
        "package sample\nfunc alpha() int { return 3 }\nfunc beta() int { return 2 }\n"},
    language_case{"rust",
                  "fn alpha() -> i32 { 1 }\nfn beta() -> i32 { 2 }\n",
                  "fn beta() -> i32 { 2 }\nfn alpha() -> i32 { 1 }\n",
                  "fn alpha() -> i32\n{\n    1\n}\nfn beta() -> i32\n{\n    2\n}\n",
                  "fn alpha() -> i32 { 3 }\nfn beta() -> i32 { 2 }\n"},
    language_case{"java",
                  "class Sample { int alpha() { return 1; } int beta() { return 2; } }\n",
                  "class Sample { int beta() { return 2; } int alpha() { return 1; } }\n",
                  "class Sample\n{\n    int alpha()\n    {\n        return 1;\n    }\n    int "
                  "beta()\n    {\n        return 2;\n    }\n}\n",
                  "class Sample { int alpha() { return 3; } int beta() { return 2; } }\n"},
    language_case{"ruby",
                  "def alpha\n  1\nend\n\ndef beta\n  2\nend\n",
                  "def beta\n  2\nend\n\ndef alpha\n  1\nend\n",
                  "def alpha\n    1\nend\n\n\ndef beta\n    2\nend\n",
                  "def alpha\n  3\nend\n\ndef beta\n  2\nend\n"},
    language_case{"bash",
                  "alpha() { echo one; }\nbeta() { echo two; }\n",
                  "beta() { echo two; }\nalpha() { echo one; }\n",
                  "alpha()\n{\n    echo one;\n}\nbeta()\n{\n    echo two;\n}\n",
                  "alpha() { echo three; }\nbeta() { echo two; }\n"},
};

auto bytes(std::string_view value) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

class graph_fixture {
public:
    graph_fixture()
        : root_(
              liquid::test::fixture_path("liquid-structural-diff-" + std::to_string(sequence_++))) {
        auto code = std::error_code{};
        std::filesystem::remove_all(root_, code);
        code.clear();
        std::filesystem::create_directories(root_, code);
        EXPECT_FALSE(code) << code.message();
    }

    ~graph_fixture() {
        auto code = std::error_code{};
        std::filesystem::remove_all(root_, code);
    }

    auto open() -> liquid::result<liquid::frontend_graph> {
        auto loaded = liquid::frontend_module::load(LIQUID_TREE_SITTER_FRONTEND);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        auto module = std::make_shared<liquid::frontend_module>(std::move(*loaded));
        return liquid::runtime::block_on(liquid::frontend_graph::open(
            root_ / "graph",
            root_,
            std::move(module),
            [](std::vector<std::byte>) -> liquid::runtime::task<std::vector<std::byte>> {
                co_return std::unexpected(liquid::error{
                    liquid::errc::unsupported, "no action executor", "structural diff test"});
            }));
    }

    auto path() const -> const std::filesystem::path& {
        return root_;
    }

private:
    inline static std::atomic_uint64_t sequence_{};
    std::filesystem::path root_;
};

auto parse(liquid::frontend_graph& graph, std::string_view language, std::string_view source)
    -> liquid::result<liquid::parse_tree> {
    auto content = bytes(source);
    auto id = liquid::hash(liquid::algorithm::blake3, content);
    if (!id) {
        return std::unexpected(id.error());
    }
    return liquid::runtime::block_on(graph.parse({
        .language = std::string{language},
        .blob = *id,
        .source = std::move(content),
    }));
}

auto compare(liquid::frontend_graph& graph,
             std::string_view language,
             std::string_view old_source,
             std::string_view new_source) -> liquid::result<liquid::structural_diff> {
    auto old_tree = parse(graph, language, old_source);
    if (!old_tree) {
        return std::unexpected(old_tree.error());
    }
    auto new_tree = parse(graph, language, new_source);
    if (!new_tree) {
        return std::unexpected(new_tree.error());
    }
    const auto old_bytes = bytes(old_source);
    const auto new_bytes = bytes(new_source);
    return liquid::repository_detail::compare_structural(
        *old_tree, *new_tree, old_bytes, new_bytes);
}

auto reindent(std::string_view input) -> std::string {
    auto output = std::string{};
    auto start = std::size_t{};
    while (start < input.size()) {
        const auto end = input.find('\n', start);
        const auto line_end = end == std::string_view::npos ? input.size() : end;
        const auto line = input.substr(start, line_end - start);
        if (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            output += "    ";
        }
        output += line;
        if (end != std::string_view::npos) {
            output += '\n';
        }
        start = line_end + (end == std::string_view::npos ? 0 : 1);
        if (end == std::string_view::npos) {
            break;
        }
    }
    output += " \n";
    return output;
}

auto sample(std::string_view language) -> std::string {
    const auto path = std::filesystem::path{LIQUID_GRAMMAR_FIXTURES} / language / "sample";
    auto input = std::ifstream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

auto edit_summary(const liquid::structural_diff& difference) -> std::string {
    auto output = std::string{};
    for (const auto& edit : difference.edits) {
        output += std::to_string(static_cast<unsigned>(edit.kind));
        output += ':';
        output += edit.node_kind;
        output += ' ';
    }
    return output;
}

auto has_recovery_node(const liquid::parse_node& node) -> bool {
    return node.error || node.missing || std::ranges::any_of(node.children, [](const auto& child) {
               return has_recovery_node(child);
           });
}

TEST(StructuralDiff, HandWrittenLanguagePairsReportMovesFormattingAndEdits) {
    auto fixture = graph_fixture{};
    auto graph = fixture.open();
    ASSERT_TRUE(graph) << graph.error().message;
    for (const auto& test : cases) {
        SCOPED_TRACE(test.language);
        auto moved = compare(*graph, test.language, test.original, test.moved);
        ASSERT_TRUE(moved) << moved.error().message << ": " << moved.error().context;
        EXPECT_TRUE(std::ranges::any_of(moved->edits, [](const auto& edit) {
            return edit.kind == liquid::structural_edit_kind::moved;
        }));

        auto formatted = compare(*graph, test.language, test.original, test.formatted);
        ASSERT_TRUE(formatted) << formatted.error().message << ": " << formatted.error().context;
        EXPECT_TRUE(formatted->formatting_only) << edit_summary(*formatted);
        EXPECT_TRUE(formatted->edits.empty()) << edit_summary(*formatted);

        auto modified = compare(*graph, test.language, test.original, test.modified);
        ASSERT_TRUE(modified) << modified.error().message << ": " << modified.error().context;
        EXPECT_FALSE(modified->formatting_only);
        EXPECT_TRUE(std::ranges::any_of(modified->edits, [](const auto& edit) {
            return edit.kind == liquid::structural_edit_kind::modified;
        }));
    }
}

TEST(StructuralDiff, EveryOtherLockedSampleIgnoresMechanicalReindentation) {
    auto fixture = graph_fixture{};
    auto graph = fixture.open();
    ASSERT_TRUE(graph) << graph.error().message;
    auto hand_written = std::set<std::string_view>{};
    for (const auto& test : cases) {
        hand_written.insert(test.language);
    }
    for (const auto& grammar : liquid_frontend::available_grammars()) {
        if (hand_written.contains(grammar.language)) {
            continue;
        }
        SCOPED_TRACE(grammar.language);
        const auto original = sample(grammar.language);
        ASSERT_FALSE(original.empty());
        const auto formatted_source = reindent(original);
        auto difference = compare(*graph, grammar.language, original, formatted_source);
        ASSERT_TRUE(difference) << difference.error().message << ": " << difference.error().context;
        EXPECT_TRUE(difference->formatting_only) << edit_summary(*difference);
        EXPECT_TRUE(difference->edits.empty()) << edit_summary(*difference);
    }
}

TEST(StructuralDiff, CommentsRemainVisibleOutsideSemanticHashes) {
    auto fixture = graph_fixture{};
    auto graph = fixture.open();
    ASSERT_TRUE(graph) << graph.error().message;
    auto difference = compare(*graph,
                              "c",
                              "// before\nint answer(void) { return 42; }\n",
                              "// after\nint answer(void) { return 42; }\n");
    ASSERT_TRUE(difference) << difference.error().message;
    ASSERT_EQ(difference->edits.size(), 1U);
    EXPECT_EQ(difference->edits.front().kind, liquid::structural_edit_kind::comment_changed);
}

TEST(StructuralDiff, CanonicalParseRetainsErrorAndMissingFlags) {
    auto fixture = graph_fixture{};
    auto graph = fixture.open();
    ASSERT_TRUE(graph) << graph.error().message;
    auto tree = parse(*graph, "c", "int answer(");
    ASSERT_TRUE(tree) << tree.error().message;
    EXPECT_TRUE(has_recovery_node(tree->root));
}

TEST(StructuralDiff, RepositoryPublishesStructuralAndLabelledTextTiers) {
    auto fixture = graph_fixture{};
    auto graph = fixture.open();
    ASSERT_TRUE(graph) << graph.error().message;
    auto created =
        liquid::runtime::block_on(liquid::repository::create(fixture.path() / "repository"));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    ASSERT_TRUE(
        liquid::runtime::block_on(repository.write("functions.c", bytes(cases.front().original))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("notes.abc", bytes("before\n"))));
    auto before = liquid::runtime::block_on(repository.cut("before"));
    ASSERT_TRUE(before) << before.error().message;
    ASSERT_TRUE(
        liquid::runtime::block_on(repository.write("functions.c", bytes(cases.front().moved))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("notes.abc", bytes("after\n"))));
    auto after = liquid::runtime::block_on(repository.cut("after"));
    ASSERT_TRUE(after) << after.error().message;

    auto difference = liquid::runtime::block_on(
        repository.diff(*before, *after, {.structural = true, .frontend = &*graph}));
    ASSERT_TRUE(difference) << difference.error().message << ": " << difference.error().context;
    const auto source =
        std::ranges::find(*difference, "functions.c", &liquid::tree_diff_entry::path);
    ASSERT_NE(source, difference->end());
    EXPECT_EQ(source->tier, liquid::diff_tier::structural);
    ASSERT_TRUE(source->structural);
    EXPECT_TRUE(std::ranges::any_of(source->structural->edits, [](const auto& edit) {
        return edit.kind == liquid::structural_edit_kind::moved;
    }));

    const auto fallback =
        std::ranges::find(*difference, "notes.abc", &liquid::tree_diff_entry::path);
    ASSERT_NE(fallback, difference->end());
    EXPECT_EQ(fallback->tier, liquid::diff_tier::text);
    EXPECT_EQ(fallback->tier_reason, "no grammar for .abc");
    EXPECT_FALSE(fallback->structural);
    EXPECT_TRUE(fallback->line_hunks);
}

} // namespace
