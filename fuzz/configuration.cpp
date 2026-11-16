// SPDX-License-Identifier: MIT

// The manifest: a `.liquid` configuration file is parsed, checked, lowered to package, target and
// source records (with its `locks` rules) and formatted back. The lowered records are also stored
// and read back as CBOR, so their decoders are fuzzed here too.

#include <cstddef>
#include <cstdint>

#include "fuzz.hpp"
#include "liquid/configuration.hpp"
#include "liquid/install.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto input = liquid::fuzz::select(liquid::fuzz::bytes(data, size), 4);
    if (!input) {
        return 0;
    }
    switch (input->decoder) {
    case 0: {
        const auto document =
            liquid::parse_configuration(liquid::fuzz::text(input->payload), "fuzz.liquid");
        if (!document) {
            break;
        }
        (void)liquid::check(*document);
        (void)liquid::format_configuration(*document);
        const auto lowered = liquid::lower(*document);
        if (lowered) {
            for (const auto& target : lowered->targets) {
                (void)liquid::encode_target(target);
            }
        }
        break;
    }
    case 1:
        (void)liquid::decode_target(input->payload);
        break;
    case 2:
        (void)liquid::decode_source(input->payload);
        break;
    default:
        (void)liquid::decode_install_intent(input->payload);
        break;
    }
    return 0;
}
