#include "liquid/hello.hpp"

#include <format>

#include "liquid/version.hpp"

namespace liquid {

std::string hello() { return std::format("hello from liquid {} — drip, drip, drip", version_string()); }

}  // namespace liquid
