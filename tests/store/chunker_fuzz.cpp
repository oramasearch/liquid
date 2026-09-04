#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "store/chunker.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto validate = [](std::span<const std::byte> input) {
        const auto first = liquid::store_detail::chunk(input);
        const auto second = liquid::store_detail::chunk(input);
        if (first.size() != second.size()) __builtin_trap();
        std::size_t offset = 0;
        for (std::size_t index = 0; index < first.size(); ++index) {
            if (first[index].offset != offset || first[index].length == 0 ||
                first[index].length > liquid::store_detail::chunk_max_size ||
                first[index].offset != second[index].offset ||
                first[index].length != second[index].length) {
                __builtin_trap();
            }
            offset += first[index].length;
        }
        if (offset != input.size()) __builtin_trap();
    };

    const auto input = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(data), size);
    validate(input);
    if (size != 0 && size < liquid::store_detail::chunk_max_size * 2) {
        std::vector<std::byte> expanded(liquid::store_detail::chunk_max_size * 2 + size);
        for (std::size_t index = 0; index < expanded.size(); ++index) {
            expanded[index] = input[index % size] ^ std::byte{static_cast<unsigned char>(index >> 8U)};
        }
        validate(expanded);
    }
    return 0;
}
