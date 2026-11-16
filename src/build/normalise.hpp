// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
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

// What a declared output may name: one regular file, a directory whose regular files are all
// outputs, or either (wrap verification re-collects recorded paths without a declared shape).
enum class output_shape : std::uint8_t {
    file,
    directory,
    either,
};

// One collected output file: its generic path below the output root, the permission bits of the
// node that was read, and the parent-owned copy of the bytes to store.
struct staged_output {
    std::string path;
    std::uint32_t mode{};
    std::filesystem::path staged;
};

// Collects one declared output below `root` without following a link. Each path component is
// opened relative to its parent with O_NOFOLLOW, and a symbolic link or any node other than a
// directory or regular file fails the collection before anything is read through it. Every
// regular file is read from its verified descriptor, normalised when `normalise` is set, and
// written to a new file in `staging`, so storage never reopens a path the action controls and no
// output is rewritten in place. A missing output yields nothing; so does a directory where `shape`
// is `file`. The caller stores and removes the staged copies.
[[nodiscard]] auto stage_output(const std::filesystem::path& root,
                                std::string_view relative,
                                output_shape shape,
                                const std::filesystem::path& staging,
                                bool normalise) -> result<std::vector<staged_output>>;
[[nodiscard]] auto normalised_output_mode(std::uint32_t mode) -> std::uint32_t;
[[nodiscard]] auto output_normaliser_rules() -> std::vector<std::string>;

} // namespace liquid::build_detail
