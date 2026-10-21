// liquid-fuse-probe: exits 0 when this host can attempt a FUSE mount, or prints why it cannot
// and exits 77, the skip code the CLI tests that mount read (liquid#362).
#include <print>

#include "fuse_probe.hpp"

int main() {
    if (const auto reason = liquid::test::fuse_unavailable_reason()) {
        std::println("{}", *reason);
        return 77;
    }
    return 0;
}
