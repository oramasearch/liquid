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
    std::filesystem::path source_root;
    std::uint64_t sequence{};
};

auto execute_action(std::shared_ptr<executor_state> state, std::vector<std::byte> action)
    -> liquid::runtime::task<std::vector<std::byte>> {
    auto decoded = cbor::decode(action);
    if (!decoded) {
        co_return std::unexpected(decoded.error());
    }
    const auto* kind_value = find(*decoded, "kind");
    const auto* compiler_value = find(*decoded, "compiler");
    const auto* arguments_value = find(*decoded, "arguments");
    const auto* flags_value = find(*decoded, "flags");
    const auto* source_value = find(*decoded, "source");
    const auto* kind = kind_value ? std::get_if<std::string>(&kind_value->data) : nullptr;
    const auto* compiler =
        compiler_value ? std::get_if<std::string>(&compiler_value->data) : nullptr;
    const auto* arguments =
        arguments_value ? std::get_if<cbor::value::array>(&arguments_value->data) : nullptr;
    const auto* flags = flags_value ? std::get_if<cbor::value::array>(&flags_value->data) : nullptr;
    const auto* source =
        source_value ? std::get_if<cbor::value::bytes>(&source_value->data) : nullptr;
    const auto symbols_action = kind && *kind == "clang-symbols";
    if (!source || (symbols_action && !flags) || (!symbols_action && (!compiler || !arguments))) {
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
    auto command = shell_quote(compiler ? *compiler : LIQUID_CXX_COMPILER);
    if (symbols_action) {
        command += " -std=c++20";
        command += " -I" + shell_quote((state->source_root / "src/store").string());
        command += " -I" + shell_quote((state->source_root / "include").string());
        command +=
            " -I" + shell_quote((std::filesystem::path{LIQUID_BINARY_ROOT} / "include").string());
    }
    const auto& compiler_arguments = flags ? *flags : *arguments;
    for (const auto& item : compiler_arguments) {
        const auto* argument = std::get_if<std::string>(&item.data);
        if (!argument) {
            co_return std::unexpected(
                liquid::error{liquid::errc::corrupt, "invalid argument", "frontend measure"});
        }
        command += " " + shell_quote(*argument);
    }
    command += " -x c++ ";
    if (flags) {
        command += shell_quote(input.string()) + " > " + shell_quote(output.string());
    } else {
        command += "-c " + shell_quote(input.string());
        command += " -o " + shell_quote(output.string());
    }
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
        {"compile_action", "measurement"},
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
    auto executor = std::make_shared<executor_state>();
    executor->directory = state_directory / "actions";
    executor->source_root = root;
    const auto open_graph = [&](std::string_view name) -> liquid::result<liquid::frontend_graph> {
        auto module_value = liquid::frontend_module::load(LIQUID_CLANG_FRONTEND);
        if (!module_value) {
            return std::unexpected(module_value.error());
        }
        auto module = std::make_shared<liquid::frontend_module>(std::move(*module_value));
        return liquid::runtime::block_on(
            liquid::frontend_graph::open(state_directory / std::string{name} / "graph",
                                         root / "src/store",
                                         module,
                                         [executor](std::vector<std::byte> action) {
                                             return execute_action(executor, std::move(action));
                                         }));
    };
    const auto original = read_text(root / "src/store/faults.cpp");
    auto build_context = context(root);
    if (!build_context) {
        std::cerr << build_context.error().message << '\n';
        return 1;
    }
    auto disabled = open_graph("disabled");
    if (!disabled) {
        std::cerr << disabled.error().message << ": " << disabled.error().context << '\n';
        return 1;
    }
    auto first = liquid::runtime::block_on(disabled->build({
        .language = "c++",
        .source_path = "faults.cpp",
        .source = bytes(original),
        .context_cbor = *build_context,
    }));
    if (!first) {
        std::cerr << first.error().message << ": " << first.error().context << '\n';
        return 1;
    }
    auto edited_source = original;
    const auto marker = edited_source.find("injector->visit(event);");
    if (marker == std::string::npos) {
        std::cerr << "measurement edit point is missing\n";
        return 1;
    }
    edited_source.insert(marker + std::string_view{"injector->visit(event);"}.size(),
                         " /* one-function edit */");
    const auto disabled_started = std::chrono::steady_clock::now();
    auto disabled_edited = liquid::runtime::block_on(disabled->build({
        .language = "c++",
        .source_path = "faults.cpp",
        .source = bytes(edited_source),
        .context_cbor = *build_context,
    }));
    const auto disabled_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - disabled_started);
    if (!disabled_edited) {
        std::cerr << disabled_edited.error().message << ": " << disabled_edited.error().context
                  << '\n';
        return 1;
    }
    auto enabled = open_graph("enabled");
    if (!enabled) {
        std::cerr << enabled.error().message << ": " << enabled.error().context << '\n';
        return 1;
    }
    first = liquid::runtime::block_on(enabled->build({
        .language = "c++",
        .source_path = "faults.cpp",
        .source = bytes(original),
        .context_cbor = *build_context,
    }));
    if (!first) {
        std::cerr << first.error().message << ": " << first.error().context << '\n';
        return 1;
    }
    const auto enabled_started = std::chrono::steady_clock::now();
    auto enabled_edited = liquid::runtime::block_on(enabled->build({
        .language = "c++",
        .source_path = "faults.cpp",
        .source = bytes(edited_source),
        .context_cbor = *build_context,
    }));
    const auto enabled_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - enabled_started);
    if (!enabled_edited) {
        std::cerr << enabled_edited.error().message << ": " << enabled_edited.error().context
                  << '\n';
        return 1;
    }
    const auto symbols_started = std::chrono::steady_clock::now();
    auto symbols = liquid::runtime::block_on(enabled->symbols({
        .language = "c++",
        .source_path = "faults.cpp",
        .source = bytes(edited_source),
        .context_cbor = *build_context,
        .compile_action = "measurement",
    }));
    const auto symbols_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - symbols_started);
    if (!symbols) {
        std::cerr << symbols.error().message << ": " << symbols.error().context << '\n';
        return 1;
    }
    std::cout << "symbol extraction: file=src/store/faults.cpp units=1 occurrences="
              << symbols->occurrences.size() << " elapsed_ms=" << symbols_elapsed.count()
              << " per_unit_ms=" << symbols_elapsed.count() << '\n';
    std::cout << "frontend one-function edit: symbols=disabled elapsed_ms="
              << disabled_elapsed.count()
              << " symbols=enabled elapsed_ms=" << enabled_elapsed.count()
              << " delta_ms=" << enabled_elapsed.count() - disabled_elapsed.count() << '\n';
    return disabled_edited->actions_run == 1 && disabled_edited->assemblies_run == 1 &&
                   enabled_edited->actions_run == 1 && enabled_edited->assemblies_run == 1
               ? 0
               : 1;
}
