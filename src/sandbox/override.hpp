#pragma once

#include <filesystem>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace liquid::sandbox_detail {

// A binary named by an override variable (LIQUID_WRAP_HOOK, LIQUID_READ_GUARD) is used only when
// it is a regular file owned by the effective user or root that no other user can write, so a
// variable set by someone else cannot slip another user's code into every action (liquid#401).
inline auto trusted_override(const std::filesystem::path& path) -> bool {
#ifdef _WIN32
    return std::filesystem::is_regular_file(path);
#else
    struct stat info{};
    return ::stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
           (info.st_uid == ::geteuid() || info.st_uid == 0) &&
           (info.st_mode & (S_IWGRP | S_IWOTH)) == 0;
#endif
}

} // namespace liquid::sandbox_detail
