#include "liquid/version.hpp"

namespace liquid {

constexpr version current_version() noexcept { return {0, 0, 1}; }

std::string_view version_string() noexcept { return "0.0.1"; }

}  // namespace liquid
