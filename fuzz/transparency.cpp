// SPDX-License-Identifier: MIT

// Transparency documents (liquid #463): RFC 3161 timestamp tokens and RFC 4998 evidence records
// that renew a log checkpoint's signature (src/transparency.cpp), and the entry and hash tiles a
// log serves (tile::entries and tile::hashes in src/lake_transparency.cpp).
//
//   0  a timestamp token: length-prefixed parts, the token, the imprinted value, a trust anchor
//   1  an evidence record: length-prefixed parts, the record, the checkpoint signature it covers,
//      a trust anchor
//   2  a tile: a level byte, a width byte pair (big-endian), then the tile bytes; the harness
//      fills in the SHA-256 the log would state, so the parser reaches the records

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "fuzz.hpp"
#include "liquid/hash.hpp"
#include "liquid/transparency.hpp"
#include "liquid/x509.hpp"

namespace transparency = liquid::transparency;

namespace {

auto anchors(const std::vector<std::span<const std::byte>>& split, std::size_t index)
    -> liquid::x509::trust_anchor_set {
    auto output = liquid::x509::trust_anchor_set{};
    if (index < split.size()) {
        (void)output.add_der(split[index]);
    }
    return output;
}

void read_tile(std::span<const std::byte> payload) {
    if (payload.size() < 3) {
        return;
    }
    auto value = transparency::tile{};
    value.level = std::to_integer<std::uint32_t>(payload[0]);
    value.index = 0;
    value.width = (std::to_integer<std::uint64_t>(payload[1]) << 8U) |
                  std::to_integer<std::uint64_t>(payload[2]);
    value.bytes.assign(payload.begin() + 3, payload.end());
    const auto digest = liquid::hash(liquid::algorithm::sha256, value.bytes);
    if (!digest) {
        return;
    }
    value.sha256 = digest->bytes;
    if (value.level == 0) {
        (void)value.entries();
    } else {
        (void)value.hashes();
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto input = liquid::fuzz::select(liquid::fuzz::bytes(data, size), 3);
    if (!input) {
        return 0;
    }
    switch (input->decoder) {
    case 0: {
        const auto split = liquid::fuzz::parts(input->payload, 3);
        if (split.size() < 2) {
            break;
        }
        (void)transparency::timestamp_token::verify(split[0], split[1], anchors(split, 2));
        break;
    }
    case 1: {
        const auto split = liquid::fuzz::parts(input->payload, 3);
        if (split.size() < 2) {
            break;
        }
        (void)transparency::evidence_record::verify(split[0], split[1], anchors(split, 2));
        break;
    }
    default:
        read_tile(input->payload);
        break;
    }
    return 0;
}
