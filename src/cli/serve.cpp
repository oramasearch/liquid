#include <filesystem>
#include <print>
#include <string_view>

#include "liquid/error.hpp"
#include "liquid/reapi.hpp"
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

} // namespace

auto serve(int argc, char** argv) -> int {
    if (argc < 1 || std::string_view{argv[0]} != "reapi") {
        return usage("serve reapi --listen <address> [--instance <name>]");
    }
    auto options = liquid::reapi_serve_options{.store_directory = std::filesystem::current_path() /
                                                                  ".liquid-store"};
    for (auto index = 1; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--listen") {
            if (++index == argc) {
                return usage("serve reapi --listen requires an address");
            }
            options.listen = argv[index];
            continue;
        }
        if (argument == "--instance") {
            if (++index == argc) {
                return usage("serve reapi --instance requires a name");
            }
            options.instance_name = argv[index];
            continue;
        }
        return usage("serve reapi --listen <address> [--instance <name>]");
    }
    if (!std::filesystem::exists(options.store_directory)) {
        return usage("serve reapi must run from a liquid repository");
    }
    std::println("REAPI listening on {}", options.listen);
    auto served = liquid::runtime::block_on(liquid::serve_reapi(std::move(options)));
    return served ? 0 : fail(served.error());
}

} // namespace liquid_cli
