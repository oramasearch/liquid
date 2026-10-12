#pragma once

#if !defined(_WIN32)
#include <sys/stat.h>
#include <sys/types.h>

#include "vfs/backend.hpp"

namespace liquid::test::vfs_conformance {

struct directory_result {
    bool opened{};
    std::vector<std::string> entries;
    int read_error{};
    int close_error{};
};

// Test-only operation interface. Kernel drivers execute real syscalls; the direct
// calibration driver invokes the core. Every driver uses the same assertions.
class filesystem_operations {
public:
    virtual ~filesystem_operations() = default;
    virtual auto getattr(const std::filesystem::path&, struct stat*) -> int = 0;
    virtual auto readdir(const std::filesystem::path&) -> directory_result = 0;
    virtual auto open(const std::filesystem::path&, int flags, mode_t mode = 0) -> int = 0;
    virtual auto pread(int, void*, std::size_t, off_t) -> ssize_t = 0;
    virtual auto pwrite(int, const void*, std::size_t, off_t) -> ssize_t = 0;
    virtual auto release(int) -> int = 0;
    virtual auto truncate(const std::filesystem::path&, off_t) -> int = 0;
    virtual auto chmod(const std::filesystem::path&, mode_t) -> int = 0;
    virtual auto rename(const std::filesystem::path&, const std::filesystem::path&) -> int = 0;
    virtual auto unlink(const std::filesystem::path&) -> int = 0;
    virtual auto get_attribute(const std::filesystem::path&, const char*, char*, std::size_t)
        -> ssize_t = 0;
    virtual auto list_attributes(const std::filesystem::path&, char*, std::size_t) -> ssize_t = 0;
    virtual auto set_attribute(const std::filesystem::path&, const char*, std::string_view)
        -> int = 0;
    virtual auto remove_attribute(const std::filesystem::path&, const char*) -> int = 0;
};

auto posix_operations() -> std::shared_ptr<filesystem_operations>;

} // namespace liquid::test::vfs_conformance
#endif
