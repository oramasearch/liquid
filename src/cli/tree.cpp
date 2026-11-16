// SPDX-License-Identifier: MIT

#include <filesystem>
#include <print>
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

auto usage(std::string_view message) -> int {
    std::println(stderr, "error: usage: {} ()", message);
    return 2;
}

auto open_repository() -> liquid::result<liquid::repository> {
    return liquid::runtime::block_on(liquid::repository::open(std::filesystem::current_path()));
}

} // namespace

auto tree_digest(int argc, char**) -> int {
    if (argc != 0) {
        return usage("tree takes no arguments");
    }
    auto repository = open_repository();
    if (!repository) {
        return fail(repository.error());
    }
    auto root = liquid::runtime::block_on(repository->root_digest());
    if (!root) {
        return fail(root.error());
    }
    std::println("{}", liquid::to_string(*root));
    return 0;
}

auto tree_move(int argc, char** argv) -> int {
    if (argc != 2) {
        return usage("move requires a source and destination");
    }
    auto repository = open_repository();
    if (!repository) {
        return fail(repository.error());
    }
    auto moved = liquid::runtime::block_on(repository->move(argv[0], argv[1]));
    if (!moved) {
        return fail(moved.error());
    }
    std::println("moved {} {}", argv[0], argv[1]);
    return 0;
}

auto tree_remove(int argc, char** argv) -> int {
    if (argc != 1) {
        return usage("rm requires one path");
    }
    auto repository = open_repository();
    if (!repository) {
        return fail(repository.error());
    }
    auto removed = liquid::runtime::block_on(repository->remove(argv[0]));
    if (!removed) {
        return fail(removed.error());
    }
    std::println("removed {}", argv[0]);
    return 0;
}

} // namespace liquid_cli
