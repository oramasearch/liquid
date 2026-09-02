#pragma once
#include <string>

#include "liquid/export.hpp"

namespace liquid {

// The hello-world capability: proves a client is linked against libliquid end to end.
// Behaviour lives in the library; the CLI, liquify and lake only call it.
[[nodiscard]] LIQUID_EXPORT std::string hello();

}  // namespace liquid
