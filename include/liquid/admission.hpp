#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/attestation.hpp"
#include "liquid/error.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

enum class admission_mode : std::uint8_t {
    off,
    attestation,
    attestation_and_log,
};

struct LIQUID_EXPORT admission_log_policy {
    std::string lake;
    std::uint64_t max_checkpoint_age_seconds{};
};

struct LIQUID_EXPORT admission_policy {
    std::uint64_t v{1};
    admission_mode mode{admission_mode::attestation};
    std::vector<std::string> issuers;
    std::vector<std::string> profiles;
    std::vector<std::string> exempt;
    std::vector<std::string> exempt_namespaces;
    std::string attestation_source;
    admission_log_policy log;
};

enum class log_verdict : std::uint8_t {
    included,
    not_included,
    unavailable,
};

class LIQUID_EXPORT attestation_source {
public:
    virtual ~attestation_source() = default;

    [[nodiscard]] virtual runtime::task<std::vector<attestation>> by_subject(digest subject) = 0;
};

class LIQUID_EXPORT log_verifier {
public:
    virtual ~log_verifier() = default;

    [[nodiscard]] virtual runtime::task<log_verdict> verify_inclusion(digest attestation) = 0;
};

struct LIQUID_EXPORT admission_attestation_verdict {
    digest attestation{};
    std::string profile;
    std::string issuer;
    bool verified{};
    std::string rule_id;
    std::string reason;
};

struct LIQUID_EXPORT admission_reason {
    std::string rule_id;
    std::string message;

    auto operator==(const admission_reason&) const -> bool = default;
};

struct LIQUID_EXPORT admission_decision {
    std::uint64_t v{1};
    std::uint64_t sequence{};
    std::string policy{"default"};
    bool allowed{};
    std::string image;
    std::optional<digest> subject;
    std::string reason;
    std::vector<admission_reason> reasons;
    std::optional<digest> attestation;
    std::vector<admission_attestation_verdict> considered;
};

struct LIQUID_EXPORT admission_options {
    std::string listen{"0.0.0.0:8443"};
    std::string control_listen{"127.0.0.1:8083"};
    std::filesystem::path policy;
    std::filesystem::path policies;
    std::filesystem::path tls_certificate;
    std::filesystem::path tls_key;
    std::filesystem::path log_cache;
    std::filesystem::path store;
    std::string attestation_source;
    std::size_t decision_replay_capacity{256};
    bool insecure_http{};
};

[[nodiscard]] LIQUID_EXPORT std::string_view to_string(admission_mode value) noexcept;
[[nodiscard]] LIQUID_EXPORT result<std::string>
encode_admission_policy(const admission_policy& value);
[[nodiscard]] LIQUID_EXPORT result<admission_policy> decode_admission_policy(std::string_view json);
[[nodiscard]] LIQUID_EXPORT result<std::vector<std::byte>>
encode_admission_decision_cbor(const admission_decision& value);
[[nodiscard]] LIQUID_EXPORT result<admission_decision>
decode_admission_decision_cbor(std::span<const std::byte> cbor);
[[nodiscard]] LIQUID_EXPORT result<std::string>
encode_admission_decision_json(const admission_decision& value);
[[nodiscard]] LIQUID_EXPORT result<admission_decision>
decode_admission_decision_json(std::string_view json);
[[nodiscard]] LIQUID_EXPORT runtime::task<admission_decision>
review_image(const admission_policy& policy,
             std::string image_reference,
             attestation_source& source,
             log_verifier& log);
[[nodiscard]] LIQUID_EXPORT runtime::task<void> serve_admission(admission_options options);

} // namespace liquid
