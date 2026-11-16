// SPDX-License-Identifier: MIT

#pragma once

#include <cerrno>
#include <filesystem>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

#if defined(__APPLE__) || defined(__linux__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "liquid/error.hpp"

namespace liquid::drive_detail {

// Pins a regular file reached without following any link below the selected folder. Store
// streaming (including its write-through reread) uses this descriptor, never the walked name.
class source_file {
public:
    source_file() = default;
    source_file(const source_file&) = delete;
    auto operator=(const source_file&) -> source_file& = delete;
    source_file(source_file&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}
    auto operator=(source_file&& other) noexcept -> source_file& {
        if (this != &other) {
            close();
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }
    ~source_file() {
        close();
    }

    [[nodiscard]] static auto open(const std::filesystem::path& folder,
                                   const std::filesystem::path& relative) -> result<source_file> {
        const auto context = relative.string();
        if (relative.empty() || relative.is_absolute() || context.find('\0') != std::string::npos) {
            return std::unexpected(error{errc::invalid_argument, "invalid drive source", context});
        }
        for (const auto& component : relative) {
            if (component.empty() || component == "." || component == "..") {
                return std::unexpected(
                    error{errc::invalid_argument, "invalid drive source component", context});
            }
        }
#if defined(__APPLE__) || defined(__linux__)
        auto directory = source_file{};
        directory.descriptor_ =
            ::open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (directory.descriptor_ < 0) {
            return std::unexpected(error{errc::io, std::system_category().message(errno), context});
        }
        for (auto part = relative.begin(); part != relative.end(); ++part) {
            const auto last = std::next(part) == relative.end();
            const auto flags =
                O_RDONLY | O_NOFOLLOW | O_CLOEXEC | (last ? O_NONBLOCK : O_DIRECTORY);
            const auto next = ::openat(directory.descriptor_, part->c_str(), flags);
            if (next < 0) {
                return std::unexpected(
                    error{errc::io, std::system_category().message(errno), context});
            }
            ::close(std::exchange(directory.descriptor_, next));
        }
        struct stat status{};
        if (::fstat(directory.descriptor_, &status) != 0) {
            return std::unexpected(error{errc::io, std::system_category().message(errno), context});
        }
        if (!S_ISREG(status.st_mode)) {
            return std::unexpected(error{errc::io, "drive source is not a regular file", context});
        }
        return directory;
#else
        return std::unexpected(
            error{errc::unsupported, "safe drive source reads are unavailable", context});
#endif
    }

    [[nodiscard]] auto path() const -> std::filesystem::path {
        return std::filesystem::path{"/dev/fd"} / std::to_string(descriptor_);
    }

private:
    void close() noexcept {
#if defined(__APPLE__) || defined(__linux__)
        if (descriptor_ >= 0) {
            ::close(std::exchange(descriptor_, -1));
        }
#endif
    }

    int descriptor_{-1};
};

} // namespace liquid::drive_detail
