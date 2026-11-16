// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>

#if defined(__linux__)
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace liquid::test {

// Why a FUSE mount cannot be attempted on this host at run time, or nothing when it can.
//
// LIQUID_BUILD_HAS_FUSE says libfuse was found when the tests were built. A Linux container
// can still lack /dev/fuse, or deny opening it, so a test that mounts checks this before it
// starts and skips with the reason instead of failing (liquid#362). Where /dev/fuse opens,
// nothing is skipped and the test runs in full.
inline auto fuse_unavailable_reason() -> std::optional<std::string> {
#if defined(__linux__)
    const auto descriptor = ::open("/dev/fuse", O_RDWR | O_CLOEXEC);
    if (descriptor < 0) {
        const auto code = errno;
        return "FUSE is unavailable at run time: cannot open /dev/fuse: " +
               std::string{std::strerror(code)};
    }
    ::close(descriptor);
#endif
    return std::nullopt;
}

} // namespace liquid::test

// Skips the current gtest, with the reason, when a FUSE mount cannot be attempted.
#define LIQUID_TEST_REQUIRE_FUSE()                                                                 \
    do {                                                                                           \
        if (const auto liquid_fuse_reason = ::liquid::test::fuse_unavailable_reason()) {           \
            GTEST_SKIP() << *liquid_fuse_reason;                                                   \
        }                                                                                          \
    } while (false)
