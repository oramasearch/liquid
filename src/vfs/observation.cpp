// SPDX-License-Identifier: MIT

#include "backend.hpp"

#if !defined(LIQUID_HAS_FUSE3) && !defined(LIQUID_HAS_FUSE2)
namespace liquid::vfs_detail {

runtime::task<std::shared_ptr<backend>> start_observed_fuse(
    repository&, digest, std::filesystem::path, std::vector<std::string>, observation_sink, bool) {
#if defined(_WIN32)
    co_return std::unexpected(
        error{errc::unsupported,
              "sandboxed actions are unavailable on Windows until Cloud Files and job objects",
              "liquid Phase 6"});
#elif defined(__APPLE__)
    co_return std::unexpected(
        error{errc::unsupported,
              "sandboxed actions require macFUSE on macOS; install macFUSE and rebuild liquid",
              "liquid #10"});
#else
    co_return std::unexpected(error{errc::unsupported,
                                    "sandboxed actions require libfuse3",
                                    "install libfuse3-dev and rebuild liquid"});
#endif
}

} // namespace liquid::vfs_detail
#endif
