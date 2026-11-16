// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace liquid::store_detail {

struct chunk_range {
    std::size_t offset{};
    std::size_t size{};
};

// The constants are part of the on-disk compatibility contract.
inline constexpr std::size_t chunk_minimum = 8U * 1024U;
inline constexpr std::size_t chunk_target = 64U * 1024U;
inline constexpr std::size_t chunk_maximum = 128U * 1024U;

class chunker {
public:
    chunker();

    [[nodiscard]] std::vector<std::vector<std::byte>> feed(std::span<const std::byte> input);
    [[nodiscard]] std::vector<std::vector<std::byte>> finish();

private:
    [[nodiscard]] bool push(std::byte value);

    std::vector<std::byte> current_;
    std::uint64_t fingerprint_{};
    bool emitted_{};
    bool finished_{};
};

std::vector<chunk_range> fastcdc(std::span<const std::byte> input);
[[nodiscard]] std::uint64_t resemblance_hash(std::span<const std::byte> input) noexcept;

} // namespace liquid::store_detail
