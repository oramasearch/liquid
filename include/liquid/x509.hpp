#pragma once

#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/export.hpp"

namespace liquid::x509 {

enum class purpose { timestamp_signing };

struct certificate {
    std::vector<std::byte> der;
    std::string subject;
    std::string issuer;
    std::chrono::sys_seconds not_before;
    std::chrono::sys_seconds not_after;
    bool certificate_authority = false;
    bool digital_signature = false;
    bool certificate_signing = false;
    bool timestamp_signing = false;
};

class LIQUID_EXPORT trust_anchor_set {
public:
    auto add_der(std::span<const std::byte> encoded) -> result<void>;
    auto add_pem(std::string_view encoded) -> result<void>;

    [[nodiscard]] auto empty() const noexcept -> bool;
    [[nodiscard]] auto certificates() const noexcept -> const std::vector<std::vector<std::byte>>&;

private:
    std::vector<std::vector<std::byte>> certificates_;
};

LIQUID_EXPORT auto parse_certificate(std::span<const std::byte> encoded) -> result<certificate>;

LIQUID_EXPORT auto validate_path(const certificate& leaf,
                                 std::span<const certificate> intermediates,
                                 const trust_anchor_set& trust,
                                 std::chrono::sys_seconds at,
                                 purpose required_purpose) -> result<std::vector<certificate>>;

} // namespace liquid::x509
