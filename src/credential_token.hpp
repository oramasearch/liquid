// SPDX-License-Identifier: MIT

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <stdlib.h>
#else
#include <unistd.h>

extern char** environ;
#endif

// The Lake bearer this process uses (L-30, liquid#410). It comes from LIQUID_TOKEN_FILE, a file only
// its owner may read, or else from LIQUID_TOKEN. A long-lived process (the daemon) wipes the variable
// from its own memory and environment once read (scrub_environment_token), so /proc/<pid>/environ
// and the processes it starts no longer carry it. Sandboxed actions already get an explicit
// environment, never this one.
namespace liquid::token_detail {

inline auto read_token_file(const std::filesystem::path& path) -> std::optional<std::string> {
    auto error = std::error_code{};
    const auto status = std::filesystem::status(path, error);
    if (error || !std::filesystem::is_regular_file(status)) {
        return std::nullopt;
    }
#if !defined(_WIN32)
    // On Windows the standard permission bits do not describe who may read the file (its ACL does),
    // so this check applies to POSIX systems only.
    using std::filesystem::perms;
    const auto shared = perms::group_all | perms::others_all;
    if ((status.permissions() & shared) != perms::none) {
        // Refused, as an SSH key would be, and said so: an empty token would otherwise fail later
        // with only an authentication error.
        std::fprintf(stderr,
                     "liquid: LIQUID_TOKEN_FILE %s is readable by its group or others; refusing "
                     "it (chmod 600)\n",
                     path.string().c_str());
        return std::nullopt;
    }
#endif
    auto input = std::ifstream{path, std::ios::binary};
    auto token = std::string{std::istreambuf_iterator<char>{input}, {}};
    while (!token.empty() && (token.back() == '\n' || token.back() == '\r' || token.back() == ' ')) {
        token.pop_back();
    }
    return token;
}

inline auto lake_token() -> std::string {
    static const auto token = [] {
        if (const auto* file = std::getenv("LIQUID_TOKEN_FILE"); file != nullptr && file[0] != '\0') {
            return read_token_file(file).value_or(std::string{});
        }
        const auto* value = std::getenv("LIQUID_TOKEN");
        return value != nullptr ? std::string{value} : std::string{};
    }();
    return token;
}

inline void scrub_environment_token() {
    static_cast<void>(lake_token());  // read it before it is gone
#if defined(_WIN32)
    static_cast<void>(::_putenv_s("LIQUID_TOKEN", ""));
#else
    constexpr auto prefix = std::string_view{"LIQUID_TOKEN="};
    for (auto** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        if (std::string_view{*entry}.starts_with(prefix)) {
            std::memset(*entry + prefix.size(), 0, std::strlen(*entry + prefix.size()));
        }
    }
    ::unsetenv("LIQUID_TOKEN");
#endif
}

} // namespace liquid::token_detail
