#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "liquid/runtime.hpp"
#include "liquid/store.hpp"

auto main(int argc, char** argv) -> int {
    if (argc != 2)
        return 2;
    const auto root = std::filesystem::path{argv[1]};
    std::error_code code;
    std::filesystem::remove_all(root, code);
    std::filesystem::create_directories(root, code);
    if (code)
        return 1;
    auto options = liquid::store_options{};
    options.custody.emplace("private", "passphrase:");
    options.write_key = "private";
    auto opened =
        liquid::runtime::block_on(liquid::store::open(root / ".liquid-store", std::move(options)));
    if (!opened)
        return 1;
    auto stored =
        liquid::runtime::block_on(opened->put(std::vector<std::byte>(4096, std::byte{0x51})));
    return stored ? 0 : 1;
}
