#pragma once

#include <cstdint>
#include <filesystem>
#include <span>

#include "liquid/error.hpp"

// Filesystem writes that stay below a root (liquid #385, #386). Install staging and tree
// materialisation write only through these functions. None of them follows a symbolic link:
// a link met where a directory or file is expected is refused, so content cannot redirect a
// later write or mode change outside its root. Paths are checked component by component with
// `lstat` and the final file is opened with `O_NOFOLLOW` where the platform has it. The roots
// they write into are private directories liquid has just created, so no other process races
// the check and the write.
namespace liquid::fs_detail {

// Creates every missing directory of `relative` below `root`, refusing a component that is a
// symbolic link or not a directory. `relative` must be relative and free of `..`. Returns
// `root / relative`.
[[nodiscard]] auto make_directories_within(const std::filesystem::path& root,
                                           const std::filesystem::path& relative)
    -> result<std::filesystem::path>;

// Fails with `errc::corrupt` when `path` exists and is a symbolic link.
[[nodiscard]] auto refuse_symlink(const std::filesystem::path& path) -> result<void>;

// Creates the directory `path`. An existing entry, which on a case- or normalisation-insensitive
// filesystem includes one whose name differs only in case or Unicode normalisation, fails with
// `errc::already_exists`.
[[nodiscard]] auto make_new_directory(const std::filesystem::path& path) -> result<void>;

// Writes a new regular file. An existing entry fails with `errc::already_exists`, as above.
[[nodiscard]] auto write_new_file(const std::filesystem::path& path,
                                  std::span<const std::byte> bytes,
                                  std::uint32_t mode) -> result<void>;

// Writes `path`, replacing an existing regular file but never writing through a symbolic link.
[[nodiscard]] auto replace_file(const std::filesystem::path& path,
                                std::span<const std::byte> bytes,
                                std::uint32_t mode) -> result<void>;

// Sets the permission bits of `path`, which must not be a symbolic link.
[[nodiscard]] auto set_mode(const std::filesystem::path& path, std::uint32_t mode) -> result<void>;

// Whether a symbolic link at `link` (relative to `root`) whose target is `target` resolves
// inside `root`. An absolute target names a path inside the root, which is how an install
// generation is used; a relative target must not climb above `root`.
[[nodiscard]] auto symlink_target_within(const std::filesystem::path& link,
                                         const std::filesystem::path& target) -> bool;

} // namespace liquid::fs_detail
