#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/hash.hpp"
#include "liquid/identity.hpp"

namespace liquid {

enum class attestation_subject_kind : std::uint8_t {
    actor,
    drip,
    change,
    artifact,
    model,
    library,
    repository,
};

enum class attestation_field_type : std::uint8_t {
    text,
    unsigned_integer,
    boolean,
    text_list,
};

enum class attestation_status_kind : std::uint8_t {
    none,
    max_age,
    revocable,
};

using attestation_value = std::variant<std::string, std::uint64_t, bool, std::vector<std::string>>;

struct LIQUID_EXPORT scan_allowance {
    std::string path;
    std::string detector;
    std::uint64_t line{};
    std::string reason;
};

struct LIQUID_EXPORT attestation_attribute {
    std::string name;
    attestation_value value;
};

struct LIQUID_EXPORT attribute_disclosure_commitment {
    std::string name;
    std::vector<std::byte> value;
};

struct LIQUID_EXPORT attribute_disclosure {
    attestation_attribute attribute;
    std::vector<std::byte> salt;
};

struct LIQUID_EXPORT profile_status_policy {
    attestation_status_kind kind{attestation_status_kind::none};
    std::chrono::seconds maximum_age{};
};

struct LIQUID_EXPORT attestation_status {
    bool revoked{};
    std::chrono::sys_seconds checked_at{};
};

struct LIQUID_EXPORT slsa_resource_descriptor {
    std::string uri;
    std::map<std::string, std::string> digest;
    std::optional<std::string> name;
};

struct LIQUID_EXPORT slsa_external_parameters {
    std::vector<std::string> command;
    std::string cwd;
    std::map<std::string, std::string> environment;
};

struct LIQUID_EXPORT slsa_internal_parameters {
    slsa_resource_descriptor toolchain;
    slsa_resource_descriptor sandbox;
    std::map<std::string, std::string> platform;
};

struct LIQUID_EXPORT slsa_build_definition {
    std::string build_type;
    slsa_external_parameters external_parameters;
    slsa_internal_parameters internal_parameters;
    std::vector<slsa_resource_descriptor> resolved_dependencies;
};

struct LIQUID_EXPORT slsa_builder {
    std::string id;
};

struct LIQUID_EXPORT slsa_build_metadata {
    std::string invocation_id;
    std::string started_on;
    std::string finished_on;
};

struct LIQUID_EXPORT slsa_run_details {
    slsa_builder builder;
    slsa_build_metadata metadata;
    std::vector<slsa_resource_descriptor> byproducts;
};

struct LIQUID_EXPORT slsa_provenance_v1 {
    slsa_build_definition build_definition;
    slsa_run_details run_details;
};

struct LIQUID_EXPORT attestation {
    digest subject{};
    std::string profile;
    std::vector<std::string> detectors;
    std::uint64_t findings_count{};
    std::chrono::sys_seconds at{};
    actor issuer;
    std::vector<scan_allowance> allowances;
    std::vector<std::byte> signature;
    attestation_subject_kind subject_kind{attestation_subject_kind::drip};
    std::vector<attestation_attribute> attributes;
    std::vector<attribute_disclosure_commitment> disclosure_commitments;
    std::optional<slsa_provenance_v1> slsa_provenance;
};

struct LIQUID_EXPORT attestation_profile_field {
    std::string name;
    attestation_field_type type{attestation_field_type::text};
    bool required{};
    bool may_commit{};
};

struct LIQUID_EXPORT attestation_profile {
    std::string name;
    std::vector<attestation_profile_field> schema;
    std::vector<attestation_subject_kind> subject_kinds;
    profile_status_policy status_policy;
};

struct LIQUID_EXPORT attestation_verification_options {
    std::chrono::sys_seconds now{};
    std::optional<attestation_status> status;
    std::vector<attribute_disclosure> disclosures;
    // The issuer's identity chain, when it travels with the attestation. Empty means the chain is
    // read from this machine's identity store; an issuer with no chain there is unknown and the
    // attestation does not verify.
    std::vector<identity_document> issuer_chain;
};

struct LIQUID_EXPORT attestation_verification {
    bool verified{};
    std::string reason;
    // Set when no identity chain binds the issuer's name to a key (`is_unknown_signer`).
    bool unknown_signer{};
};

class LIQUID_EXPORT attestation_profile_registry {
public:
    [[nodiscard]] static const attestation_profile_registry& global();
    [[nodiscard]] const std::vector<attestation_profile>& profiles() const noexcept;
    [[nodiscard]] const attestation_profile* find(std::string_view name) const noexcept;
    [[nodiscard]] result<void> validate(const attestation& value) const;

private:
    attestation_profile_registry();
    std::vector<attestation_profile> profiles_;
};

[[nodiscard]] LIQUID_EXPORT std::string_view to_string(attestation_subject_kind value) noexcept;
[[nodiscard]] LIQUID_EXPORT std::string_view to_string(attestation_field_type value) noexcept;
[[nodiscard]] LIQUID_EXPORT std::string_view to_string(attestation_status_kind value) noexcept;
[[nodiscard]] LIQUID_EXPORT result<std::vector<std::byte>>
attribute_commitment(const attestation_attribute& attribute, std::span<const std::byte> salt);
[[nodiscard]] LIQUID_EXPORT result<std::vector<std::byte>>
encode_attestation_signature(const signature& value);
[[nodiscard]] LIQUID_EXPORT result<std::vector<std::byte>>
encode_attestation(const attestation& value);
[[nodiscard]] LIQUID_EXPORT result<attestation>
decode_attestation(std::span<const std::byte> bytes);
[[nodiscard]] LIQUID_EXPORT result<attestation_verification>
verify_attestation(const attestation& value, const attestation_verification_options& options = {});
[[nodiscard]] LIQUID_EXPORT result<bool> verify(const attestation& value);
[[nodiscard]] LIQUID_EXPORT result<void> validate_delegation(const attestation& value,
                                                             const attestation* parent = nullptr);

// The validity window of a `delegation/1` (liquid#514). Every delegation carries `not_after`, in
// seconds since the Unix epoch. A delegation is valid only when `at` is no later than `now` plus
// `delegation_clock_skew`, `not_after` is after `at` and at most `delegation_maximum_lifetime`
// after it, and `now` is before `not_after`.
inline constexpr std::chrono::seconds delegation_maximum_lifetime{std::chrono::days{30}};
inline constexpr std::chrono::seconds delegation_clock_skew{std::chrono::minutes{5}};

// Refuses a delegation outside its validity window: `errc::unauthorized` when it is expired or
// issued in the future, `errc::invalid_argument` when `not_after` is missing or the window is
// longer than `delegation_maximum_lifetime`.
[[nodiscard]] LIQUID_EXPORT result<void> check_delegation_window(const attestation& value,
                                                                 std::chrono::sys_seconds now);

// Answers whether a delegation is revoked, at the time it is asked (liquid#514). `id` is the
// digest of the delegation's encoded record, the id `liquid session start` names. A session asks
// for every delegation it acts on when it is forked, when it is opened, and before every lease and
// publish. An empty resolver, an error, a revoked status or a status older than the profile's
// status age (one hour for `delegation/1`) refuses: there is no default that lets a delegation
// through unchecked.
using delegation_status_resolver =
    std::function<result<attestation_status>(const attestation& delegation, const digest& id)>;

// The local revocation list: a resolver that reads `$LIQUID_HOME/trust/delegation-revocations`
// (`trust_file_path`) each time it is asked. The file holds one delegation digest per line; blank
// lines and text after `#` are ignored. A digest listed there is revoked; a missing file means no
// local revocations. A line that is not a digest, or a file that cannot be read, is an error, so a
// damaged list refuses every delegation instead of revoking none. The status is checked now.
[[nodiscard]] LIQUID_EXPORT delegation_status_resolver local_delegation_status();
[[nodiscard]] LIQUID_EXPORT runtime::task<digest>
attest_image(std::string image_reference,
             digest tree,
             std::optional<digest> result,
             std::filesystem::path store_directory);

} // namespace liquid
