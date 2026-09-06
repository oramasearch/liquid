#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/export.hpp"
#include "liquid/x509.hpp"

namespace liquid::transparency {

struct timestamp_verification {
    std::chrono::sys_seconds generated_at;
    std::string policy;
    std::string nonce;
    std::string digest_algorithm;
    std::string signature_algorithm;
    x509::certificate signer;
    std::vector<x509::certificate> certificate_path;
};

class LIQUID_EXPORT timestamp_token {
public:
    static auto verify(std::span<const std::byte> token,
                       std::span<const std::byte> imprint_value,
                       const x509::trust_anchor_set& trust) -> result<timestamp_verification>;
};

struct archive_timestamp_verification {
    std::uint64_t sequence = 0;
    std::vector<std::byte> encoded;
    timestamp_verification timestamp;
};

struct evidence_verification {
    std::string digest_algorithm;
    std::vector<archive_timestamp_verification> archive_timestamps;
};

class LIQUID_EXPORT evidence_record {
public:
    static auto verify(std::span<const std::byte> record,
                       std::span<const std::byte> checkpoint_signature_value,
                       const x509::trust_anchor_set& trust) -> result<evidence_verification>;
};

} // namespace liquid::transparency
