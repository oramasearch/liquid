// SPDX-License-Identifier: MIT

// liquid's own binary delta (liquid #463): the delta record a peer or a store hands back, and the
// zstd patch that `liquid delta apply` and a delta fetch apply to a source.
//
//   0  a delta record: decode, encode, decode again; the second decode must equal the first
//   1  two length-prefixed parts, a source and a patch: the harness makes a record whose source
//      and blob digests match them, so apply_delta reaches the patch itself
//   2  as 1, with the record's algorithm set to executable_sections

#include <cstddef>
#include <cstdint>

#include "fuzz.hpp"
#include "liquid/delta.hpp"
#include "liquid/hash.hpp"

namespace {

auto same(const liquid::digest& left, const liquid::digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

void apply(std::span<const std::byte> payload, liquid::delta_algorithm algorithm) {
    const auto split = liquid::fuzz::parts(payload, 2);
    if (split.size() != 2) {
        return;
    }
    const auto source = liquid::hash(liquid::algorithm::blake3, split[0]);
    const auto blob = liquid::hash(liquid::algorithm::blake3, split[1]);
    if (!source || !blob) {
        return;
    }
    const auto record = liquid::delta_record{*source, *source, algorithm, *blob};
    (void)liquid::apply_delta(record, split[0], split[1]);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto input = liquid::fuzz::select(liquid::fuzz::bytes(data, size), 3);
    if (!input) {
        return 0;
    }
    switch (input->decoder) {
    case 0: {
        const auto decoded = liquid::decode_delta(input->payload);
        if (!decoded) {
            break;
        }
        const auto encoded = liquid::encode_delta(*decoded);
        if (!encoded) {
            break;
        }
        const auto again = liquid::decode_delta(*encoded);
        liquid::fuzz::require(
            again && same(again->source, decoded->source) && same(again->target, decoded->target) &&
            same(again->blob, decoded->blob) && again->algorithm == decoded->algorithm);
        break;
    }
    case 1:
        apply(input->payload, liquid::delta_algorithm::zstd_patch);
        break;
    default:
        apply(input->payload, liquid::delta_algorithm::executable_sections);
        break;
    }
    return 0;
}
