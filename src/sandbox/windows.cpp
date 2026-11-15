// SPDX-License-Identifier: MIT

#include "sandbox/sandbox.hpp"

#if defined(_WIN32)

namespace liquid::sandbox_detail {

auto launch(const launch_request&) -> result<launch_result> {
    return std::unexpected(
        error{errc::unsupported,
              "sandboxed actions are unavailable on Windows until Cloud Files and job objects",
              "liquid Phase 6"});
}

auto launch_wrap(const wrap_launch_request&) -> result<launch_result> {
    return std::unexpected(
        error{errc::unsupported,
              "liquid wrap is unavailable on Windows until Cloud Files and job objects",
              "liquid Phase 6"});
}

} // namespace liquid::sandbox_detail

#endif
