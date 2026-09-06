#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/cbor.hpp"
#include "common/module_support.hpp"
#include "common/syntax.hpp"
#include "liquid/frontend_abi.h"

namespace {

using liquid_frontend::cbor_value;

auto bytes_value(liquid_frontend_bytes_v1 value) -> std::span<const std::uint8_t> {
    return {value.data, value.size};
}

auto text_field(const cbor_value& value, std::string_view name) -> const std::string* {
    const auto* child = liquid_frontend::cbor_find(value, name);
    return child ? std::get_if<std::string>(&child->data) : nullptr;
}

auto bytes_field(const cbor_value& value, std::string_view name) -> const cbor_value::bytes* {
    const auto* child = liquid_frontend::cbor_find(value, name);
    return child ? std::get_if<cbor_value::bytes>(&child->data) : nullptr;
}

auto uint_field(const cbor_value& value, std::string_view name) -> const std::uint64_t* {
    const auto* child = liquid_frontend::cbor_find(value, name);
    return child ? std::get_if<std::uint64_t>(&child->data) : nullptr;
}

auto range_start(const cbor_value& unit) -> const std::uint64_t* {
    const auto* range = liquid_frontend::cbor_find(unit, "range");
    const auto* start = range ? liquid_frontend::cbor_find(*range, "start") : nullptr;
    return start ? uint_field(*start, "byte") : nullptr;
}

auto range_end(const cbor_value& unit) -> const std::uint64_t* {
    const auto* range = liquid_frontend::cbor_find(unit, "range");
    const auto* end = range ? liquid_frontend::cbor_find(*range, "end") : nullptr;
    return end ? uint_field(*end, "byte") : nullptr;
}

auto quoted_includes(std::span<const std::uint8_t> source) -> std::vector<std::string> {
    const auto text = std::string_view{reinterpret_cast<const char*>(source.data()), source.size()};
    auto output = std::vector<std::string>{};
    auto position = std::size_t{};
    while (position < text.size()) {
        const auto line_end = text.find('\n', position);
        const auto line = text.substr(position,
                                      line_end == std::string_view::npos ? text.size() - position
                                                                         : line_end - position);
        auto cursor = line.find("#include");
        if (cursor != std::string_view::npos) {
            cursor = line.find('"', cursor + 8);
            const auto end = cursor == std::string_view::npos ? std::string_view::npos
                                                              : line.find('"', cursor + 1);
            if (cursor != std::string_view::npos && end != std::string_view::npos) {
                output.emplace_back(line.substr(cursor + 1, end - cursor - 1));
            }
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        position = line_end + 1;
    }
    std::ranges::sort(output);
    output.erase(std::unique(output.begin(), output.end()), output.end());
    return output;
}

auto source_for_unit(std::span<const std::uint8_t> source,
                     std::uint64_t target_start,
                     std::uint64_t target_end) -> std::vector<std::uint8_t> {
    auto output = std::vector<std::uint8_t>{source.begin(), source.end()};
    auto units = liquid_frontend::parse_units(liquid_frontend::syntax_language::cpp, source);
    auto replacements = std::vector<std::pair<std::size_t, std::size_t>>{};
    for (const auto& unit : units) {
        if (unit.kind != 2 || (unit.start.byte == target_start && unit.end.byte == target_end)) {
            continue;
        }
        if (unit.body_start < unit.body_end && unit.body_end <= source.size()) {
            replacements.emplace_back(unit.body_start, unit.body_end);
        }
    }
    std::ranges::sort(replacements, std::greater<>{}, &std::pair<std::size_t, std::size_t>::first);
    for (const auto& [start, end] : replacements) {
        output.erase(output.begin() + static_cast<std::ptrdiff_t>(start),
                     output.begin() + static_cast<std::ptrdiff_t>(end));
        output.insert(output.begin() + static_cast<std::ptrdiff_t>(start), ';');
    }
    return output;
}

auto shell_quote(std::string_view value) -> std::string {
    auto output = std::string{"'"};
    for (const auto character : value) {
        if (character == '\'') {
            output += "'\\''";
        } else {
            output += character;
        }
    }
    output += '\'';
    return output;
}

class assembly_directory {
public:
    assembly_directory() {
        static auto sequence = std::atomic_uint64_t{};
        auto code = std::error_code{};
        const auto parent = std::filesystem::temp_directory_path(code);
        if (code) {
            return;
        }
        for (auto attempt = std::uint64_t{}; attempt < 100; ++attempt) {
            path_ = parent / ("liquid-frontend-assembly-" + std::to_string(sequence.fetch_add(1)));
            if (std::filesystem::create_directory(path_, code)) {
                return;
            }
            code.clear();
        }
        path_.clear();
    }

    ~assembly_directory() {
        if (path_.empty()) {
            return;
        }
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
    }

    auto path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    std::filesystem::path path_;
};

auto compiler_arguments(const cbor_value& context) -> std::vector<std::string> {
    auto output = std::vector<std::string>{};
    const auto* supplied = liquid_frontend::cbor_find(context, "arguments");
    const auto* values = supplied ? std::get_if<cbor_value::array>(&supplied->data) : nullptr;
    if (!values) {
        return output;
    }
    for (const auto& value : *values) {
        const auto* argument = std::get_if<std::string>(&value.data);
        if (argument) {
            output.push_back(*argument);
        }
    }
    return output;
}

auto integer_field(const nlohmann::json& value, std::string_view name)
    -> std::optional<std::size_t> {
    const auto found = value.find(name);
    if (found == value.end() || !found->is_number_unsigned()) {
        return std::nullopt;
    }
    return found->get<std::size_t>();
}

auto node_offset(const nlohmann::json& node) -> std::optional<std::size_t> {
    const auto location = node.find("loc");
    if (location != node.end() && location->is_object()) {
        if (const auto offset = integer_field(*location, "offset")) {
            return offset;
        }
    }
    const auto range = node.find("range");
    if (range == node.end() || !range->is_object()) {
        return std::nullopt;
    }
    const auto begin = range->find("begin");
    return begin == range->end() || !begin->is_object() ? std::nullopt
                                                        : integer_field(*begin, "offset");
}

auto node_end(const nlohmann::json& node, std::size_t start) -> std::size_t {
    const auto range = node.find("range");
    if (range == node.end() || !range->is_object()) {
        return start;
    }
    const auto end = range->find("end");
    if (end == range->end() || !end->is_object()) {
        return start;
    }
    const auto offset = integer_field(*end, "offset");
    const auto length = integer_field(*end, "tokLen");
    return offset && length ? *offset + *length : start;
}

auto source_point_at(std::span<const std::uint8_t> source, std::size_t offset)
    -> liquid_frontend::source_point {
    offset = std::min(offset, source.size());
    auto line = std::uint64_t{1};
    auto column = std::uint64_t{1};
    for (auto index = std::size_t{}; index < offset; ++index) {
        if (source[index] == static_cast<std::uint8_t>('\n')) {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    return liquid_frontend::source_point{offset, line, column};
}

auto definition_kind(std::string_view value) -> std::string_view {
    if (value == "FunctionDecl" || value == "CXXMethodDecl" || value == "CXXConstructorDecl" ||
        value == "CXXDestructorDecl") {
        return "function";
    }
    if (value == "CXXRecordDecl" || value == "RecordDecl" || value == "EnumDecl" ||
        value == "TypedefDecl" || value == "TypeAliasDecl") {
        return "type";
    }
    if (value == "FieldDecl") {
        return "field";
    }
    if (value == "ParmVarDecl") {
        return "parameter";
    }
    if (value == "NamespaceDecl") {
        return "namespace";
    }
    if (value == "VarDecl") {
        return "variable";
    }
    return {};
}

auto has_body(const nlohmann::json& node) -> bool {
    const auto inner = node.find("inner");
    if (inner == node.end() || !inner->is_array()) {
        return false;
    }
    return std::ranges::any_of(
        *inner, [](const auto& child) { return child.value("kind", "") == "CompoundStmt"; });
}

void extract_symbols(const nlohmann::json& node,
                     std::span<const std::uint8_t> source,
                     std::string_view container,
                     std::vector<liquid_frontend::syntax_symbol>& output) {
    const auto kind = node.value("kind", "");
    const auto offset = node_offset(node);
    const auto name = node.value("name", "");
    auto next_container = std::string{container};
    const auto definition = definition_kind(kind);
    if (offset && *offset < source.size() && !name.empty() && !definition.empty() &&
        !node.value("isImplicit", false) &&
        (kind != "FunctionDecl" && kind != "CXXMethodDecl" ? true : has_body(node))) {
        auto symbol = std::string{"c++:"};
        if (!container.empty()) {
            symbol += std::string{container} + "::";
        }
        symbol += name;
        output.push_back(
            liquid_frontend::syntax_symbol{symbol,
                                           name,
                                           std::string{definition},
                                           "definition",
                                           source_point_at(source, *offset),
                                           source_point_at(source, node_end(node, *offset)),
                                           std::string{container}});
        if (kind == "NamespaceDecl" || kind == "CXXRecordDecl" || kind == "RecordDecl" ||
            kind == "EnumDecl" || kind == "FunctionDecl" || kind == "CXXMethodDecl") {
            if (!next_container.empty()) {
                next_container += "::";
            }
            next_container += name;
        }
    }
    if (kind == "DeclRefExpr" && offset && *offset < source.size()) {
        const auto referenced = node.find("referencedDecl");
        const auto referenced_name =
            referenced == node.end() ? std::string{} : referenced->value("name", "");
        if (!referenced_name.empty()) {
            output.push_back(
                liquid_frontend::syntax_symbol{"c++:" + referenced_name,
                                               referenced_name,
                                               "unknown",
                                               "reference",
                                               source_point_at(source, *offset),
                                               source_point_at(source, node_end(node, *offset)),
                                               std::string{container}});
        }
    }
    const auto inner = node.find("inner");
    if (inner == node.end() || !inner->is_array()) {
        return;
    }
    for (const auto& child : *inner) {
        extract_symbols(child, source, next_container, output);
    }
}

auto symbols_from_ast(std::span<const std::uint8_t> source,
                      std::span<const std::uint8_t> action_output,
                      std::string& failure)
    -> std::optional<std::vector<liquid_frontend::syntax_symbol>> {
    auto tree = nlohmann::json::parse(
        reinterpret_cast<const char*>(action_output.data()),
        reinterpret_cast<const char*>(action_output.data() + action_output.size()),
        nullptr,
        false);
    if (tree.is_discarded()) {
        failure = "the compiler produced invalid AST JSON";
        return std::nullopt;
    }
    auto symbols = std::vector<liquid_frontend::syntax_symbol>{};
    extract_symbols(tree, source, {}, symbols);
    std::ranges::sort(symbols, {}, [](const auto& symbol) { return symbol.start.byte; });
    return symbols;
}

auto units(void*,
           const char* language,
           liquid_frontend_bytes_v1 blob,
           liquid_frontend_bytes_v1,
           const liquid_frontend_callbacks_v1* callbacks,
           liquid_frontend_owned_bytes_v1* output,
           liquid_frontend_error_v1* error) -> int {
    if (!language || std::string_view{language} != "c++" || !output ||
        (!blob.data && blob.size != 0)) {
        return liquid_frontend::fail(
            error, LIQUID_FRONTEND_STATUS_INVALID_ARGUMENT, "clang frontend requires C++ source");
    }
    if (liquid_frontend::cancelled(callbacks)) {
        return liquid_frontend::fail(
            error, LIQUID_FRONTEND_STATUS_CANCELLED, "unit discovery was cancelled");
    }
    liquid_frontend::progress(callbacks, "units", 0, 1);
    const auto found =
        liquid_frontend::parse_units(liquid_frontend::syntax_language::cpp, {blob.data, blob.size});
    liquid_frontend::progress(callbacks, "units", 1, 1);
    return liquid_frontend::return_bytes(liquid_frontend::encode_units(found), output);
}

auto compile_unit(void*,
                  liquid_frontend_bytes_v1 unit_bytes,
                  liquid_frontend_bytes_v1 context_bytes,
                  const liquid_frontend_callbacks_v1* callbacks,
                  liquid_frontend_owned_bytes_v1* output,
                  liquid_frontend_error_v1* error) -> int {
    if (!output) {
        return liquid_frontend::fail(
            error, LIQUID_FRONTEND_STATUS_INVALID_ARGUMENT, "compile output is required");
    }
    auto unit = liquid_frontend::cbor_decode(bytes_value(unit_bytes));
    auto context = liquid_frontend::cbor_decode(bytes_value(context_bytes));
    if (!unit || !context) {
        return liquid_frontend::fail(error,
                                     LIQUID_FRONTEND_STATUS_INVALID_ARGUMENT,
                                     "compile values must be canonical CBOR");
    }
    const auto* name = text_field(*unit, "name");
    const auto* start = range_start(*unit);
    const auto* end = range_end(*unit);
    const auto* compiler = text_field(*context, "compiler");
    const auto* source = bytes_field(*context, "source");
    if (!name || !start || !end || !compiler || !source || *end > source->size()) {
        return liquid_frontend::fail(error,
                                     LIQUID_FRONTEND_STATUS_INVALID_ARGUMENT,
                                     "compile context requires compiler and source");
    }
    if (liquid_frontend::cancelled(callbacks)) {
        return liquid_frontend::fail(
            error, LIQUID_FRONTEND_STATUS_CANCELLED, "compile action was cancelled");
    }
    auto arguments = cbor_value::array{};
    if (const auto* value = liquid_frontend::cbor_find(*context, "arguments")) {
        if (const auto* supplied = std::get_if<cbor_value::array>(&value->data)) {
            arguments = *supplied;
        }
    }
    auto reads = cbor_value::array{};
    for (auto& read : quoted_includes(*source)) {
        reads.emplace_back(std::move(read));
    }
    if (const auto* value = liquid_frontend::cbor_find(*context, "reads")) {
        if (const auto* supplied = std::get_if<cbor_value::array>(&value->data)) {
            reads.insert(reads.end(), supplied->begin(), supplied->end());
        }
    }
    liquid_frontend::progress(callbacks, "compile_unit", 0, 1);
    auto action = cbor_value::map{
        {"v", std::uint64_t{1}},
        {"kind", "clang-compile-unit"},
        {"name", *name},
        {"compiler", *compiler},
        {"arguments", std::move(arguments)},
        {"reads", std::move(reads)},
        {"source", source_for_unit(*source, *start, *end)},
    };
    liquid_frontend::progress(callbacks, "compile_unit", 1, 1);
    return liquid_frontend::return_bytes(
        liquid_frontend::cbor_encode(cbor_value{std::move(action)}), output);
}

auto assemble(void*,
              liquid_frontend_bytes_v1 unit_outputs,
              liquid_frontend_bytes_v1 context_bytes,
              const liquid_frontend_callbacks_v1* callbacks,
              liquid_frontend_owned_bytes_v1* output,
              liquid_frontend_error_v1* error) -> int {
    if (!output) {
        return liquid_frontend::fail(
            error, LIQUID_FRONTEND_STATUS_INVALID_ARGUMENT, "assembly output is required");
    }
    auto decoded = liquid_frontend::cbor_decode(bytes_value(unit_outputs));
    auto context = liquid_frontend::cbor_decode(bytes_value(context_bytes));
    const auto* units = decoded ? std::get_if<cbor_value::array>(&decoded->data) : nullptr;
    if (!units || !context) {
        return liquid_frontend::fail(error,
                                     LIQUID_FRONTEND_STATUS_INVALID_ARGUMENT,
                                     "unit outputs must be a canonical CBOR array");
    }
    if (liquid_frontend::cancelled(callbacks)) {
        return liquid_frontend::fail(
            error, LIQUID_FRONTEND_STATUS_CANCELLED, "assembly was cancelled");
    }
    liquid_frontend::progress(callbacks, "assemble", 0, 1);
#if defined(_WIN32)
    auto assembled = cbor_value::map{
        {"v", std::uint64_t{1}},
        {"kind", "clang-translation-unit-portable"},
        {"units", *units},
    };
#else
    const auto* compiler = text_field(*context, "compiler");
    if (!compiler) {
        return liquid_frontend::fail(error,
                                     LIQUID_FRONTEND_STATUS_INVALID_ARGUMENT,
                                     "assembly context requires the compiler");
    }
    auto directory = assembly_directory{};
    if (directory.path().empty()) {
        return liquid_frontend::fail(
            error, LIQUID_FRONTEND_STATUS_INTERNAL, "cannot create the assembly directory");
    }
    auto command = shell_quote(*compiler) + " -r";
    for (std::size_t index = 0; index < units->size(); ++index) {
        const auto* output_value = liquid_frontend::cbor_find((*units)[index], "output");
        const auto* object =
            output_value ? std::get_if<cbor_value::bytes>(&output_value->data) : nullptr;
        if (!object) {
            return liquid_frontend::fail(error,
                                         LIQUID_FRONTEND_STATUS_INVALID_ARGUMENT,
                                         "unit output is not an object byte string");
        }
        const auto path = directory.path() / (std::to_string(index) + ".o");
        auto stream = std::ofstream{path, std::ios::binary | std::ios::trunc};
        stream.write(reinterpret_cast<const char*>(object->data()),
                     static_cast<std::streamsize>(object->size()));
        stream.close();
        if (!stream) {
            return liquid_frontend::fail(
                error, LIQUID_FRONTEND_STATUS_INTERNAL, "cannot write a unit object");
        }
        command += " " + shell_quote(path.string());
    }
    const auto translation_unit = directory.path() / "translation-unit.o";
    command += " -o " + shell_quote(translation_unit.string());
    if (std::system(command.c_str()) != 0) {
        return liquid_frontend::fail(
            error, LIQUID_FRONTEND_STATUS_INTERNAL, "clang could not assemble the unit objects");
    }
    auto stream = std::ifstream{translation_unit, std::ios::binary};
    auto object =
        cbor_value::bytes{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    if (object.empty()) {
        return liquid_frontend::fail(
            error, LIQUID_FRONTEND_STATUS_INTERNAL, "cannot read the translation-unit object");
    }
    auto assembled = cbor_value::map{
        {"v", std::uint64_t{1}},
        {"kind", "clang-translation-unit"},
        {"object", std::move(object)},
    };
#endif
    liquid_frontend::progress(callbacks, "assemble", 1, 1);
    return liquid_frontend::return_bytes(
        liquid_frontend::cbor_encode(cbor_value{std::move(assembled)}), output);
}

auto symbols(void*,
             const char* language,
             liquid_frontend_bytes_v1 blob,
             liquid_frontend_bytes_v1 context_bytes,
             const liquid_frontend_callbacks_v1* callbacks,
             liquid_frontend_owned_bytes_v1* output,
             liquid_frontend_error_v1* error) -> int {
    if (!language || std::string_view{language} != "c++" || !output ||
        (!blob.data && blob.size != 0)) {
        return liquid_frontend::fail(
            error, LIQUID_FRONTEND_STATUS_INVALID_ARGUMENT, "clang frontend requires C++ source");
    }
    if (liquid_frontend::cancelled(callbacks)) {
        return liquid_frontend::fail(
            error, LIQUID_FRONTEND_STATUS_CANCELLED, "symbol extraction was cancelled");
    }
    const auto context = context_bytes.size == 0
                             ? std::optional<cbor_value>{cbor_value{cbor_value::map{}}}
                             : liquid_frontend::cbor_decode(bytes_value(context_bytes));
    if (!context) {
        return liquid_frontend::fail(error,
                                     LIQUID_FRONTEND_STATUS_INVALID_ARGUMENT,
                                     "symbol context must be canonical CBOR");
    }
    const auto* compile_action = text_field(*context, "compile_action");
    if (!compile_action || compile_action->empty()) {
        return liquid_frontend::fail(error,
                                     LIQUID_FRONTEND_STATUS_INVALID_ARGUMENT,
                                     "symbol context requires the recorded compile action");
    }
    auto reads = cbor_value::array{};
    for (auto& read : quoted_includes({blob.data, blob.size})) {
        reads.emplace_back(std::move(read));
    }
    if (const auto* value = liquid_frontend::cbor_find(*context, "reads")) {
        if (const auto* supplied = std::get_if<cbor_value::array>(&value->data)) {
            reads.insert(reads.end(), supplied->begin(), supplied->end());
        }
    }
    auto flags = cbor_value::array{};
    flags.emplace_back("-fsyntax-only");
    flags.emplace_back("-fparse-all-comments");
    flags.emplace_back("-Xclang");
    flags.emplace_back("-ast-dump=json");
    liquid_frontend::progress(callbacks, "symbols", 0, 1);
    auto action = cbor_value::map{
        {"v", std::uint64_t{1}},
        {"kind", "clang-symbols"},
        {"compile_action", *compile_action},
        {"flags", std::move(flags)},
        {"reads", std::move(reads)},
        {"source", cbor_value::bytes{blob.data, blob.data + blob.size}},
    };
    liquid_frontend::progress(callbacks, "symbols", 1, 1);
    return liquid_frontend::return_bytes(
        liquid_frontend::cbor_encode(cbor_value{std::move(action)}), output);
}

auto symbols_from_output(void*,
                         const char* language,
                         liquid_frontend_bytes_v1 blob,
                         liquid_frontend_bytes_v1 action_output,
                         const liquid_frontend_callbacks_v1* callbacks,
                         liquid_frontend_owned_bytes_v1* output,
                         liquid_frontend_error_v1* error) -> int {
    if (!language || std::string_view{language} != "c++" || !output ||
        (!blob.data && blob.size != 0) || (!action_output.data && action_output.size != 0)) {
        return liquid_frontend::fail(error,
                                     LIQUID_FRONTEND_STATUS_INVALID_ARGUMENT,
                                     "clang frontend requires C++ symbol action output");
    }
    if (liquid_frontend::cancelled(callbacks)) {
        return liquid_frontend::fail(
            error, LIQUID_FRONTEND_STATUS_CANCELLED, "symbol decoding was cancelled");
    }
    auto failure = std::string{};
    const auto found =
        symbols_from_ast({blob.data, blob.size}, {action_output.data, action_output.size}, failure);
    if (!found) {
        return liquid_frontend::fail(
            error, LIQUID_FRONTEND_STATUS_INTERNAL, "clang could not decode symbols: " + failure);
    }
    return liquid_frontend::return_bytes(
        liquid_frontend::encode_symbols(liquid_frontend::syntax_language::cpp, "compiler", *found),
        output);
}

constexpr const char* cpp_extensions[] = {".cc", ".cpp", ".cxx", ".hh", ".hpp", ".hxx"};
constexpr liquid_frontend_language_v1 languages[] = {{"c++", cpp_extensions, 6}};

const liquid_frontend_module_v1 module = {
    LIQUID_FRONTEND_ABI_VERSION,
    sizeof(liquid_frontend_module_v1),
    "clang-cpp",
    languages,
    1,
    nullptr,
    units,
    compile_unit,
    assemble,
    symbols,
    symbols_from_output,
    nullptr,
    nullptr,
    0,
    nullptr,
};

} // namespace

extern "C" LIQUID_FRONTEND_MODULE_EXPORT const liquid_frontend_module_v1*
liquid_frontend_get_module_v1(void) {
    return &module;
}
