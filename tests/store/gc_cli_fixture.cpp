#include <chrono>
#include <cstddef>
#include <filesystem>
#include <vector>

#include "liquid/runtime.hpp"
#include "liquid/store.hpp"

int main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    const std::filesystem::path path = argv[1];
    std::filesystem::remove_all(path);
    auto store = liquid::runtime::block_on(liquid::store::open(path));
    if (!store)
        return 1;
    std::vector<std::byte> bytes(4096, std::byte{42});
    auto id = liquid::runtime::block_on(store->put(std::move(bytes)));
    if (!id)
        return 1;
    auto lease = liquid::runtime::block_on(store->lease_root(*id, std::chrono::hours{1}));
    return lease ? 0 : 1;
}
