#include "liquid/tiers.hpp"

auto tiers_header() -> int {
    return static_cast<int>(liquid::support_matrix().platforms.size());
}
