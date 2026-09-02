#include <cstring>
#include <print>
#include <span>
#include <string_view>

#include "liquid/liquid.hpp"

namespace {

int usage() {
    std::println("liquid — a version control system, build system, package manager,");
    std::println("artifact service and runtime in one program.");
    std::println("");
    std::println("usage: liquid <command>");
    std::println("");
    std::println("commands:");
    std::println("  version    print the version");
    std::println("  hello      say hello (proves the toolchain end to end)");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    const std::span<char*> args(argv, static_cast<std::size_t>(argc));
    if (args.size() < 2) return usage();

    const std::string_view cmd = args[1];
    if (cmd == "version" || cmd == "--version") {
        std::println("liquid {}", liquid::version_string());
        return 0;
    }
    if (cmd == "hello") {
        std::println("hello from liquid {} — drip, drip, drip", liquid::version_string());
        return 0;
    }
    return usage();
}
