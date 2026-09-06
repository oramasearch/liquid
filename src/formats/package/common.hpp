#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "formats/tar/writer.hpp"
#include "liquid/error.hpp"
#include "liquid/hash.hpp"

namespace liquid::formats::package {

struct file {
    std::string path;
    std::vector<std::byte> bytes;
    std::uint32_t mode{0644};
};

[[nodiscard]] auto text_bytes(std::string_view text) -> std::vector<std::byte>;
[[nodiscard]] auto sha256(std::span<const std::byte> input) -> result<digest>;
[[nodiscard]] auto hex_bytes(std::span<const std::byte> input) -> std::string;
[[nodiscard]] auto md5_hex(std::span<const std::byte> input) -> std::string;
[[nodiscard]] auto zstd(std::span<const std::byte> input) -> result<std::vector<std::byte>>;
[[nodiscard]] auto tar(std::span<const file> files,
                       std::uint64_t source_date_epoch,
                       std::string_view prefix = {}) -> result<std::vector<std::byte>>;
[[nodiscard]] auto cpio(std::span<const file> files, std::uint64_t source_date_epoch)
    -> result<std::vector<std::byte>>;
[[nodiscard]] auto join(std::span<const std::string> values, std::string_view separator)
    -> std::string;
[[nodiscard]] auto attribute(std::span<const std::byte> attributes, std::string_view name)
    -> result<std::string>;

} // namespace liquid::formats::package
