// SPDX-License-Identifier: MIT

// The lock records of the package manager: a package spec as typed or written in a manifest, the
// stored package record, and the concretised spec that pins a resolution.

#include <cstddef>
#include <cstdint>

#include "fuzz.hpp"
#include "liquid/resolve.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto input = liquid::fuzz::select(liquid::fuzz::bytes(data, size), 3);
    if (!input) {
        return 0;
    }
    switch (input->decoder) {
    case 0: {
        const auto spec = liquid::parse_spec(liquid::fuzz::text(input->payload));
        if (spec) {
            (void)liquid::to_string(*spec);
        }
        break;
    }
    case 1: {
        const auto package = liquid::decode_package(input->payload);
        if (package) {
            (void)liquid::encode_package(*package);
        }
        break;
    }
    default: {
        const auto concretised = liquid::decode_concretised_spec(input->payload);
        if (concretised) {
            (void)liquid::encode_concretised_spec(*concretised);
        }
        break;
    }
    }
    return 0;
}
