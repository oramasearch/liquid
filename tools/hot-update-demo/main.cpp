// SPDX-License-Identifier: MIT

#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "liquid/hot_abi.h"
#include "liquid/runtime.hpp"

namespace {

using value_function = auto (*)() -> int;

auto original_value() -> int {
    return 1;
}

value_function current_value = original_value;

auto run(const char* endpoint, int iterations) -> liquid::runtime::task<void> {
    if (liquid_hot_register("demo_value", reinterpret_cast<void**>(&current_value)) !=
            LIQUID_HOT_OK ||
        liquid_hot_listen(endpoint) != LIQUID_HOT_OK) {
        co_return std::unexpected(
            liquid::error{liquid::errc::internal, liquid_hot_last_error(), "liquid-hot demo"});
    }
    for (auto iteration = 0; iteration < iterations; ++iteration) {
        if (liquid_hot_quiescent() != LIQUID_HOT_OK) {
            co_return std::unexpected(
                liquid::error{liquid::errc::internal, liquid_hot_last_error(), "liquid-hot demo"});
        }
        const auto value = current_value();
        std::printf("value=%d\n", value);
        std::fflush(stdout);
        if (value != 1) {
            break;
        }
        auto [wait_error] = co_await liquid::runtime::delay(std::chrono::milliseconds{100});
        if (wait_error) {
            co_return std::unexpected(
                liquid::error{liquid::errc::internal, wait_error.message(), "liquid-hot demo"});
        }
    }
    liquid_hot_close();
    co_return {};
}

} // namespace

auto main(int argc, char** argv) -> int {
    if (argc != 3) {
        std::fprintf(stderr, "usage: liquid-hot-demo <endpoint> <iterations>\n");
        return 2;
    }
    const auto iterations = std::atoi(argv[2]);
    auto result = liquid::runtime::block_on(run(argv[1], iterations));
    if (!result) {
        std::fprintf(stderr, "error: %s\n", result.error().message.c_str());
        return 1;
    }
    return 0;
}
