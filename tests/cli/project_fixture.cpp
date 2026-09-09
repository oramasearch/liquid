#include <chrono>
#include <filesystem>
#include <print>
#include <stdexcept>
#include <string>

#include "liquid/build.hpp"
#include "liquid/repository.hpp"

namespace {

template <class T> auto require(liquid::result<T> value) -> T {
    if (!value) {
        throw std::runtime_error(value.error().message + " (" + value.error().context + ")");
    }
    return std::move(*value);
}

auto populate(liquid::repository& repository) -> liquid::result_id {
    const auto root = require(liquid::runtime::block_on(repository.root_digest()));
    const auto timestamp = std::chrono::sys_seconds{std::chrono::seconds{1'700'000'000}};
    auto input = liquid::action_input{
        .command = {"make"}, .inputs = root, .toolchain = root, .platform = {"linux", "arm64"}};
    auto value = liquid::action_result{.stdout_output = root,
                                       .stderr_output = root,
                                       .worker = "projection-fixture",
                                       .observed_at = timestamp,
                                       .output_tree = root};
    value.finished_at = timestamp;
    return require(liquid::runtime::block_on(
        liquid::build::record_remote_result(repository, std::move(input), std::move(value))));
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::println(stderr, "usage: project-fixture <repository|build> <path>");
        return 2;
    }
    try {
        const auto path = std::filesystem::path{argv[2]};
        auto repository = liquid::repository{};
        if (std::string_view{argv[1]} == "build") {
            const auto cache = require(liquid::build::wrap_cache_directory(path));
            repository = require(
                liquid::runtime::block_on(liquid::repository::create(cache / "repository")));
            require(liquid::runtime::block_on(repository.import_tree(path)));
        } else {
            repository = require(liquid::runtime::block_on(liquid::repository::open(path)));
        }
        const auto result = populate(repository);
        const auto record =
            require(liquid::runtime::block_on(liquid::build::result(repository, result)));
        const auto root = require(liquid::runtime::block_on(repository.root_digest()));
        const auto tree = require(liquid::runtime::block_on(repository.root()));
        std::println("result={}", liquid::to_string(result.value));
        std::println("tree={}", liquid::to_string(root));
        std::println("action={}", liquid::to_string(record.action));
        for (const auto& entry : tree.entries) {
            if (entry.kind == liquid::node_kind::blob) {
                std::println("blob={}", liquid::to_string(entry.ref));
                break;
            }
        }
    } catch (const std::exception& error) {
        std::println(stderr, "{}", error.what());
        return 1;
    }
    return 0;
}
