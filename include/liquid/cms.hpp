#pragma once

#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/export.hpp"
#include "liquid/x509.hpp"

namespace liquid::cms {

struct signed_data_verification {
    std::vector<std::byte> content;
    std::string content_type;
    std::string digest_algorithm;
    std::string signature_algorithm;
    x509::certificate signer;
    std::vector<x509::certificate> certificate_path;
};

class LIQUID_EXPORT signed_data {
public:
    static auto verify(std::span<const std::byte> encoded,
                       const x509::trust_anchor_set& trust,
                       std::chrono::sys_seconds validation_time)
        -> result<signed_data_verification>;
};

} // namespace liquid::cms
