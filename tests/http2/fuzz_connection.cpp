// libFuzzer harness for the HTTP/2 connection (liquid#412). Built only with
// -DLIQUID_BUILD_FUZZERS=ON and Clang; see docs/library/http2.md.

#include <cstddef>
#include <cstdint>
#include <span>

#include "fuzz_input.hpp"

extern "C" auto LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) -> int {
    liquid::http2::fuzz::run_input(std::span<const std::uint8_t>(data, size));
    return 0;
}
