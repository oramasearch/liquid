#include "fs/contained.hpp"

#include <cerrno>
#include <cstring>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <fstream>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace liquid::fs_detail {
namespace {

auto failure(errc code, std::string message, const std::filesystem::path& path) -> error {
    return {code, std::move(message), path.string()};
}

auto through_symlink(const std::filesystem::path& path) -> error {
    return failure(errc::corrupt, "refusing to follow a symbolic link below the root", path);
}

auto collision(const std::filesystem::path& path) -> error {
    return failure(errc::already_exists,
                   "entry already exists; names that differ only in case or Unicode "
                   "normalisation collide on this filesystem",
                   path);
}

auto existing_kind(const std::filesystem::path& path) -> result<std::filesystem::file_type> {
    auto code = std::error_code{};
    const auto status = std::filesystem::symlink_status(path, code);
    if (code && code != std::errc::no_such_file_or_directory) {
        return std::unexpected(failure(errc::io, code.message(), path));
    }
    return status.type();
}

#if defined(_WIN32)
auto write_file(const std::filesystem::path& path,
                std::span<const std::byte> bytes,
                std::uint32_t mode,
                bool exclusive) -> result<void> {
    auto kind = existing_kind(path);
    if (!kind) {
        return std::unexpected(kind.error());
    }
    if (*kind == std::filesystem::file_type::symlink) {
        return std::unexpected(through_symlink(path));
    }
    if (*kind != std::filesystem::file_type::not_found) {
        if (exclusive) {
            return std::unexpected(collision(path));
        }
        if (*kind != std::filesystem::file_type::regular) {
            return std::unexpected(failure(errc::corrupt, "entry is not a regular file", path));
        }
    }
    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
        return std::unexpected(failure(errc::io, "cannot write file", path));
    }
    return set_mode(path, mode);
}
#else
auto write_file(const std::filesystem::path& path,
                std::span<const std::byte> bytes,
                std::uint32_t mode,
                bool exclusive) -> result<void> {
    if (!exclusive) {
        auto kind = existing_kind(path);
        if (!kind) {
            return std::unexpected(kind.error());
        }
        if (*kind == std::filesystem::file_type::symlink) {
            return std::unexpected(through_symlink(path));
        }
        if (*kind != std::filesystem::file_type::not_found &&
            *kind != std::filesystem::file_type::regular) {
            return std::unexpected(failure(errc::corrupt, "entry is not a regular file", path));
        }
    }
    const auto flags = O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC | (exclusive ? O_EXCL : O_TRUNC);
    const auto descriptor = ::open(path.c_str(), flags, 0600);
    if (descriptor < 0) {
        const auto code = errno;
        if (code == EEXIST) {
            return std::unexpected(collision(path));
        }
        if (code == ELOOP) {
            return std::unexpected(through_symlink(path));
        }
        return std::unexpected(failure(errc::io, std::strerror(code), path));
    }
    auto remaining = bytes;
    while (!remaining.empty()) {
        const auto written = ::write(descriptor, remaining.data(), remaining.size());
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            const auto code = errno;
            ::close(descriptor);
            return std::unexpected(failure(errc::io, std::strerror(code), path));
        }
        remaining = remaining.subspan(static_cast<std::size_t>(written));
    }
    if (::fchmod(descriptor, static_cast<mode_t>(mode & 07777U)) != 0) {
        const auto code = errno;
        ::close(descriptor);
        return std::unexpected(failure(errc::io, std::strerror(code), path));
    }
    if (::close(descriptor) != 0) {
        return std::unexpected(failure(errc::io, std::strerror(errno), path));
    }
    return {};
}
#endif

} // namespace

auto make_directories_within(const std::filesystem::path& root,
                             const std::filesystem::path& relative)
    -> result<std::filesystem::path> {
    if (relative.has_root_path()) {
        return std::unexpected(failure(errc::internal, "contained path is not relative", relative));
    }
    auto current = root;
    for (const auto& component : relative) {
        if (component.empty() || component == ".") {
            continue;
        }
        if (component == "..") {
            return std::unexpected(
                failure(errc::corrupt, "contained path climbs above its root", relative));
        }
        current /= component;
        auto kind = existing_kind(current);
        if (!kind) {
            return std::unexpected(kind.error());
        }
        if (*kind == std::filesystem::file_type::not_found) {
            auto made = make_new_directory(current);
            if (!made) {
                return std::unexpected(made.error());
            }
            continue;
        }
        if (*kind == std::filesystem::file_type::symlink) {
            return std::unexpected(through_symlink(current));
        }
        if (*kind != std::filesystem::file_type::directory) {
            return std::unexpected(
                failure(errc::corrupt, "path component is not a directory", current));
        }
    }
    return current;
}

auto refuse_symlink(const std::filesystem::path& path) -> result<void> {
    auto kind = existing_kind(path);
    if (!kind) {
        return std::unexpected(kind.error());
    }
    if (*kind == std::filesystem::file_type::symlink) {
        return std::unexpected(through_symlink(path));
    }
    return {};
}

auto make_new_directory(const std::filesystem::path& path) -> result<void> {
#if defined(_WIN32)
    auto code = std::error_code{};
    if (!std::filesystem::create_directory(path, code)) {
        if (code) {
            return std::unexpected(failure(errc::io, code.message(), path));
        }
        return std::unexpected(collision(path));
    }
#else
    if (::mkdir(path.c_str(), 0777) != 0) {
        const auto code = errno;
        if (code == EEXIST) {
            return std::unexpected(collision(path));
        }
        return std::unexpected(failure(errc::io, std::strerror(code), path));
    }
#endif
    return {};
}

auto write_new_file(const std::filesystem::path& path,
                    std::span<const std::byte> bytes,
                    std::uint32_t mode) -> result<void> {
    return write_file(path, bytes, mode, true);
}

auto replace_file(const std::filesystem::path& path,
                  std::span<const std::byte> bytes,
                  std::uint32_t mode) -> result<void> {
    return write_file(path, bytes, mode, false);
}

auto set_mode(const std::filesystem::path& path, std::uint32_t mode) -> result<void> {
    auto refused = refuse_symlink(path);
    if (!refused) {
        return refused;
    }
    auto code = std::error_code{};
    std::filesystem::permissions(path,
                                 static_cast<std::filesystem::perms>(mode & 07777U),
                                 std::filesystem::perm_options::replace,
                                 code);
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), path));
    }
    return {};
}

auto symlink_target_within(const std::filesystem::path& link, const std::filesystem::path& target)
    -> bool {
    if (target.has_root_path()) {
        return true;
    }
    auto depth = std::ptrdiff_t{};
    for (const auto& component : link.parent_path()) {
        if (component == "..") {
            return false;
        }
        if (!component.empty() && component != ".") {
            ++depth;
        }
    }
    for (const auto& component : target) {
        if (component == "..") {
            if (--depth < 0) {
                return false;
            }
        } else if (!component.empty() && component != ".") {
            ++depth;
        }
    }
    return true;
}

} // namespace liquid::fs_detail
