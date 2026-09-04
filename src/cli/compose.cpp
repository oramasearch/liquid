#include <filesystem>
#include <optional>
#include <print>
#include <string_view>
#include <utility>

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

auto open_repository(const std::filesystem::path& path) -> liquid::result<liquid::repository> {
    return liquid::runtime::block_on(liquid::repository::open(path));
}

} // namespace

auto compose_nest(int argc, char** argv) -> int {
    if (argc != 2) {
        return usage("nest requires a repository and destination path");
    }
    auto destination = open_repository(std::filesystem::current_path());
    if (!destination) {
        return fail(destination.error());
    }
    auto source = open_repository(argv[0]);
    if (!source) {
        return fail(source.error());
    }
    auto nested = liquid::runtime::block_on(destination->nest(std::move(*source), argv[1]));
    if (!nested) {
        return fail(nested.error());
    }
    std::println("nested {} at {}", argv[0], argv[1]);
    return 0;
}

auto compose_split(int argc, char** argv) -> int {
    if (argc != 2) {
        return usage("split requires a path and destination repository");
    }
    auto repository = open_repository(std::filesystem::current_path());
    if (!repository) {
        return fail(repository.error());
    }
    auto split = liquid::runtime::block_on(repository->split(argv[0], argv[1]));
    if (!split) {
        return fail(split.error());
    }
    std::println("split {} into {}", argv[0], argv[1]);
    return 0;
}

auto compose_combine(int argc, char** argv) -> int {
    if (argc != 1 && argc != 3) {
        return usage("combine requires a repository and optional --at path");
    }
    if (argc == 3 && std::string_view{argv[1]} != "--at") {
        return usage("combine accepts only --at path");
    }
    auto destination = open_repository(std::filesystem::current_path());
    if (!destination) {
        return fail(destination.error());
    }
    auto source = open_repository(argv[0]);
    if (!source) {
        return fail(source.error());
    }
    auto at = argc == 3 ? std::optional<std::string>{argv[2]} : std::nullopt;
    auto combined =
        liquid::runtime::block_on(destination->combine(std::move(*source), std::move(at)));
    if (!combined) {
        return fail(combined.error());
    }
    std::println("combined {}", argv[0]);
    return 0;
}

} // namespace liquid_cli
