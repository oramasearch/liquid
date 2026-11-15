// SPDX-License-Identifier: MIT

#include <filesystem>
#include <print>
#include <string_view>

#include "liquid/build.hpp"
#include "liquid/error.hpp"
#include "liquid/hash.hpp"
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

auto display_text(std::string_view value) -> std::string {
    auto output = std::string{};
    for (const auto character : value) {
        if (character == '\n') {
            output += "\\n";
        } else if (character == '\r') {
            output += "\\r";
        } else if (character == '\t') {
            output += "\\t";
        } else if (character == '\0') {
            output += "\\0";
        } else {
            output.push_back(character);
        }
    }
    return output;
}

auto print_fact(const liquid::configure_fact& fact) -> void {
    std::println("fact={} target={} toolchain={} sysroot={} exit={} probe={}",
                 liquid::to_string(fact.id),
                 fact.key.target,
                 liquid::to_string(fact.key.toolchain),
                 liquid::to_string(fact.key.sysroot),
                 fact.answer.exit,
                 display_text(fact.probe));
    for (const auto& [name, answer] : fact.answer.variables) {
        std::println("variable={} value={}", name, answer);
    }
}

} // namespace

auto facts(int argc, char** argv) -> int {
    if (argc < 1) {
        return usage("facts requires list, show, or export");
    }
    const auto operation = std::string_view{argv[0]};
    if (operation == "list") {
        if (argc != 1) {
            return usage("facts list accepts no arguments");
        }
        auto listed = liquid::runtime::block_on(liquid::build::facts_list());
        if (!listed) {
            return fail(listed.error());
        }
        for (const auto& fact : *listed) {
            print_fact(fact);
        }
        return 0;
    }
    if (operation == "show") {
        if (argc != 2) {
            return usage("facts show requires one digest");
        }
        auto id = liquid::parse_digest(argv[1]);
        if (!id) {
            return usage("facts show requires a valid digest");
        }
        auto shown = liquid::runtime::block_on(liquid::build::facts_show(*id));
        if (!shown) {
            return fail(shown.error());
        }
        print_fact(*shown);
        return 0;
    }
    if (operation == "export") {
        if (argc != 1) {
            return usage("facts export accepts no arguments");
        }
        auto exported = liquid::runtime::block_on(liquid::build::facts_export());
        if (!exported) {
            return fail(exported.error());
        }
        std::println("{}", *exported);
        return 0;
    }
    return usage("facts requires list, show, or export");
}

} // namespace liquid_cli
