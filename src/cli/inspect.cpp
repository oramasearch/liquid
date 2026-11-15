// SPDX-License-Identifier: MIT

#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/format.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace liquid_cli {
namespace {

auto fail(const liquid::error& value) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(value.code), value.message, value.context);
    return 1;
}

auto read_file(const std::filesystem::path& path) -> liquid::result<std::vector<std::byte>> {
    auto input = std::ifstream{path, std::ios::binary | std::ios::ate};
    if (!input) {
        return std::unexpected(
            liquid::error{liquid::errc::io, "cannot read inspect input", path.string()});
    }
    const auto end = input.tellg();
    if (end < 0) {
        return std::unexpected(
            liquid::error{liquid::errc::io, "cannot size inspect input", path.string()});
    }
    auto output = std::vector<std::byte>(static_cast<std::size_t>(end));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size()));
    if (!input && !output.empty()) {
        return std::unexpected(
            liquid::error{liquid::errc::io, "cannot read inspect input", path.string()});
    }
    return output;
}

auto json_string(std::string_view value) -> std::string {
    auto output = std::string{"\""};
    for (const auto character : value) {
        if (character == '\\' || character == '"') {
            output += '\\';
        }
        output += character;
    }
    return output + '"';
}

auto tree_json(const liquid::format_tree& tree) -> std::string {
    auto output = "{\"codec\":" + json_string(tree.codec) +
                  ",\"tier\":" + std::to_string(static_cast<unsigned>(tree.tier)) +
                  ",\"truncated\":" + (tree.truncated ? "true" : "false") + ",\"entries\":[";
    for (auto index = std::size_t{}; index < tree.entries.size(); ++index) {
        const auto& entry = tree.entries[index];
        output += index == 0 ? "" : ",";
        output += "{\"path\":" + json_string(entry.path) +
                  ",\"kind\":" + std::to_string(static_cast<unsigned>(entry.kind)) +
                  ",\"size\":" + std::to_string(entry.size) +
                  ",\"opaque_reason\":" + json_string(entry.opaque_reason) + "}";
    }
    return output + "]}";
}

} // namespace

auto inspect(int argc, char** argv) -> int {
    auto json = false;
    auto operand = std::string_view{};
    for (auto index = 0; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--json") {
            json = true;
        } else if (argument.starts_with('-') || !operand.empty()) {
            std::println(stderr, "error: usage: inspect <path|digest> [--json]");
            return 2;
        } else {
            operand = argument;
        }
    }
    if (operand.empty()) {
        std::println(stderr, "error: usage: inspect <path|digest> [--json]");
        return 2;
    }
    auto contents = liquid::result<std::vector<std::byte>>{std::unexpected(liquid::error{
        liquid::errc::not_found, "inspect input does not exist", std::string{operand}})};
    const auto path = std::filesystem::path{operand};
    if (std::filesystem::is_regular_file(path)) {
        contents = read_file(path);
    } else if (const auto id = liquid::parse_digest(operand)) {
        auto repository =
            liquid::runtime::block_on(liquid::repository::open(std::filesystem::current_path()));
        if (!repository) {
            return fail(repository.error());
        }
        contents = liquid::runtime::block_on(repository->blob(*id));
    }
    if (!contents) {
        return fail(contents.error());
    }
    auto tree = liquid::format_registry::global().unpack(*contents, operand);
    if (!tree) {
        return fail(tree.error());
    }
    if (json) {
        std::println("{}", tree_json(*tree));
    } else {
        std::println("codec: {}", tree->codec);
        std::println("tier: {}", static_cast<unsigned>(tree->tier));
        for (const auto& entry : tree->entries) {
            std::println("entry: {} size={}", entry.path, entry.size);
        }
    }
    return 0;
}

} // namespace liquid_cli
