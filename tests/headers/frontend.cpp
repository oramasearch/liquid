#include "liquid/frontend.hpp"

void use_frontend_header() {
    liquid::frontend_registry registry;
    static_cast<void>(registry.find("c++"));
}
