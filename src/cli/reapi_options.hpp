#pragma once

#include <charconv>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <utility>

#include "liquid/error.hpp"
#include "liquid/reapi.hpp"

namespace liquid_cli {

// Both service commands use the same environment policy; invalid values refuse startup.
inline auto reapi_upload_options() -> liquid::result<liquid::reapi_upload_limits> {
    auto limits = liquid::reapi_upload_limits{};
    const auto read = [](const char* name, std::size_t& target) -> liquid::result<void> {
        const auto* raw = std::getenv(name);
        if (raw == nullptr) {
            return {};
        }
        const auto text = std::string_view{raw};
        auto parsed = std::size_t{};
        const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || parsed == 0 ||
            parsed > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return std::unexpected(
                liquid::error{liquid::errc::invalid_argument, "expected a positive integer", name});
        }
        target = parsed;
        return {};
    };
    auto idle_ms = static_cast<std::size_t>(limits.idle_timeout.count());
    for (const auto& [name, target] :
         {std::pair{"LIQUID_REAPI_MAX_UPLOAD_BYTES", &limits.max_upload_bytes},
          {"LIQUID_REAPI_MAX_BUFFERED_BYTES", &limits.max_buffered_bytes},
          {"LIQUID_REAPI_MAX_UPLOADS", &limits.max_uploads},
          {"LIQUID_REAPI_MAX_RESOURCE_BYTES", &limits.max_resource_bytes},
          {"LIQUID_REAPI_UPLOAD_IDLE_MS", &idle_ms}}) {
        auto parsed = read(name, *target);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
    }
    limits.idle_timeout = std::chrono::milliseconds{idle_ms};
    return limits;
}

} // namespace liquid_cli
