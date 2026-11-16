// SPDX-License-Identifier: MIT

// DER, X.509 and CMS as liquid reads them from a timestamp authority or a signed release
// (liquid #463): src/asn1.cpp, src/x509.cpp and src/cms.cpp.
//
//   0  DER: walk every element, descending into constructed ones (at most 64 levels), and read
//      each as an unsigned integer and an object identifier
//   1  an X.509 certificate (parse_certificate)
//   2  trust anchors: PEM text (add_pem) and the same bytes as DER (add_der)
//   3  a certificate path: length-prefixed parts, the leaf, intermediates, and the trust anchor
//      last, validated for timestamp signing at a fixed time
//   4  CMS SignedData: two length-prefixed parts, the SignedData and a trust anchor, verified at
//      a fixed time

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "fuzz.hpp"
#include "liquid/asn1.hpp"
#include "liquid/cms.hpp"
#include "liquid/x509.hpp"

namespace asn1 = liquid::asn1;
namespace x509 = liquid::x509;

namespace {

constexpr std::size_t depth_limit = 64;
constexpr std::size_t part_limit = 8;

// 2026-01-01T00:00:00Z.
constexpr auto validation_time = std::chrono::sys_seconds{std::chrono::seconds{1'767'225'600}};

void walk(std::span<const std::byte> input, std::size_t depth) {
    auto reader = asn1::reader{input};
    for (;;) {
        const auto element = reader.next();
        if (!element || !*element) {
            return;
        }
        (void)asn1::unsigned_integer(**element);
        (void)asn1::object_identifier(**element);
        if ((*element)->identifier.constructed && depth < depth_limit) {
            walk((*element)->content, depth + 1);
        }
    }
}

auto anchor(std::span<const std::byte> input) -> x509::trust_anchor_set {
    auto output = x509::trust_anchor_set{};
    (void)output.add_der(input);
    return output;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto input = liquid::fuzz::select(liquid::fuzz::bytes(data, size), 5);
    if (!input) {
        return 0;
    }
    const auto payload = input->payload;
    switch (input->decoder) {
    case 0: {
        walk(payload, 0);
        auto single = asn1::reader{payload};
        (void)single.single();
        break;
    }
    case 1:
        (void)x509::parse_certificate(payload);
        break;
    case 2: {
        auto trust = x509::trust_anchor_set{};
        (void)trust.add_pem(liquid::fuzz::text(payload));
        (void)trust.add_der(payload);
        break;
    }
    case 3: {
        const auto split = liquid::fuzz::parts(payload, part_limit);
        if (split.size() < 2) {
            break;
        }
        const auto leaf = x509::parse_certificate(split.front());
        if (!leaf) {
            break;
        }
        auto intermediates = std::vector<x509::certificate>{};
        for (std::size_t index = 1; index + 1 < split.size(); ++index) {
            auto parsed = x509::parse_certificate(split[index]);
            if (parsed) {
                intermediates.push_back(std::move(*parsed));
            }
        }
        (void)x509::validate_path(*leaf,
                                  intermediates,
                                  anchor(split.back()),
                                  validation_time,
                                  x509::purpose::timestamp_signing);
        break;
    }
    default: {
        const auto split = liquid::fuzz::parts(payload, 2);
        if (split.empty()) {
            break;
        }
        const auto trust = split.size() > 1 ? anchor(split[1]) : x509::trust_anchor_set{};
        (void)liquid::cms::signed_data::verify(split[0], trust, validation_time);
        break;
    }
    }
    return 0;
}
