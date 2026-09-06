#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <variant>
#include <vector>

#include "liquid/build.hpp"
#include "liquid/export.hpp"
#include "liquid/hash.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

class repository;

enum class projection_target : std::uint8_t {
    oci_image,
    deb,
    rpm,
    apk,
    helm_chart,
    macos_pkg,
    macos_dmg,
    msix,
    winget_manifest,
};

enum class platform_signing_system : std::uint8_t {
    macos,
    windows,
};

struct LIQUID_EXPORT package_metadata {
    std::string name;
    std::string version;
    std::string release{"1"};
    std::string architecture;
    std::string description;
    std::string maintainer;
    std::string licence;
    std::string homepage;
    std::string identifier;
    std::string publisher;
    std::string display_name;
    std::string executable;
    std::vector<std::string> declared_dependencies;
    std::vector<std::string> provides;
    std::vector<std::string> conflicts;
    std::vector<std::string> replaces;
};

struct LIQUID_EXPORT projection_source {
    std::variant<result_id, digest, std::filesystem::path> value;
};

struct LIQUID_EXPORT projection_service {
    std::string name;
    std::string image_repository;
    digest image;
    std::vector<std::uint16_t> ports;
    std::map<std::string, std::string> env;
    std::vector<std::string> volumes;
};

struct LIQUID_EXPORT projection_options {
    std::filesystem::path output_directory{"projection"};
    std::uint64_t source_date_epoch{};
    bool compat_gzip{};
    bool allow_abi_break{};
    std::vector<std::string> pre_depends;
    std::vector<std::string> recommends;
    std::vector<std::string> breaks;
    std::vector<std::string> conffiles;
    std::vector<std::string> oci_entrypoint;
    std::vector<std::string> oci_command;
    std::filesystem::path entitlements;
    std::string installer_url;
};

struct LIQUID_EXPORT projection_request {
    std::vector<projection_source> sources;
    package_metadata metadata;
    projection_target target{projection_target::oci_image};
    projection_options options;
    std::vector<projection_service> services;
};

struct LIQUID_EXPORT projected_artifact {
    std::filesystem::path path;
    liquid::digest digest;
    std::string media_type;
    bool reproducible{true};
};

struct LIQUID_EXPORT platform_signing_request {
    std::filesystem::path path;
    std::filesystem::path artifact;
    liquid::digest unsigned_digest;
    platform_signing_system platform{platform_signing_system::macos};
    std::string identifier;
    std::string executable;
    std::filesystem::path entitlements;
};

struct LIQUID_EXPORT pinned_artifact {
    std::string name;
    liquid::digest digest;
};

struct LIQUID_EXPORT projection_dependency {
    std::string value;
    bool declared{};
};

struct LIQUID_EXPORT projection_report {
    std::vector<projected_artifact> artifacts;
    std::vector<platform_signing_request> signing_requests;
    std::vector<digest> attestations;
    std::vector<pinned_artifact> pinned;
    std::vector<projection_dependency> dependencies;
    bool abi_break_allowed{};
};

struct LIQUID_EXPORT platform_signing_options {
    std::string identity;
    std::string timestamp_server;
    std::filesystem::path output;
    bool notarize{true};
};

struct LIQUID_EXPORT platform_signature_report {
    projected_artifact artifact;
    liquid::digest unsigned_digest;
    std::filesystem::path attestation;
    std::string signing_identity_fingerprint;
    std::string platform_verification;
    std::string notarization_ticket;
    std::string timestamp_response;
    bool notarization_skipped{};
};

struct LIQUID_EXPORT platform_signature_verification {
    platform_signing_system platform{platform_signing_system::macos};
    liquid::digest signed_digest;
    std::string signing_identity_fingerprint;
    std::string platform_verification;
    std::string notarization_ticket;
    std::string timestamp_response;
};

[[nodiscard]] LIQUID_EXPORT runtime::task<projection_report> project(repository& source,
                                                                     projection_request request);
[[nodiscard]] LIQUID_EXPORT result<std::string>
projection_report_json(const projection_report& report);
[[nodiscard]] LIQUID_EXPORT runtime::task<platform_signature_report>
sign_platform(repository& source, std::filesystem::path artifact, platform_signing_options options);
[[nodiscard]] LIQUID_EXPORT runtime::task<platform_signature_verification>
verify_platform_signature(std::filesystem::path artifact);
[[nodiscard]] LIQUID_EXPORT std::string_view to_string(projection_target target) noexcept;
[[nodiscard]] LIQUID_EXPORT std::string_view to_string(platform_signing_system platform) noexcept;

} // namespace liquid
