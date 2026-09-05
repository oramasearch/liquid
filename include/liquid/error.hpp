#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "liquid/export.hpp"

namespace liquid {

enum class errc : std::uint16_t {
    ok = 0,
    not_found,
    already_exists,
    conflict,
    invalid_argument,
    unsupported,
    corrupt,
    io,
    locked,
    unauthorized,
    unavailable,
    busy,
    internal,
    not_remote,
};

struct LIQUID_EXPORT error {
    errc code;
    std::string message;
    std::string context;
};

template <class T> using result = std::expected<T, error>;

[[nodiscard]] LIQUID_EXPORT std::string_view to_string(errc code) noexcept;

} // namespace liquid
