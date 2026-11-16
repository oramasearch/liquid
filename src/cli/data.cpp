// SPDX-License-Identifier: MIT

#include <cstddef>
#include <filesystem>
#include <format>
#include <print>
#include <random>
#include <string>
#include <string_view>

#include "liquid/data.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace liquid_cli {
namespace {

auto fail(const liquid::error& value) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(value.code), value.message, value.context);
    return 1;
}

auto usage(std::string_view message) -> int {
    std::println(stderr, "error: usage: {} ()", message);
    return 2;
}

auto json_string(std::string_view value) -> std::string {
    auto output = std::string{"\""};
    for (const auto character : value) {
        switch (character) {
        case '\\':
            output += "\\\\";
            break;
        case '"':
            output += "\\\"";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20U) {
                output += std::format("\\u{:04x}", static_cast<unsigned char>(character));
            } else {
                output += character;
            }
        }
    }
    output += '"';
    return output;
}

auto random_session() -> liquid::session_id {
    auto output = liquid::session_id{};
    auto random = std::random_device{};
    for (auto& value : output.bytes) {
        value = std::byte{static_cast<unsigned char>(random())};
    }
    return output;
}

auto open_repository() -> liquid::result<liquid::repository> {
    return liquid::runtime::block_on(liquid::repository::open(std::filesystem::current_path()));
}

void print_branch(const liquid::data_branch_record& branch) {
    std::println("id={} session={} database={} mechanism={} url={} created_at={}",
                 branch.id,
                 liquid::to_string(branch.session),
                 branch.database,
                 liquid::to_string(branch.mechanism),
                 branch.url,
                 branch.created_at.time_since_epoch().count());
}

void print_branch_json(const liquid::data_branch_record& branch) {
    std::print("{{\"id\":{},\"session\":{},\"database\":{},\"mechanism\":{},\"url\":{},"
               "\"created_at\":{}}}",
               json_string(branch.id),
               json_string(liquid::to_string(branch.session)),
               json_string(branch.database),
               json_string(liquid::to_string(branch.mechanism)),
               json_string(branch.url),
               branch.created_at.time_since_epoch().count());
}

auto branch(int argc, char** argv) -> int {
    if (argc < 1) {
        return usage("data branch <database> [--session <id>]");
    }
    const auto database = std::string{argv[0]};
    auto session = random_session();
    for (auto index = 1; index < argc; ++index) {
        if (std::string_view{argv[index]} != "--session" || ++index == argc) {
            return usage("data branch <database> [--session <id>]");
        }
        auto parsed = liquid::parse_session_id(argv[index]);
        if (!parsed) {
            return fail(parsed.error());
        }
        session = *parsed;
    }
    auto repository = open_repository();
    if (!repository) {
        return fail(repository.error());
    }
    auto created = liquid::runtime::block_on(
        liquid::data_branch::create(*repository, session, {.name = database}));
    if (!created) {
        return fail(created.error());
    }
    print_branch(*created);
    return 0;
}

auto list(int argc, char** argv) -> int {
    const auto json = argc == 1 && std::string_view{argv[0]} == "--json";
    if (argc != 0 && !json) {
        return usage("data list [--json]");
    }
    auto repository = open_repository();
    if (!repository) {
        return fail(repository.error());
    }
    auto branches = liquid::runtime::block_on(liquid::data_branch::list(*repository));
    if (!branches) {
        return fail(branches.error());
    }
    if (json) {
        std::print("[");
        for (std::size_t index = 0; index < branches->size(); ++index) {
            if (index != 0) {
                std::print(",");
            }
            print_branch_json((*branches)[index]);
        }
        std::println("]");
        return 0;
    }
    for (const auto& branch : *branches) {
        print_branch(branch);
    }
    return 0;
}

auto mutate(int argc, char** argv, bool promote) -> int {
    if (argc != 1) {
        return usage(promote ? "data promote <id>" : "data discard <id>");
    }
    auto repository = open_repository();
    if (!repository) {
        return fail(repository.error());
    }
    auto changed =
        promote ? liquid::runtime::block_on(liquid::data_branch::promote(*repository, argv[0]))
                : liquid::runtime::block_on(liquid::data_branch::discard(*repository, argv[0]));
    if (!changed) {
        return fail(changed.error());
    }
    std::println("{} {}", promote ? "promoted" : "discarded", argv[0]);
    return 0;
}

} // namespace

auto data(int argc, char** argv) -> int {
    if (argc == 0) {
        return usage("data <branch|list|discard|promote> ...");
    }
    const auto command = std::string_view{argv[0]};
    if (command == "branch") {
        return branch(argc - 1, argv + 1);
    }
    if (command == "list") {
        return list(argc - 1, argv + 1);
    }
    if (command == "discard") {
        return mutate(argc - 1, argv + 1, false);
    }
    if (command == "promote") {
        return mutate(argc - 1, argv + 1, true);
    }
    return usage("data <branch|list|discard|promote> ...");
}

} // namespace liquid_cli
