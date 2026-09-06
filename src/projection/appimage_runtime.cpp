#include "projection/appimage_runtime.hpp"

#include <cstdint>

#if defined(LIQUID_HAS_APPIMAGE_RUNTIME)
extern "C" const std::uint8_t liquid_appimage_runtime_x86_64_start[];
extern "C" const std::uint8_t liquid_appimage_runtime_x86_64_end[];
extern "C" const std::uint8_t liquid_appimage_runtime_aarch64_start[];
extern "C" const std::uint8_t liquid_appimage_runtime_aarch64_end[];
#endif

namespace liquid::projection_detail {
namespace {

#if defined(LIQUID_HAS_APPIMAGE_RUNTIME)
auto bytes(const std::uint8_t* begin, const std::uint8_t* end) -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(begin), static_cast<std::size_t>(end - begin)};
}
#endif

} // namespace

result<std::span<const std::byte>> appimage_runtime(std::string_view architecture) {
#if defined(LIQUID_HAS_APPIMAGE_RUNTIME)
    if (architecture == "x86_64" || architecture == "amd64") {
        return bytes(liquid_appimage_runtime_x86_64_start, liquid_appimage_runtime_x86_64_end);
    }
    if (architecture == "aarch64" || architecture == "arm64") {
        return bytes(liquid_appimage_runtime_aarch64_start, liquid_appimage_runtime_aarch64_end);
    }
#endif
    return std::unexpected(error{errc::unsupported,
                                 "AppImage runtime is unavailable for this architecture",
                                 std::string{architecture}});
}

} // namespace liquid::projection_detail
