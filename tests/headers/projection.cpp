#include "liquid/projection.hpp"

auto projection_header_isolated() -> int {
    const auto request = liquid::projection_request{};
    return static_cast<int>(request.target);
}
