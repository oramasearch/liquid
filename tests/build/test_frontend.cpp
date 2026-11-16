// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "../temporary_directory.hpp"
#include "encoding/cbor.hpp"
#include "liquid/frontend.hpp"
#include "liquid/repository.hpp"

extern "C" const liquid_frontend_module_v1* liquid_frontend_get_module_v1(void);

namespace {

namespace cbor = liquid::encoding::cbor;

auto process_id() -> std::uint64_t {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::_getpid());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

auto bytes(std::string_view value) -> std::vector<std::byte> {
    auto output = std::vector<std::byte>(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        output[index] = static_cast<std::byte>(value[index]);
    }
    return output;
}

auto read_text(const std::filesystem::path& path) -> std::string {
    auto input = std::ifstream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void write_text(const std::filesystem::path& path, std::string_view value) {
    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

auto map(std::initializer_list<std::pair<std::string, cbor::value>> entries) -> cbor::value {
    auto output = cbor::value::map{};
    for (auto& entry : entries) {
        output.emplace_back(entry.first, entry.second);
    }
    return output;
}

auto find(const cbor::value& value, std::string_view key) -> const cbor::value* {
    const auto* object = std::get_if<cbor::value::map>(&value.data);
    if (!object) {
        return nullptr;
    }
    for (const auto& [candidate, child] : *object) {
        const auto* text = std::get_if<std::string>(&candidate.data);
        if (text && *text == key) {
            return &child;
        }
    }
    return nullptr;
}

auto shell_quote(std::string_view value) -> std::string {
#if defined(_WIN32)
    auto output = std::string{"\""};
    for (const auto character : value) {
        if (character == '"') {
            output += "\\\"";
        } else {
            output += character;
        }
    }
    output += '"';
#else
    auto output = std::string{"'"};
    for (const auto character : value) {
        if (character == '\'') {
            output += "'\\''";
        } else {
            output += character;
        }
    }
    output += '\'';
#endif
    return output;
}

class frontend_directory {
public:
    frontend_directory()
        : root_(liquid::test::fixture_path("liquid-frontend-test-" + std::to_string(process_id()) +
                                           "-" + std::to_string(next_.fetch_add(1)))),
          project_(root_ / "project") {
        auto code = std::error_code{};
        std::filesystem::remove_all(root_, code);
        code.clear();
        std::filesystem::create_directories(root_, code);
        EXPECT_FALSE(code) << code.message();
        std::filesystem::copy(
            LIQUID_FRONTEND_FIXTURES, project_, std::filesystem::copy_options::recursive, code);
        EXPECT_FALSE(code) << code.message();
    }

    ~frontend_directory() {
        auto code = std::error_code{};
        std::filesystem::remove_all(root_, code);
    }

    auto project() const -> const std::filesystem::path& {
        return project_;
    }

    auto graph_state() const -> std::filesystem::path {
        return root_ / "graph";
    }

    auto action_state() const -> std::filesystem::path {
        return root_ / "actions";
    }

    auto repository_state() const -> std::filesystem::path {
        return root_ / "repository";
    }

private:
    inline static std::atomic_uint64_t next_{};
    std::filesystem::path root_;
    std::filesystem::path project_;
};

struct executor_state {
    std::filesystem::path directory;
    std::uint64_t sequence{};
};

auto execute_action(std::shared_ptr<executor_state> state, std::vector<std::byte> action)
    -> liquid::runtime::task<std::vector<std::byte>> {
    auto decoded = cbor::decode(action);
    if (!decoded) {
        co_return std::unexpected(decoded.error());
    }
    const auto* compiler_value = find(*decoded, "compiler");
    const auto* arguments_value = find(*decoded, "arguments");
    const auto* source_value = find(*decoded, "source");
    const auto* compiler =
        compiler_value ? std::get_if<std::string>(&compiler_value->data) : nullptr;
    const auto* arguments =
        arguments_value ? std::get_if<cbor::value::array>(&arguments_value->data) : nullptr;
    const auto* source =
        source_value ? std::get_if<cbor::value::bytes>(&source_value->data) : nullptr;
    if (!compiler || !arguments || !source) {
        co_return std::unexpected(
            liquid::error{liquid::errc::corrupt, "invalid action", "frontend test"});
    }
    auto code = std::error_code{};
    std::filesystem::create_directories(state->directory, code);
    if (code) {
        co_return std::unexpected(
            liquid::error{liquid::errc::io, code.message(), state->directory.string()});
    }
    const auto number = state->sequence++;
    const auto input = state->directory / (std::to_string(number) + ".cpp");
#if defined(_WIN32)
    const auto output = state->directory / (std::to_string(number) + ".obj");
#else
    const auto output = state->directory / (std::to_string(number) + ".o");
#endif
    auto source_file = std::ofstream{input, std::ios::binary | std::ios::trunc};
    source_file.write(reinterpret_cast<const char*>(source->data()),
                      static_cast<std::streamsize>(source->size()));
    source_file.close();
    if (!source_file) {
        co_return std::unexpected(
            liquid::error{liquid::errc::io, "cannot write compiler input", input.string()});
    }
    auto command = shell_quote(*compiler);
    for (const auto& item : *arguments) {
        const auto* argument = std::get_if<std::string>(&item.data);
        if (!argument) {
            co_return std::unexpected(
                liquid::error{liquid::errc::corrupt, "invalid argument", "frontend test"});
        }
        command += " " + shell_quote(*argument);
    }
#if defined(_WIN32)
    command += " /TP /c " + shell_quote(input.string());
    command += " /Fo" + shell_quote(output.string());
#else
    command += " -x c++ -c " + shell_quote(input.string());
    command += " -o " + shell_quote(output.string());
#endif
    if (std::system(command.c_str()) != 0) {
        co_return std::unexpected(
            liquid::error{liquid::errc::internal, "compiler action failed", command});
    }
    auto object = std::ifstream{output, std::ios::binary};
    auto characters =
        std::vector<char>{std::istreambuf_iterator<char>{object}, std::istreambuf_iterator<char>{}};
    if (characters.empty()) {
        co_return std::unexpected(
            liquid::error{liquid::errc::io, "compiler output is empty", output.string()});
    }
    auto result = std::vector<std::byte>(characters.size());
    std::ranges::copy(characters, reinterpret_cast<char*>(result.data()));
    co_return result;
}

auto compiler_context(const frontend_directory& fixture) -> liquid::result<std::vector<std::byte>> {
#if defined(_WIN32)
    auto arguments = cbor::value::array{
        cbor::value{"/std:c++20"},
        cbor::value{"/I" + fixture.project().string()},
    };
#else
    auto arguments = cbor::value::array{
        cbor::value{"-std=c++20"},
        cbor::value{"-I" + fixture.project().string()},
    };
#endif
    return cbor::encode(map({
        {"compiler", std::string{LIQUID_CXX_COMPILER}},
        {"arguments", std::move(arguments)},
    }));
}

auto open_graph(frontend_directory& fixture) -> liquid::result<liquid::frontend_graph> {
    auto module_value = liquid::frontend_module::load(LIQUID_CLANG_FRONTEND);
    if (!module_value) {
        return std::unexpected(module_value.error());
    }
    auto module = std::make_shared<liquid::frontend_module>(std::move(*module_value));
    auto executor = std::make_shared<executor_state>();
    executor->directory = fixture.action_state();
    return liquid::runtime::block_on(
        liquid::frontend_graph::open(fixture.graph_state(),
                                     fixture.project(),
                                     std::move(module),
                                     [executor](std::vector<std::byte> action) {
                                         return execute_action(executor, std::move(action));
                                     }));
}

auto build(liquid::frontend_graph& graph, std::string source, const std::vector<std::byte>& context)
    -> liquid::result<liquid::frontend_build_report> {
    return liquid::runtime::block_on(graph.build({
        .language = "c++",
        .source_path = "functions.cpp",
        .source = bytes(source),
        .context_cbor = context,
    }));
}

TEST(FrontendModule, clang_loads_dynamically_and_registers_statically) {
    ASSERT_TRUE(std::filesystem::is_regular_file(LIQUID_CLANG_FRONTEND));
    auto dynamic = liquid::frontend_module::load(LIQUID_CLANG_FRONTEND);
    ASSERT_TRUE(dynamic) << dynamic.error().message;
    EXPECT_EQ(dynamic->name(), "clang-cpp");
    EXPECT_TRUE(dynamic->supports("c++"));

    auto linked = liquid::frontend_module::linked(liquid_frontend_get_module_v1());
    ASSERT_TRUE(linked) << linked.error().message;
    EXPECT_EQ(linked->name(), dynamic->name());
    EXPECT_EQ(linked->languages(), dynamic->languages());
}

TEST(FrontendModule, older_v1_descriptor_without_syntax_remains_loadable) {
    auto descriptor = *liquid_frontend_get_module_v1();
    descriptor.struct_size = offsetof(liquid_frontend_module_v1, syntax);
    auto linked = liquid::frontend_module::linked(&descriptor);
    ASSERT_TRUE(linked) << linked.error().message;
    auto syntax = linked->syntax("c++", bytes("int value = 1;\n"));
    ASSERT_FALSE(syntax);
    EXPECT_EQ(syntax.error().code, liquid::errc::unsupported);
}

TEST(FrontendModule, older_v1_descriptor_without_parse_remains_loadable) {
    auto descriptor = *liquid_frontend_get_module_v1();
    descriptor.struct_size = offsetof(liquid_frontend_module_v1, parse);
    auto linked = liquid::frontend_module::linked(&descriptor);
    ASSERT_TRUE(linked) << linked.error().message;
    auto parsed = linked->parse("c++", bytes("int value = 1;\n"));
    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error().code, liquid::errc::unsupported);
}

#if defined(__APPLE__) || defined(__linux__)
TEST(FrontendModule, clang_shared_object_exports_only_the_v1_entrypoint) {
#if defined(__APPLE__)
    auto command = shell_quote(LIQUID_NM) + " -g -U -P " + shell_quote(LIQUID_CLANG_FRONTEND);
#else
    auto command =
        shell_quote(LIQUID_NM) + " -D --defined-only -P " + shell_quote(LIQUID_CLANG_FRONTEND);
#endif
    command += " 2>&1";
    auto* pipe = ::popen(command.c_str(), "r");
    ASSERT_NE(pipe, nullptr);
    auto output = std::string{};
    auto buffer = std::array<char, 512>{};
    while (::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        output += buffer.data();
    }
    const auto status = ::pclose(pipe);
    ASSERT_EQ(status, 0) << output;

    auto symbols = std::vector<std::string>{};
    auto lines = std::istringstream{output};
    auto line = std::string{};
    while (std::getline(lines, line)) {
        auto fields = std::istringstream{line};
        auto symbol = std::string{};
        fields >> symbol;
        if (!symbol.empty()) {
            symbols.push_back(std::move(symbol));
        }
    }
    ASSERT_EQ(symbols.size(), 1U) << output;
    if (symbols.front().starts_with('_')) {
        symbols.front().erase(0, 1);
    }
    EXPECT_EQ(symbols.front(), LIQUID_FRONTEND_ENTRYPOINT_V1);
}
#endif

TEST(FrontendModule, tree_sitter_fallback_discovers_a_c_definition) {
    auto module = liquid::frontend_module::load(LIQUID_TREE_SITTER_FRONTEND);
    ASSERT_TRUE(module) << module.error().message;
    ASSERT_TRUE(module->supports("c"));
    const auto source = std::string{"int answer(void) {\n    return 42;\n}\n"};
    auto units = module->units("c", bytes(source));
    ASSERT_TRUE(units) << units.error().message;
    const auto found = std::ranges::find_if(*units, [](const auto& unit) {
        return unit.kind == liquid::frontend_unit_kind::definition && unit.name == "answer";
    });
    ASSERT_NE(found, units->end());
    EXPECT_EQ(found->range.start.byte, 0U);
    EXPECT_EQ(found->range.start.line, 1U);
    EXPECT_EQ(found->range.start.column, 1U);
    EXPECT_EQ(found->range.end.byte, source.size() - 1);
    EXPECT_EQ(found->range.end.line, 3U);
}

TEST(FrontendModule, tree_sitter_exposes_canonical_concrete_syntax) {
    auto module = liquid::frontend_module::load(LIQUID_TREE_SITTER_FRONTEND);
    ASSERT_TRUE(module) << module.error().message;
    const auto source = std::string{"int answer(void) {\n    return 42;\n}\n"};
    auto syntax = module->syntax("c", bytes(source));
    ASSERT_TRUE(syntax) << syntax.error().message;
    EXPECT_EQ(syntax->language, "c");
    EXPECT_EQ(syntax->root.kind, "translation_unit");
    const auto* literal = syntax->root.node_at(source.find("42"));
    ASSERT_NE(literal, nullptr);
    EXPECT_EQ(literal->kind, "number_literal");
    const auto* function = literal->enclosing("function_definition");
    ASSERT_NE(function, nullptr);
    EXPECT_EQ(function->range.start.byte, 0U);
    auto unsupported = liquid::frontend_module::load(LIQUID_CLANG_FRONTEND);
    ASSERT_TRUE(unsupported);
    auto unavailable = unsupported->syntax("c++", bytes(source));
    ASSERT_FALSE(unavailable);
    EXPECT_EQ(unavailable.error().code, liquid::errc::unsupported);
}

TEST(FrontendGraph, body_edit_runs_one_action_and_reassembles) {
    auto fixture = frontend_directory{};
    auto graph = open_graph(fixture);
    ASSERT_TRUE(graph) << graph.error().message << ": " << graph.error().context;
    auto context = compiler_context(fixture);
    ASSERT_TRUE(context) << context.error().message;
    const auto original = read_text(fixture.project() / "functions.cpp");
    auto first = build(*graph, original, *context);
    ASSERT_TRUE(first) << first.error().message << ": " << first.error().context;
    ASSERT_EQ(first->units.size(), 2U);
    EXPECT_EQ(first->actions_run, 2U);
    EXPECT_EQ(first->assemblies_run, 1U);

    auto edited = original;
    const auto expression = std::string{"return VALUE + 1;"};
    const auto position = edited.find(expression);
    ASSERT_NE(position, std::string::npos);
    edited.replace(position, expression.size(), "return VALUE + 3;");
    auto rebuilt = build(*graph, edited, *context);
    ASSERT_TRUE(rebuilt) << rebuilt.error().message << ": " << rebuilt.error().context;
    ASSERT_EQ(rebuilt->units.size(), 2U);
    EXPECT_EQ(rebuilt->actions_run, 1U);
    EXPECT_EQ(rebuilt->assemblies_run, 1U);
    EXPECT_EQ(std::ranges::count(rebuilt->units, true, &liquid::frontend_unit_output::action_ran),
              1);
    const auto left =
        std::ranges::find(rebuilt->units, "left", [](const auto& unit) { return unit.unit.name; });
    ASSERT_NE(left, rebuilt->units.end());
    EXPECT_TRUE(left->action_ran);
    const auto right =
        std::ranges::find(rebuilt->units, "right", [](const auto& unit) { return unit.unit.name; });
    ASSERT_NE(right, rebuilt->units.end());
    EXPECT_FALSE(right->action_ran);
}

TEST(FrontendGraph, structural_diff_after_build_parses_each_blob_once) {
    auto fixture = frontend_directory{};
    auto graph = open_graph(fixture);
    ASSERT_TRUE(graph) << graph.error().message << ": " << graph.error().context;
    auto context = compiler_context(fixture);
    ASSERT_TRUE(context) << context.error().message;
    const auto original = read_text(fixture.project() / "functions.cpp");
    auto built = build(*graph, original, *context);
    ASSERT_TRUE(built) << built.error().message << ": " << built.error().context;
    ASSERT_EQ(graph->parse_count(), 1U);

    auto created =
        liquid::runtime::block_on(liquid::repository::create(fixture.repository_state()));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("functions.cpp", bytes(original))));
    auto before = liquid::runtime::block_on(repository.cut("before"));
    ASSERT_TRUE(before) << before.error().message;
    auto edited = original;
    const auto position = edited.find("return VALUE + 1;");
    ASSERT_NE(position, std::string::npos);
    edited.replace(position, std::string_view{"return VALUE + 1;"}.size(), "return VALUE + 3;");
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("functions.cpp", bytes(edited))));
    auto after = liquid::runtime::block_on(repository.cut("after"));
    ASSERT_TRUE(after) << after.error().message;
    auto difference = liquid::runtime::block_on(
        repository.diff(*before, *after, {.structural = true, .frontend = &*graph}));
    ASSERT_TRUE(difference) << difference.error().message << ": " << difference.error().context;
    ASSERT_EQ(difference->size(), 1U);
    EXPECT_EQ(difference->front().tier, liquid::diff_tier::structural);
    EXPECT_EQ(graph->parse_count(), 2U);
}

TEST(FrontendGraph, header_edit_reruns_every_reader_and_reassembles) {
    auto fixture = frontend_directory{};
    auto graph = open_graph(fixture);
    ASSERT_TRUE(graph) << graph.error().message << ": " << graph.error().context;
    auto context = compiler_context(fixture);
    ASSERT_TRUE(context) << context.error().message;
    const auto source = read_text(fixture.project() / "functions.cpp");
    auto first = build(*graph, source, *context);
    ASSERT_TRUE(first) << first.error().message << ": " << first.error().context;
    ASSERT_EQ(first->units.size(), 2U);

    write_text(fixture.project() / "value.hpp", "#pragma once\n\n#define VALUE 8\n");
    auto marked = graph->mark_changed("value.hpp");
    ASSERT_TRUE(marked) << marked.error().message;
    EXPECT_GT(*marked, 0U);
    auto rebuilt = build(*graph, source, *context);
    ASSERT_TRUE(rebuilt) << rebuilt.error().message << ": " << rebuilt.error().context;
    ASSERT_EQ(rebuilt->units.size(), 2U);
    EXPECT_EQ(rebuilt->actions_run, 2U);
    EXPECT_EQ(rebuilt->assemblies_run, 1U);
    EXPECT_TRUE(std::ranges::all_of(rebuilt->units, &liquid::frontend_unit_output::action_ran));
    EXPECT_NE(rebuilt->translation_unit_output, first->translation_unit_output);
}

} // namespace
