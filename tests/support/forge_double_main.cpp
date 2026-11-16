// SPDX-License-Identifier: MIT

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <print>
#include <string>
#include <string_view>
#include <thread>

#include "forge_double.hpp"

namespace {

std::atomic_bool stopping{};

extern "C" void stop(int) {
    stopping.store(true);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3 || std::string_view{argv[1]} != "--listen") {
        std::println(stderr, "error: usage: liquid-forge-double --listen <address>:<port>");
        return 2;
    }
    try {
        auto server = liquid::test::forge_double{{}, argv[2]};
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        std::println("listening={}", server.url());
        std::fflush(stdout);
        while (!stopping.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
        return 0;
    } catch (const std::exception& exception) {
        std::println(stderr, "error: {}", exception.what());
        return 1;
    }
}
