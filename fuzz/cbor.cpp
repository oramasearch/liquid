// Canonical CBOR, the encoding of every liquid record in the store and on the sync wire.
// Property: whatever decodes re-encodes, and the re-encoding decodes to the same value.

#include <cstddef>
#include <cstdint>

#include "encoding/cbor.hpp"
#include "fuzz.hpp"

namespace cbor = liquid::encoding::cbor;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto decoded = cbor::decode(liquid::fuzz::bytes(data, size));
    if (!decoded) {
        return 0;
    }
    const auto encoded = cbor::encode(*decoded);
    if (!encoded) {
        return 0;
    }
    const auto again = cbor::decode(*encoded);
    liquid::fuzz::require(again.has_value() && *again == *decoded);
    return 0;
}
