#include <cstddef>
#include <filesystem>
#include <fstream>
#include <vector>

#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

int main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    const std::filesystem::path path = argv[1];
    std::filesystem::remove_all(path);
    auto repository = liquid::runtime::block_on(liquid::repository::create(path));
    if (!repository)
        return 1;
    std::vector<std::byte> bytes(4096, std::byte{42});
    auto written = liquid::runtime::block_on(repository->write("retained.bin", std::move(bytes)));
    if (!written)
        return 1;
    auto cut = liquid::runtime::block_on(repository->cut("fixture"));
    if (!cut)
        return 1;
    std::ofstream digest_file(path / "RETAINED_DIGEST", std::ios::binary | std::ios::trunc);
    if (!digest_file.is_open())
        return 1;
    digest_file << liquid::to_string(cut->value) << '\n';
    return digest_file ? 0 : 1;
}
