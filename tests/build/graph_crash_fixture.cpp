#include <chrono>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "liquid/graph.hpp"
#include "liquid/runtime.hpp"

namespace {

using namespace std::chrono_literals;

auto as_bytes(std::string_view value) -> std::vector<std::byte> {
    auto output = std::vector<std::byte>(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        output[index] = static_cast<std::byte>(value[index]);
    }
    return output;
}

auto as_text(std::span<const std::byte> value) -> std::string {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

auto compile_query(liquid::query_context& context, std::vector<std::byte> arguments)
    -> liquid::runtime::task<std::vector<std::byte>> {
    auto source = co_await context.read(
        liquid::graph_input_key{liquid::graph_input_kind::working_tree_blob, as_text(arguments)});
    if (!source) {
        co_return std::unexpected(source.error());
    }
    auto text = as_text(source->bytes);
    if (const auto comment = text.find("//"); comment != std::string::npos) {
        text.erase(comment);
    }
    auto digits = std::string{};
    for (const auto character : text) {
        if (character >= '0' && character <= '9') {
            digits += character;
        }
    }
    co_return as_bytes(digits);
}

auto interrupted_query(liquid::query_context& context,
                       std::vector<std::byte> arguments,
                       std::filesystem::path marker)
    -> liquid::runtime::task<std::vector<std::byte>> {
    auto stable = co_await context.read(liquid::query_key{"compile", std::move(arguments)});
    if (!stable) {
        co_return std::unexpected(stable.error());
    }
    auto marked = co_await liquid::runtime::offload([marker = std::move(marker)] {
        auto output = std::ofstream{marker, std::ios::trunc};
        output << "ready\n";
        return output.good();
    });
    if (!marked) {
        co_return std::unexpected(marked.error());
    }
    if (!*marked) {
        co_return std::unexpected(
            liquid::error{liquid::errc::io, "could not write marker", "graph crash fixture"});
    }
    auto [delay_error] = co_await liquid::runtime::delay(1h);
    if (delay_error) {
        co_return std::unexpected(
            liquid::error{liquid::errc::unavailable, "delay cancelled", "graph crash fixture"});
    }
    co_return std::move(stable->bytes);
}

auto run(const std::filesystem::path& state,
         const std::filesystem::path& tree,
         const std::filesystem::path& marker) -> liquid::runtime::task<void> {
    auto registry = liquid::query_registry{};
    auto added = registry.add({.name = "compile", .compute = compile_query});
    if (!added) {
        co_return std::unexpected(added.error());
    }
    added = registry.add(
        {.name = "interrupted",
         .compute = [marker](liquid::query_context& context, std::vector<std::byte> arguments) {
             return interrupted_query(context, std::move(arguments), marker);
         }});
    if (!added) {
        co_return std::unexpected(added.error());
    }
    auto opened = co_await liquid::graph::open(state, std::move(registry), {.working_tree = tree});
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    auto stable = co_await opened->demand({"compile", as_bytes("unit.cpp")});
    if (!stable) {
        co_return std::unexpected(stable.error());
    }
    auto interrupted = co_await opened->demand({"interrupted", as_bytes("unit.cpp")});
    if (!interrupted) {
        co_return std::unexpected(interrupted.error());
    }
    co_return liquid::result<void>{};
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        return 2;
    }
    auto result = liquid::runtime::block_on(run(argv[1], argv[2], argv[3]));
    return result ? 0 : 1;
}
