// SPDX-License-Identifier: MIT

#include <filesystem>
#include <print>
#include <string>
#include <string_view>

#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace liquid_cli {
namespace {

auto fail(const liquid::error& error) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(error.code), error.message, error.context);
    return 1;
}

auto usage() -> int {
    std::println(stderr, "error: usage: conflict <show|resolve> <path>");
    return 2;
}

auto kind_name(liquid::conflict_kind kind) -> std::string_view {
    switch (kind) {
    case liquid::conflict_kind::text:
        return "text";
    case liquid::conflict_kind::structural:
        return "structural";
    case liquid::conflict_kind::crdt_suspicious:
        return "crdt-suspicious";
    case liquid::conflict_kind::suspicious_merge:
        return "suspicious_merge";
    }
    return "text";
}

auto json_string(std::string_view value) -> std::string {
    auto output = std::string{"\""};
    for (const auto character : value) {
        if (character == '\\' || character == '"') {
            output.push_back('\\');
        }
        output.push_back(character);
    }
    output.push_back('"');
    return output;
}

} // namespace

auto conflict(int argc, char** argv) -> int {
    if (argc != 2) {
        return usage();
    }
    const auto operation = std::string_view{argv[0]};
    const auto path = std::string{argv[1]};
    if (operation != "show" && operation != "resolve") {
        return usage();
    }
    auto repository =
        liquid::runtime::block_on(liquid::repository::open(std::filesystem::current_path()));
    if (!repository) {
        return fail(repository.error());
    }
    auto value = liquid::runtime::block_on(repository->conflict_at(path));
    if (!value) {
        return fail(value.error());
    }
    if (operation == "resolve") {
        auto resolved = liquid::runtime::block_on(repository->resolve_conflict(path));
        if (!resolved) {
            return fail(resolved.error());
        }
        std::println("change={} resolved={}",
                     liquid::to_string(resolved->value),
                     liquid::to_string(value->id));
        return 0;
    }
    std::println("conflict={}", liquid::to_string(value->id));
    std::println("path={}", value->region.path);
    std::println("kind={}", kind_name(value->kind));
    std::println("base={}", liquid::to_string(value->base));
    std::println("left={}", liquid::to_string(value->left));
    std::println("right={}", liquid::to_string(value->right));
    if (value->region.bytes) {
        std::println("byte_range={}+{}", value->region.bytes->offset, value->region.bytes->length);
    } else if (value->region.nodes) {
        std::println("node_range=present");
    }
    std::println("explanation={}", value->explanation);
    return 0;
}

auto conflicts(int argc, char** argv) -> int {
    const auto json = argc == 1 && std::string_view{argv[0]} == "--json";
    if (argc != 0 && !json) {
        std::println(stderr, "error: usage: conflicts [--json]");
        return 2;
    }
    auto repository =
        liquid::runtime::block_on(liquid::repository::open(std::filesystem::current_path()));
    if (!repository) {
        return fail(repository.error());
    }
    auto values = liquid::runtime::block_on(repository->conflicts());
    if (!values) {
        return fail(values.error());
    }
    if (json) {
        std::print("[");
    }
    for (auto index = std::size_t{}; index < values->size(); ++index) {
        const auto& value = (*values)[index];
        if (json) {
            if (index != 0) {
                std::print(",");
            }
            std::print("{{\"id\":{},\"kind\":{},\"path\":{},\"reasons\":[",
                       json_string(liquid::to_string(value.id)),
                       json_string(kind_name(value.kind)),
                       json_string(value.region.path));
            for (auto reason = std::size_t{}; reason < value.reasons.size(); ++reason) {
                if (reason != 0) {
                    std::print(",");
                }
                std::print("{}", json_string(value.reasons[reason]));
            }
            std::print("]");
            if (value.node) {
                std::print(",\"node\":{}", json_string(*value.node));
            }
            std::print("}}");
        } else {
            std::println("{}\t{}\t{}\t{}",
                         liquid::to_string(value.id),
                         kind_name(value.kind),
                         value.region.path,
                         value.reasons.empty() ? value.explanation : value.reasons.front());
        }
    }
    if (json) {
        std::println("]");
    }
    return 0;
}

} // namespace liquid_cli
