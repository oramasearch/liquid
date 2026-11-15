// SPDX-License-Identifier: MIT

#include <filesystem>
#include <format>
#include <print>
#include <string_view>

#include "liquid/hash.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace liquid_cli {
namespace {

auto fail(const liquid::error& error) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(error.code), error.message, error.context);
    return 1;
}

auto usage(std::string_view message) -> int {
    std::println(stderr, "error: usage: {} ()", message);
    return 2;
}

} // namespace

auto retain(int argc, char** argv) -> int {
    if (argc != 2 || std::string_view{argv[0]} != "explain") {
        return usage("retain explain <digest>");
    }
    auto id = liquid::parse_digest(argv[1]);
    if (!id) {
        return fail(id.error());
    }
    auto repository =
        liquid::runtime::block_on(liquid::repository::open(std::filesystem::current_path()));
    if (!repository) {
        return fail(repository.error());
    }
    auto explanation = liquid::runtime::block_on(repository->retain_explain(*id));
    if (!explanation) {
        return fail(explanation.error());
    }
    std::println("{} {}: {}",
                 liquid::to_string(explanation->id),
                 explanation->retained ? "retained" : "evicted",
                 explanation->reason);
    return 0;
}

} // namespace liquid_cli
