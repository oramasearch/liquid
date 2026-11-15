// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "crypto/signature.hpp"
#include "liquid/asn1.hpp"
#include "liquid/x509.hpp"

namespace liquid::x509::detail {

struct parsed_certificate {
    certificate summary;
    std::vector<std::byte> tbs;
    std::vector<std::byte> issuer_der;
    std::vector<std::byte> subject_der;
    std::vector<std::byte> serial;
    crypto::public_key public_key;
    crypto::signature_parameters signature_parameters;
    std::vector<std::byte> signature;
    bool timestamp_eku_critical = false;
};

auto parse(std::span<const std::byte> encoded) -> result<parsed_certificate>;
auto parse_signature_algorithm(const asn1::element& value) -> result<crypto::signature_parameters>;

} // namespace liquid::x509::detail
