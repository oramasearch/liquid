#include "liquid/test/forge_double.hpp"

auto main() -> int {
    return sizeof(liquid::test::forge_double) > 0 ? 0 : 1;
}
