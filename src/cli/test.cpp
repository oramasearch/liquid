// SPDX-License-Identifier: MIT

#include <filesystem>
#include <print>
#include <string>
#include <string_view>

#include "liquid/build.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace liquid_cli {
namespace {

auto fail(const liquid::error& value) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(value.code), value.message, value.context);
    return 1;
}

auto run_tests(std::string filter) -> liquid::runtime::task<int> {
    auto repository = co_await liquid::repository::open_build(std::filesystem::current_path());
    if (!repository) {
        co_return std::unexpected(repository.error());
    }
    auto handle = repository->run_tests(std::move(filter));
    if (!handle) {
        co_return std::unexpected(handle.error());
    }
    auto subscription = handle->events();
    if (!subscription) {
        co_return std::unexpected(subscription.error());
    }
    auto exit = 0;
    while (true) {
        auto event = co_await subscription->next();
        if (!event) {
            co_return std::unexpected(event.error());
        }
        if (!*event) {
            break;
        }
        if ((*event)->test) {
            const auto& result = *(*event)->test;
            std::println("{} {} duration_ms={} hit={}",
                         result.name,
                         result.status == liquid::test_status::passed ? "passed" : "failed",
                         result.duration.count(),
                         result.hit);
            if (result.status == liquid::test_status::failed && !result.output.empty()) {
                std::println("{}", result.output);
            }
        }
        if ((*event)->summary) {
            exit = (*event)->summary->exit;
        }
    }
    co_return exit;
}

} // namespace

auto test(int argc, char** argv) -> int {
    if (argc > 1) {
        std::println(stderr, "error: usage: test [filter]");
        return 2;
    }
    auto result = liquid::runtime::block_on(run_tests(argc == 0 ? "" : argv[0]));
    return result ? *result : fail(result.error());
}

} // namespace liquid_cli
