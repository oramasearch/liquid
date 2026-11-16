// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "liquid/error.hpp"

namespace liquid::build_detail {

enum class output_kind : std::uint8_t {
    directory_tree,
    ar_archive,
    elf_object,
    macho_object,
    pe_object,
    text,
    python_bytecode,
    opaque,
};

[[nodiscard]] auto output_kind_for(const std::filesystem::path& path,
                                   std::span<const std::byte> contents) -> output_kind;
[[nodiscard]] auto normalise_output(output_kind kind, std::vector<std::byte> contents)
    -> result<std::vector<std::byte>>;
[[nodiscard]] auto normalise_file(const std::filesystem::path& path) -> result<bool>;
[[nodiscard]] auto normalised_output_mode(std::uint32_t mode) -> std::uint32_t;
[[nodiscard]] auto output_normaliser_rules() -> std::vector<std::string>;

} // namespace liquid::build_detail
