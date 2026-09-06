#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "liquid/export.hpp"
#include "liquid/format.hpp"

namespace liquid {

struct LIQUID_EXPORT language_support {
    std::string language;
    std::vector<std::string> extensions;
    std::uint8_t tier{};
    std::string mechanism;
    std::string notes;
};

struct LIQUID_EXPORT format_support {
    std::string format;
    std::uint8_t tier{3};
    std::string codec;
    format_limits limits;
};

struct LIQUID_EXPORT platform_support {
    std::string os;
    std::string tier_1_status;
    std::string sandbox;
    std::string vfs;
    std::string notes;
};

struct LIQUID_EXPORT matrix {
    std::vector<language_support> languages;
    std::vector<format_support> formats;
    std::vector<platform_support> platforms;
};

[[nodiscard]] LIQUID_EXPORT auto support_matrix() -> matrix;

} // namespace liquid
