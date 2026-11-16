// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/hash.hpp"
#include "reapi/messages.hpp"

// The REAPI ActionCache's files (liquid #428, LSR-034).
//
// An action digest arrives from an unauthenticated peer. Before it names a file it must be the
// lowercase hexadecimal encoding of a digest of the request's algorithm, of exactly that
// algorithm's length; anything else is refused as `errc::invalid_argument` (gRPC
// INVALID_ARGUMENT) before the filesystem is touched. Entries are then opened directly inside the
// cache root with no symbolic link followed, at the root or at the entry, and written by creating
// a new file and renaming it over the entry, so a link planted under an entry's name is replaced,
// never written through.
namespace liquid::reapi {

// The cache entry name for `action`: its hash, validated as described above.
[[nodiscard]] auto action_cache_name(const Digest& action, algorithm hash_algorithm)
    -> result<std::string>;

// `errc::not_found` when no entry exists.
[[nodiscard]] auto read_action_cache(const std::filesystem::path& root, std::string_view name)
    -> result<std::vector<std::byte>>;

[[nodiscard]] auto write_action_cache(const std::filesystem::path& root,
                                      std::string_view name,
                                      std::span<const std::byte> contents) -> result<void>;

} // namespace liquid::reapi
