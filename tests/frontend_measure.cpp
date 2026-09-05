#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "encoding/cbor.hpp"
#include "liquid/frontend.hpp"

namespace {

namespace cbor = liquid::encoding::cbor;

auto bytes(std::string_view value) -> std::vector<std::byte> {
    auto output = std::vector<std::byte>(value.size());
    std::ranges::copy(value, reinterpret_cast<char*>(output.data()));
    return output;
}

auto read_text(const std::filesystem::path& path) -> std::string {
    auto input = std::ifstream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
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
            liquid::error{liquid::errc::corrupt, "invalid action", "frontend measure"});
    }
    std::filesystem::create_directories(state->directory);
    const auto number = state->sequence++;
    const auto input = state->directory / (std::to_string(number) + ".cpp");
    const auto output = state->directory / (std::to_string(number) + ".o");
    auto source_file = std::ofstream{input, std::ios::binary | std::ios::trunc};
    source_file.write(reinterpret_cast<const char*>(source->data()),
                      static_cast<std::streamsize>(source->size()));
    source_file.close();
    auto command = shell_quote(*compiler);
    for (const auto& item : *arguments) {
        const auto* argument = std::get_if<std::string>(&item.data);
        if (!argument) {
            co_return std::unexpected(
                liquid::error{liquid::errc::corrupt, "invalid argument", "frontend measure"});
        }
        command += " " + shell_quote(*argument);
    }
    command += " -x c++ -c " + shell_quote(input.string());
    command += " -o " + shell_quote(output.string());
    if (std::system(command.c_str()) != 0) {
        co_return std::unexpected(
            liquid::error{liquid::errc::internal, "compiler action failed", command});
    }
    auto object = std::ifstream{output, std::ios::binary};
    auto characters =
        std::vector<char>{std::istreambuf_iterator<char>{object}, std::istreambuf_iterator<char>{}};
    auto result = std::vector<std::byte>(characters.size());
    std::ranges::copy(characters, reinterpret_cast<char*>(result.data()));
    co_return result;
}

auto context(const std::filesystem::path& root) -> liquid::result<std::vector<std::byte>> {
    return cbor::encode(map({
        {"compiler", std::string{LIQUID_CXX_COMPILER}},
        {"arguments",
         cbor::value::array{
             cbor::value{"-std=c++20"},
             cbor::value{"-I" + (root / "src/store").string()},
             cbor::value{"-I" + (root / "include").string()},
             cbor::value{"-I" + (std::filesystem::path{LIQUID_BINARY_ROOT} / "include").string()},
         }},
    }));
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: liquid-frontend-measure <source-root> <empty-state-directory>\n";
        return 2;
    }
    const auto root = std::filesystem::path{argv[1]};
    const auto state_directory = std::filesystem::path{argv[2]};
    std::filesystem::remove_all(state_directory);
    auto module_value = liquid::frontend_module::load(LIQUID_CLANG_FRONTEND);
    if (!module_value) {
        std::cerr << module_value.error().message << '\n';
        return 1;
    }
    auto module = std::make_shared<liquid::frontend_module>(std::move(*module_value));
    auto executor = std::make_shared<executor_state>();
    executor->directory = state_directory / "actions";
    auto graph = liquid::runtime::block_on(
        liquid::frontend_graph::open(state_directory / "graph",
                                     root / "src/store",
                                     module,
                                     [executor](std::vector<std::byte> action) {
                                         return execute_action(executor, std::move(action));
                                     }));
    if (!graph) {
        std::cerr << graph.error().message << '\n';
        return 1;
    }
    auto source = read_text(root / "src/store/faults.cpp");
    auto build_context = context(root);
    if (!build_context) {
        std::cerr << build_context.error().message << '\n';
        return 1;
    }
    const auto symbols_started = std::chrono::steady_clock::now();
    auto symbols_value = module->symbols("c++", bytes(source), *build_context);
    const auto symbols_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - symbols_started);
    if (!symbols_value) {
        std::cerr << symbols_value.error().message << ": " << symbols_value.error().context << '\n';
        return 1;
    }
    const auto subject = liquid::hash(liquid::algorithm::blake3, bytes(source));
    if (!subject) {
        std::cerr << subject.error().message << ": " << subject.error().context << '\n';
        return 1;
    }
    auto symbols = liquid::decode_frontend_symbols(*subject, *symbols_value);
    if (!symbols) {
        std::cerr << symbols.error().message << ": " << symbols.error().context << '\n';
        return 1;
    }
    std::cout << "symbol extraction: file=src/store/faults.cpp units=1 occurrences="
              << symbols->occurrences.size() << " elapsed_ms=" << symbols_elapsed.count()
              << " per_unit_ms=" << symbols_elapsed.count() << '\n';
    auto first = liquid::runtime::block_on(graph->build({
        .language = "c++",
        .source_path = "faults.cpp",
        .source = bytes(source),
        .context_cbor = *build_context,
    }));
    if (!first) {
        std::cerr << first.error().message << ": " << first.error().context << '\n';
        return 1;
    }
    const auto marker = source.find("injector->visit(event);");
    if (marker == std::string::npos) {
        std::cerr << "measurement edit point is missing\n";
        return 1;
    }
    source.insert(marker + std::string_view{"injector->visit(event);"}.size(),
                  " /* one-function edit */");
    const auto started = std::chrono::steady_clock::now();
    auto edited = liquid::runtime::block_on(graph->build({
        .language = "c++",
        .source_path = "faults.cpp",
        .source = bytes(source),
        .context_cbor = *build_context,
    }));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    if (!edited) {
        std::cerr << edited.error().message << ": " << edited.error().context << '\n';
        return 1;
    }
    std::cout << "frontend one-function edit: file=src/store/faults.cpp units="
              << edited->units.size() << " actions_run=" << edited->actions_run
              << " assemblies_run=" << edited->assemblies_run << " elapsed_ms=" << elapsed.count()
              << '\n';
    return edited->actions_run == 1 && edited->assemblies_run == 1 ? 0 : 1;
}
