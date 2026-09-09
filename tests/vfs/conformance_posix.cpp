#include "conformance_operations.hpp"

#if !defined(_WIN32)
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace liquid::test::vfs_conformance {
namespace {

class posix_driver final : public filesystem_operations {
public:
    auto getattr(const std::filesystem::path& path, struct stat* output) -> int override {
        return ::stat(path.c_str(), output);
    }

    auto readdir(const std::filesystem::path& path) -> directory_result override {
        auto result = directory_result{};
        auto* directory = ::opendir(path.c_str());
        if (!directory) {
            return result;
        }
        result.opened = true;
        errno = 0;
        while (const auto* entry = ::readdir(directory)) {
            result.entries.emplace_back(entry->d_name);
        }
        result.read_error = errno;
        result.close_error = ::closedir(directory);
        return result;
    }

    auto open(const std::filesystem::path& path, int flags, mode_t mode) -> int override {
        return ::open(path.c_str(), flags, mode);
    }

    auto pread(int file, void* into, std::size_t size, off_t offset) -> ssize_t override {
        return ::pread(file, into, size, offset);
    }

    auto pwrite(int file, const void* from, std::size_t size, off_t offset) -> ssize_t override {
        return ::pwrite(file, from, size, offset);
    }

    auto release(int file) -> int override {
        return ::close(file);
    }

    auto truncate(const std::filesystem::path& path, off_t size) -> int override {
        return ::truncate(path.c_str(), size);
    }

    auto chmod(const std::filesystem::path& path, mode_t mode) -> int override {
        return ::chmod(path.c_str(), mode);
    }

    auto rename(const std::filesystem::path& from, const std::filesystem::path& to)
        -> int override {
        return ::rename(from.c_str(), to.c_str());
    }

    auto unlink(const std::filesystem::path& path) -> int override {
        return ::unlink(path.c_str());
    }

    auto
    get_attribute(const std::filesystem::path& path, const char* name, char* into, std::size_t size)
        -> ssize_t override {
#if defined(__APPLE__)
        return ::getxattr(path.c_str(), name, into, size, 0, 0);
#else
        return ::getxattr(path.c_str(), name, into, size);
#endif
    }

    auto list_attributes(const std::filesystem::path& path, char* into, std::size_t size)
        -> ssize_t override {
#if defined(__APPLE__)
        return ::listxattr(path.c_str(), into, size, 0);
#else
        return ::listxattr(path.c_str(), into, size);
#endif
    }

    auto set_attribute(const std::filesystem::path& path, const char* name, std::string_view value)
        -> int override {
#if defined(__APPLE__)
        return ::setxattr(path.c_str(), name, value.data(), value.size(), 0, 0);
#else
        return ::setxattr(path.c_str(), name, value.data(), value.size(), 0);
#endif
    }

    auto remove_attribute(const std::filesystem::path& path, const char* name) -> int override {
#if defined(__APPLE__)
        return ::removexattr(path.c_str(), name, 0);
#else
        return ::removexattr(path.c_str(), name);
#endif
    }
};

} // namespace

auto posix_operations() -> std::shared_ptr<filesystem_operations> {
    return std::make_shared<posix_driver>();
}

} // namespace liquid::test::vfs_conformance
#endif
