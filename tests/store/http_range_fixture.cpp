#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <vector>

#include "liquid/store.hpp"

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::vector<std::byte> input(512 * 1024);
    std::uint64_t state = 0x987654321ULL;
    for (auto& byte : input) {
        state = state * 6364136223846793005ULL + 1;
        byte = std::byte{static_cast<unsigned char>(state >> 32U)};
    }
    liquid::store_options options;
    options.index_cache_size = 8 * 1024 * 1024;
    auto store = liquid::store::open(std::filesystem::path(argv[1]), options);
    if (!store) {
        std::cerr << store.error().message << '\n';
        return 1;
    }
    auto id = store->put(input);
    if (!id) {
        std::cerr << id.error().message << '\n';
        return 1;
    }
    return 0;
}
