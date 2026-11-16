// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/daemon.hpp"
#include "liquid/edit.hpp"
#include "liquid/frontend.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "temporary_directory.hpp"

namespace {

class edit_repository {
public:
    edit_repository()
        : path_(liquid::test::fixture_path("liquid-edit-test-" + std::to_string(next_++))) {
        std::filesystem::remove_all(path_);
        auto created = liquid::runtime::block_on(liquid::repository::create(path_));
        if (!created) {
            throw std::runtime_error(created.error().message);
        }
        repository_ = std::move(*created);
    }

    ~edit_repository() {
        std::filesystem::remove_all(path_);
    }

    auto repository() -> liquid::repository& {
        return repository_;
    }

    auto path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    inline static std::atomic_uint64_t next_{};
    std::filesystem::path path_;
    liquid::repository repository_;
};

auto bytes(std::string_view text) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(text.data()),
            reinterpret_cast<const std::byte*>(text.data() + text.size())};
}

auto text(std::span<const std::byte> value) -> std::string {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

auto position(std::string_view source, std::size_t byte) -> liquid::frontend_source_position {
    auto line = std::uint64_t{1};
    auto column = std::uint64_t{1};
    for (auto index = std::size_t{}; index < byte; ++index) {
        if (source[index] == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    return {byte, line, column};
}

auto range(std::string_view source, std::size_t start, std::size_t end)
    -> liquid::frontend_source_range {
    return {position(source, start), position(source, end)};
}

auto fingerprint(std::string_view source, liquid::frontend_source_range selected)
    -> liquid::digest {
    auto value = liquid::hash(
        liquid::algorithm::blake3,
        std::span{reinterpret_cast<const std::byte*>(source.data() + selected.start.byte),
                  static_cast<std::size_t>(selected.end.byte - selected.start.byte)});
    EXPECT_TRUE(value) << value.error().message;
    return value.value_or(liquid::digest{});
}

auto occurrence(std::string_view source,
                std::string name,
                liquid::symbol_role role,
                std::size_t offset,
                std::string symbol = "c++:shared") -> liquid::symbol_occurrence {
    return {.symbol = std::move(symbol),
            .name = name,
            .kind = liquid::symbol_kind::function,
            .role = role,
            .range = range(source, offset, offset + name.size())};
}

void write(liquid::repository& repository, std::string path, std::string_view source) {
    auto written = liquid::runtime::block_on(repository.write(std::move(path), bytes(source)));
    ASSERT_TRUE(written) << written.error().message;
}

void store_symbols(liquid::repository& repository,
                   std::string_view path,
                   std::string_view source,
                   std::vector<liquid::symbol_occurrence> occurrences,
                   liquid::symbol_origin origin = liquid::symbol_origin::compiler) {
    auto subject = liquid::hash(liquid::algorithm::blake3, bytes(source));
    ASSERT_TRUE(subject);
    auto stored = liquid::runtime::block_on(repository.store_symbols(
        {*subject, path.ends_with(".c") ? "c" : "c++", origin, {}, std::move(occurrences)}));
    ASSERT_TRUE(stored) << stored.error().message;
}

auto compile(const std::filesystem::path& directory, std::string name, std::string_view source)
    -> bool {
    const auto input = directory / (name + ".cpp");
    const auto output = directory / (name + ".o");
    auto file = std::ofstream{input, std::ios::binary | std::ios::trunc};
    file.write(source.data(), static_cast<std::streamsize>(source.size()));
    file.close();
    const auto command = std::string{"\"" LIQUID_CXX_COMPILER "\" -std=c++20 -I\""} +
                         directory.string() + "\" -c \"" + input.string() + "\" -o \"" +
                         output.string() + "\"";
    return std::system(command.c_str()) == 0;
}

auto read_source(const std::filesystem::path& path) -> std::string {
    auto input = std::ifstream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

TEST(StructuralEdit, rename_across_compiler_records_is_exact_and_undoes_as_one_operation) {
    edit_repository fixture;
    const auto left = std::string{"int shared() { return 7; }\n"};
    const auto right = std::string{
        "int shared();\nint use() { return shared(); }\nconst char* label = \"shared\";\n"};
    write(fixture.repository(), "left.cpp", left);
    write(fixture.repository(), "right.cpp", right);
    store_symbols(
        fixture.repository(),
        "left.cpp",
        left,
        {occurrence(left, "shared", liquid::symbol_role::definition, left.find("shared"))});
    store_symbols(
        fixture.repository(),
        "right.cpp",
        right,
        {occurrence(right, "shared", liquid::symbol_role::reference, right.find("shared")),
         occurrence(right,
                    "shared",
                    liquid::symbol_role::reference,
                    right.find("shared", right.find("shared") + 1))});

    auto preview = liquid::runtime::block_on(fixture.repository().preview_edit(
        liquid::rename_request{liquid::edit_source_location{"right.cpp", 2, 20}, "updated"},
        {"test", "rename", "fixture"}));
    ASSERT_TRUE(preview) << preview.error().message;
    ASSERT_EQ(preview->changes.size(), 2U);
    ASSERT_EQ(preview->refused.size(), 1U);
    EXPECT_NE(preview->refused.front().reason.find("string"), std::string::npos);
    auto encoded = liquid::encode_edit_preview(*preview);
    ASSERT_TRUE(encoded);
    auto decoded = liquid::decode_edit_preview(*encoded);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(liquid::to_string(decoded->id), liquid::to_string(preview->id));

    auto applied = liquid::runtime::block_on(fixture.repository().apply_edit(*preview));
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(applied->drips.size(), 2U);
    auto left_after = liquid::runtime::block_on(fixture.repository().read("left.cpp"));
    auto right_after = liquid::runtime::block_on(fixture.repository().read("right.cpp"));
    ASSERT_TRUE(left_after);
    ASSERT_TRUE(right_after);
    EXPECT_EQ(text(*left_after), "int updated() { return 7; }\n");
    EXPECT_NE(text(*right_after).find("return updated()"), std::string::npos);
    EXPECT_NE(text(*right_after).find("\"shared\""), std::string::npos);
    EXPECT_TRUE(compile(fixture.path(), "left-after", text(*left_after)));
    EXPECT_TRUE(compile(fixture.path(), "right-after", text(*right_after)));
    for (const auto& file : preview->changes) {
        auto contents = liquid::runtime::block_on(fixture.repository().read(file.path));
        ASSERT_TRUE(contents);
        auto digest = liquid::hash(liquid::algorithm::blake3, *contents);
        ASSERT_TRUE(digest);
        EXPECT_EQ(liquid::to_string(*digest), liquid::to_string(file.after));
    }
    auto operations = liquid::runtime::block_on(fixture.repository().operations());
    ASSERT_TRUE(operations);
    ASSERT_FALSE(operations->empty());
    EXPECT_EQ(operations->front().kind, "structural_edit");
    auto undone = liquid::runtime::block_on(fixture.repository().undo(applied->operation));
    ASSERT_TRUE(undone);
    EXPECT_EQ(undone->applied.size(), 1U);
    left_after = liquid::runtime::block_on(fixture.repository().read("left.cpp"));
    right_after = liquid::runtime::block_on(fixture.repository().read("right.cpp"));
    ASSERT_TRUE(left_after);
    ASSERT_TRUE(right_after);
    EXPECT_EQ(text(*left_after), left);
    EXPECT_EQ(text(*right_after), right);
}

TEST(StructuralEdit, rename_collision_names_the_conflicting_symbol_and_range) {
    edit_repository fixture;
    const auto source = std::string{"int shared() { return 1; }\nint taken() { return 2; }\n"};
    write(fixture.repository(), "collision.cpp", source);
    store_symbols(
        fixture.repository(),
        "collision.cpp",
        source,
        {occurrence(source, "shared", liquid::symbol_role::definition, source.find("shared")),
         occurrence(
             source, "taken", liquid::symbol_role::definition, source.find("taken"), "c++:taken")});
    auto preview = liquid::runtime::block_on(fixture.repository().preview_edit(
        liquid::rename_request{liquid::edit_source_location{"collision.cpp", 1, 5}, "taken"},
        {"test", "rename", std::nullopt}));
    ASSERT_TRUE(preview);
    EXPECT_TRUE(preview->changes.empty());
    ASSERT_EQ(preview->refused.size(), 1U);
    EXPECT_NE(preview->refused.front().reason.find("c++:taken"), std::string::npos);
    EXPECT_EQ(preview->refused.front().target.node.range.start.line, 2U);
}

TEST(StructuralEdit, free_symbols_in_different_files_do_not_collide) {
    edit_repository fixture;
    const auto left = std::string{"int shared() { return 1; }\n"};
    const auto right = std::string{"int taken() { return 2; }\n"};
    write(fixture.repository(), "left.cpp", left);
    write(fixture.repository(), "right.cpp", right);
    store_symbols(
        fixture.repository(),
        "left.cpp",
        left,
        {occurrence(left, "shared", liquid::symbol_role::definition, left.find("shared"))});
    store_symbols(
        fixture.repository(),
        "right.cpp",
        right,
        {occurrence(
            right, "taken", liquid::symbol_role::definition, right.find("taken"), "c++:taken")});
    auto preview = liquid::runtime::block_on(fixture.repository().preview_edit(
        liquid::rename_request{liquid::edit_source_location{"left.cpp", 1, 5}, "taken"},
        {"test", "rename", std::nullopt}));
    ASSERT_TRUE(preview) << preview.error().message;
    EXPECT_TRUE(preview->refused.empty());
    ASSERT_EQ(preview->changes.size(), 1U);
    EXPECT_EQ(preview->changes.front().path, "left.cpp");
}

TEST(StructuralEdit, extract_function_and_move_definition_produce_compiling_minimal_changes) {
    edit_repository extract_fixture;
    const auto source = std::string{"int compute(int input) {\n    int total = input;\n    total "
                                    "+= 2;\n    return total;\n}\n"};
    write(extract_fixture.repository(), "extract.cpp", source);
    const auto function_name = source.find("compute");
    store_symbols(
        extract_fixture.repository(),
        "extract.cpp",
        source,
        {occurrence(
            source, "compute", liquid::symbol_role::definition, function_name, "c++:compute")},
        liquid::symbol_origin::syntactic);
    const auto selection_start = source.find("    total += 2;");
    const auto selection_end = source.find('\n', selection_start) + 1;
    auto extracted = liquid::runtime::block_on(extract_fixture.repository().preview_edit(
        liquid::extract_request{"extract.cpp",
                                range(source, selection_start, selection_end),
                                "add_two",
                                liquid::extract_kind::function},
        {"test", "extract", std::nullopt}));
    ASSERT_TRUE(extracted) << extracted.error().message;
    ASSERT_TRUE(extracted->refused.empty())
        << (extracted->refused.empty() ? "" : extracted->refused.front().reason);
    ASSERT_EQ(extracted->changes.size(), 1U);
    EXPECT_EQ(extracted->changes.front().changes.size(), 2U);
    auto extract_result =
        liquid::runtime::block_on(extract_fixture.repository().apply_edit(*extracted));
    ASSERT_TRUE(extract_result) << extract_result.error().message;
    auto extracted_source =
        liquid::runtime::block_on(extract_fixture.repository().read("extract.cpp"));
    ASSERT_TRUE(extracted_source);
    EXPECT_NE(text(*extracted_source).find("static void add_two(int& total)"), std::string::npos);
    EXPECT_NE(text(*extracted_source).find("add_two(total);"), std::string::npos);
    EXPECT_TRUE(compile(extract_fixture.path(), "extract-after", text(*extracted_source)));

    edit_repository move_fixture;
    const auto from =
        std::string{"#include \"shared.hpp\"\nint moved() { return shared_value; }\n"};
    const auto to =
        std::string{"#include \"shared.hpp\"\nint moved();\nint use() { return moved(); }\n"};
    write(move_fixture.repository(), "from.cpp", from);
    write(move_fixture.repository(), "to.cpp", to);
    const auto moved_name = from.find("moved");
    store_symbols(
        move_fixture.repository(),
        "from.cpp",
        from,
        {occurrence(from, "moved", liquid::symbol_role::definition, moved_name, "c++:moved")});
    store_symbols(
        move_fixture.repository(),
        "to.cpp",
        to,
        {occurrence(to, "moved", liquid::symbol_role::reference, to.find("moved"), "c++:moved"),
         occurrence(to,
                    "moved",
                    liquid::symbol_role::reference,
                    to.find("moved", to.find("moved") + 1),
                    "c++:moved")});
    auto moved = liquid::runtime::block_on(move_fixture.repository().preview_edit(
        liquid::move_request{liquid::edit_source_location{"to.cpp", 3, 20}, "to.cpp", std::nullopt},
        {"test", "move", std::nullopt}));
    ASSERT_TRUE(moved) << moved.error().message;
    ASSERT_TRUE(moved->refused.empty());
    ASSERT_EQ(moved->changes.size(), 2U);
    auto move_result = liquid::runtime::block_on(move_fixture.repository().apply_edit(*moved));
    ASSERT_TRUE(move_result) << move_result.error().message;
    EXPECT_EQ(move_result->drips.size(), 2U);
    auto from_after = liquid::runtime::block_on(move_fixture.repository().read("from.cpp"));
    auto to_after = liquid::runtime::block_on(move_fixture.repository().read("to.cpp"));
    ASSERT_TRUE(from_after);
    ASSERT_TRUE(to_after);
    auto header = std::ofstream{move_fixture.path() / "shared.hpp"};
    header << "inline constexpr int shared_value = 3;\n";
    header.close();
    EXPECT_TRUE(compile(move_fixture.path(), "from-after", text(*from_after)));
    EXPECT_TRUE(compile(move_fixture.path(), "to-after", text(*to_after)));
}

TEST(StructuralEdit, extract_value_initializes_a_local_and_compiles) {
    edit_repository fixture;
    const auto source =
        std::string{"int compute(int input) {\n    int total = input + 1;\n    return total;\n}\n"};
    write(fixture.repository(), "value.cpp", source);
    store_symbols(fixture.repository(),
                  "value.cpp",
                  source,
                  {occurrence(source,
                              "compute",
                              liquid::symbol_role::definition,
                              source.find("compute"),
                              "c++:compute")},
                  liquid::symbol_origin::syntactic);
    const auto start = source.find("input + 1");
    const auto selected = range(source, start, start + std::string_view{"input + 1"}.size());
    auto preview = liquid::runtime::block_on(fixture.repository().preview_edit(
        liquid::extract_request{"value.cpp", selected, "incremented", liquid::extract_kind::value},
        {"test", "extract", std::nullopt}));
    ASSERT_TRUE(preview) << preview.error().message;
    ASSERT_TRUE(preview->refused.empty());
    auto applied = liquid::runtime::block_on(fixture.repository().apply_edit(*preview));
    ASSERT_TRUE(applied) << applied.error().message;
    auto after = liquid::runtime::block_on(fixture.repository().read("value.cpp"));
    ASSERT_TRUE(after);
    EXPECT_NE(text(*after).find("const auto incremented = input + 1;"), std::string::npos);
    EXPECT_TRUE(compile(fixture.path(), "value-after", text(*after)));
}

TEST(StructuralEdit, tree_edit_fingerprint_refuses_stale_nodes) {
    edit_repository fixture;
    const auto source = std::string{"int value() { return 1; }\n"};
    write(fixture.repository(), "tree.cpp", source);
    const auto start = source.find('1');
    const auto selected = range(source, start, start + 1);
    const auto target = liquid::edit_target{
        "tree.cpp", {"number_literal", selected, fingerprint(source, selected)}};
    auto preview = liquid::runtime::block_on(fixture.repository().preview_edit(
        liquid::tree_edit_request{{{target, liquid::tree_edit_operation::replace_node, "2"}}},
        {"test", "tree_edit", std::nullopt}));
    ASSERT_TRUE(preview);
    ASSERT_TRUE(preview->refused.empty());
    ASSERT_EQ(preview->changes.size(), 1U);
    auto applied = liquid::runtime::block_on(fixture.repository().apply_edit(*preview));
    ASSERT_TRUE(applied);
    auto after = liquid::runtime::block_on(fixture.repository().read("tree.cpp"));
    ASSERT_TRUE(after);
    EXPECT_TRUE(compile(fixture.path(), "tree-after", text(*after)));

    auto stale = liquid::runtime::block_on(fixture.repository().preview_edit(
        liquid::tree_edit_request{{{target, liquid::tree_edit_operation::replace_node, "3"}}},
        {"test", "tree_edit", std::nullopt}));
    ASSERT_TRUE(stale);
    EXPECT_TRUE(stale->changes.empty());
    ASSERT_EQ(stale->refused.size(), 1U);
    EXPECT_EQ(stale->refused.front().reason, "stale node fingerprint");
}

TEST(StructuralEdit, tree_edit_supports_all_four_operations) {
    edit_repository fixture;
    const auto source =
        std::string{"int value() { int a = 1; int b = 2; int c = 3; 4; return a + b + c; }\n"};
    write(fixture.repository(), "operations.cpp", source);
    const auto one = source.find('1');
    const auto two = source.find('2');
    const auto three = source.find('3');
    const auto four = source.find('4');
    const auto target = [&](std::size_t offset) {
        const auto selected = range(source, offset, offset + 1);
        return liquid::edit_target{"operations.cpp",
                                   {"number_literal", selected, fingerprint(source, selected)}};
    };
    auto preview = liquid::runtime::block_on(fixture.repository().preview_edit(
        liquid::tree_edit_request{{
            {target(one), liquid::tree_edit_operation::replace_node, "5"},
            {target(two), liquid::tree_edit_operation::insert_before, "10 + "},
            {target(three), liquid::tree_edit_operation::insert_after, " + 20"},
            {target(four), liquid::tree_edit_operation::delete_node, {}},
        }},
        {"test", "tree_edit", std::nullopt}));
    ASSERT_TRUE(preview) << preview.error().message;
    ASSERT_TRUE(preview->refused.empty());
    auto applied = liquid::runtime::block_on(fixture.repository().apply_edit(*preview));
    ASSERT_TRUE(applied) << applied.error().message;
    auto after = liquid::runtime::block_on(fixture.repository().read("operations.cpp"));
    ASSERT_TRUE(after);
    EXPECT_TRUE(compile(fixture.path(), "operations-after", text(*after)));
}

TEST(StructuralEdit, apply_refuses_a_stale_file_before_writing) {
    edit_repository fixture;
    const auto left = std::string{"int left() { return 1; }\n"};
    const auto right = std::string{"int right() { return 2; }\n"};
    write(fixture.repository(), "left.cpp", left);
    write(fixture.repository(), "right.cpp", right);
    const auto left_number = range(left, left.find('1'), left.find('1') + 1);
    const auto right_number = range(right, right.find('2'), right.find('2') + 1);
    auto preview = liquid::runtime::block_on(fixture.repository().preview_edit(
        liquid::tree_edit_request{{
            {{"left.cpp", {"number_literal", left_number, fingerprint(left, left_number)}},
             liquid::tree_edit_operation::replace_node,
             "3"},
            {{"right.cpp", {"number_literal", right_number, fingerprint(right, right_number)}},
             liquid::tree_edit_operation::replace_node,
             "4"},
        }},
        {"test", "tree_edit", std::nullopt}));
    ASSERT_TRUE(preview) << preview.error().message;
    write(fixture.repository(), "right.cpp", "int right() { return 9; }\n");
    auto applied = liquid::runtime::block_on(fixture.repository().apply_edit(*preview));
    ASSERT_FALSE(applied);
    EXPECT_EQ(applied.error().code, liquid::errc::conflict);
    EXPECT_EQ(applied.error().context, "right.cpp");
    auto unchanged = liquid::runtime::block_on(fixture.repository().read("left.cpp"));
    ASSERT_TRUE(unchanged);
    EXPECT_EQ(text(*unchanged), left);
}

TEST(StructuralEdit, tree_edit_compiles_a_fixture_copy_of_store_faults) {
    edit_repository fixture;
    const auto root = std::filesystem::path{LIQUID_SOURCE_ROOT};
    const auto source = read_source(root / "src/store/faults.cpp");
    const auto header = read_source(root / "src/store/faults.hpp");
    write(fixture.repository(), "faults.cpp", source);
    auto header_file =
        std::ofstream{fixture.path() / "faults.hpp", std::ios::binary | std::ios::trunc};
    header_file.write(header.data(), static_cast<std::streamsize>(header.size()));
    header_file.close();
    const auto marker = source.find("injector->visit(event);");
    ASSERT_NE(marker, std::string::npos);
    const auto selected = range(source, marker, marker + std::string_view{"injector"}.size());
    const auto target =
        liquid::edit_target{"faults.cpp", {"identifier", selected, fingerprint(source, selected)}};
    auto preview = liquid::runtime::block_on(fixture.repository().preview_edit(
        liquid::tree_edit_request{{
            {target, liquid::tree_edit_operation::insert_before, "/* structural edit */ "},
        }},
        {"test", "tree_edit", std::nullopt}));
    ASSERT_TRUE(preview) << preview.error().message;
    ASSERT_TRUE(preview->refused.empty());
    auto applied = liquid::runtime::block_on(fixture.repository().apply_edit(*preview));
    ASSERT_TRUE(applied) << applied.error().message;
    auto after = liquid::runtime::block_on(fixture.repository().read("faults.cpp"));
    ASSERT_TRUE(after);
    const auto input = fixture.path() / "faults-after.cpp";
    auto output = std::ofstream{input, std::ios::binary | std::ios::trunc};
    output.write(reinterpret_cast<const char*>(after->data()),
                 static_cast<std::streamsize>(after->size()));
    output.close();
    const auto command = std::string{"\"" LIQUID_CXX_COMPILER "\" -std=c++20 -I\""} +
                         fixture.path().string() + "\" -I\"" + (root / "include").string() +
                         "\" -I\"" + std::filesystem::path{LIQUID_BINARY_ROOT}.string() +
                         "/include\" -c \"" + input.string() + "\" -o \"" +
                         (fixture.path() / "faults-after.o").string() + "\"";
    EXPECT_EQ(std::system(command.c_str()), 0);
}

#if !defined(_WIN32) && defined(LIQUID_CLI)
TEST(StructuralEdit, daemon_round_trip_and_cli_json_use_the_public_records) {
    edit_repository fixture;
    const auto source = std::string{"int value() { return 1; }\n"};
    write(fixture.repository(), "daemon.cpp", source);
    const auto start = source.find('1');
    const auto selected = range(source, start, start + 1);
    const auto target = liquid::edit_target{
        "daemon.cpp", {"number_literal", selected, fingerprint(source, selected)}};
    auto request = liquid::edit_request{
        liquid::tree_edit_request{{{target, liquid::tree_edit_operation::replace_node, "2"}}}};
    auto started = liquid::runtime::block_on(liquid::daemon::start(fixture.path()));
    ASSERT_TRUE(started) << started.error().message;
    auto attached = liquid::runtime::block_on(liquid::daemon::attach(fixture.path()));
    ASSERT_TRUE(attached);
    auto preview =
        liquid::runtime::block_on(attached->preview_edit(request, {"test", "tree_edit", "daemon"}));
    ASSERT_TRUE(preview) << preview.error().message;
    auto applied = liquid::runtime::block_on(attached->apply_edit(*preview));
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(applied->drips.size(), 1U);
    EXPECT_TRUE(liquid::runtime::block_on(attached->shutdown()));
    EXPECT_TRUE(liquid::runtime::block_on(started->wait()));

    edit_repository cli_fixture;
    write(cli_fixture.repository(), "cli.cpp", source);
    const auto cli_target =
        liquid::edit_target{"cli.cpp", {"number_literal", selected, fingerprint(source, selected)}};
    auto cli_request = liquid::edit_request{
        liquid::tree_edit_request{{{cli_target, liquid::tree_edit_operation::replace_node, "2"}}}};
    auto expected_preview = liquid::runtime::block_on(
        cli_fixture.repository().preview_edit(cli_request, {"liquid", "tree_edit", std::nullopt}));
    ASSERT_TRUE(expected_preview) << expected_preview.error().message;
    auto encoded = liquid::encode_edit_request(cli_request);
    ASSERT_TRUE(encoded);
    const auto request_path = cli_fixture.path() / "request.cbor";
    auto request_file = std::ofstream{request_path, std::ios::binary | std::ios::trunc};
    request_file.write(reinterpret_cast<const char*>(encoded->data()),
                       static_cast<std::streamsize>(encoded->size()));
    request_file.close();
    const auto output_path = cli_fixture.path() / "preview.json";
    auto child_environment = std::string{};
    const auto temporary_root = std::filesystem::temp_directory_path();
    if (temporary_root.is_relative()) {
        const auto absolute_root = std::filesystem::absolute(temporary_root);
        const auto relative_root = std::filesystem::relative(absolute_root, cli_fixture.path());
        child_environment = "TMPDIR=\"" + relative_root.string() + "\" ";
    }
    const auto command = std::string{"cd \""} + cli_fixture.path().string() + "\" && " +
                         child_environment + "\"" + LIQUID_CLI + "\" edit apply \"" +
                         request_path.string() + "\" --json > \"" + output_path.string() + "\"";
    ASSERT_EQ(std::system(command.c_str()), 0);
    auto output = std::ifstream{output_path};
    const auto json = std::string{std::istreambuf_iterator<char>{output}, {}};
    const auto& expected_file = expected_preview->changes.front();
    const auto& expected_change = expected_file.changes.front();
    const auto expected_json = std::format(
        "{{\"v\":1,\"id\":\"{}\",\"changes\":[{{\"path\":\"cli.cpp\",\"before\":\"{}\","
        "\"after\":\"{}\",\"changes\":[{{\"range\":{{\"start\":{{\"byte\":{},\"line\":{},"
        "\"column\":{}}},\"end\":{{\"byte\":{},\"line\":{},\"column\":{}}}}},\"old_text\":"
        "\"1\",\"new_text\":\"2\"}}]}}],\"refused\":[],\"attribution\":{{\"tool\":\"liquid\","
        "\"operation\":\"tree_edit\",\"agent\":null}}}}\n",
        liquid::to_string(expected_preview->id),
        liquid::to_string(expected_file.before),
        liquid::to_string(expected_file.after),
        expected_change.range.start.byte,
        expected_change.range.start.line,
        expected_change.range.start.column,
        expected_change.range.end.byte,
        expected_change.range.end.line,
        expected_change.range.end.column);
    EXPECT_EQ(json, expected_json);
    auto cli_daemon = liquid::runtime::block_on(liquid::daemon::attach(cli_fixture.path()));
    ASSERT_TRUE(cli_daemon);
    EXPECT_TRUE(liquid::runtime::block_on(cli_daemon->shutdown()));
}
#endif

} // namespace
