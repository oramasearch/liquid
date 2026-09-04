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

auto locks_lock(int argc, char** argv) -> int {
    if (argc != 1) {
        return usage("lock requires one path");
    }
    auto repository = open_repository();
    if (!repository) {
        return fail(repository.error());
    }
    auto locked = liquid::runtime::block_on(repository->lock(argv[0]));
    if (!locked) {
        return fail(locked.error());
    }
    std::println("locked {}", argv[0]);
    return 0;
}

auto locks_unlock(int argc, char** argv) -> int {
    if (argc != 1) {
        return usage("unlock requires one path");
    }
    auto repository = open_repository();
    if (!repository) {
        return fail(repository.error());
    }
    auto unlocked = liquid::runtime::block_on(repository->unlock(argv[0]));
    if (!unlocked) {
        return fail(unlocked.error());
    }
    std::println("unlocked {}", argv[0]);
    return 0;
}

} // namespace liquid_cli
