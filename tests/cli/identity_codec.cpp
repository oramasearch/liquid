// SPDX-License-Identifier: MIT

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "liquid/identity.hpp"

namespace {

auto read_bytes(const std::filesystem::path& path) -> std::vector<std::byte> {
    auto input = std::ifstream{path, std::ios::binary};
    auto output = std::vector<std::byte>{};
    for (char character; input.get(character);) {
        output.push_back(std::byte{static_cast<unsigned char>(character)});
    }
    return output;
}

} // namespace

auto main(int argc, char** argv) -> int {
    if (argc < 2) {
        std::cerr << "identity document path required\n";
        return 2;
    }
    auto documents = std::vector<liquid::identity_document>{};
    for (auto index = 1; index < argc; ++index) {
        const auto stored = read_bytes(argv[index]);
        const auto decoded = liquid::decode_identity_document(stored);
        if (!decoded) {
            std::cerr << decoded.error().message << '\n';
            return 1;
        }
        const auto encoded = liquid::encode_identity_document(*decoded);
        if (!encoded || *encoded != stored) {
            std::cerr << "identity document did not round-trip\n";
            return 1;
        }
        documents.push_back(*decoded);
    }
    if (documents.size() == 1) {
        const auto verified = liquid::verify_document(documents.front());
        return verified && *verified ? 0 : 1;
    }
    const auto verified = liquid::verify_chain(documents);
    return verified ? 0 : 1;
}
