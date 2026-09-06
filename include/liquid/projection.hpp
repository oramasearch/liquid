#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "liquid/build.hpp"
#include "liquid/export.hpp"
#include "liquid/hash.hpp"
#include "liquid/install.hpp"
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
    ostree_commit,
    bootc_image,
    homebrew,
    pkgbuild,
    appimage,
    flatpak,
    snap,
};

enum class platform_signing_system : std::uint8_t {
    macos,
    windows,
};

enum class ostree_repository_mode : std::uint8_t {
    archive_z2,
    bare_user,
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
    std::variant<result_id, digest> value;
    std::filesystem::path path;
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
    std::optional<std::filesystem::path> against;
    std::vector<std::string> pre_depends;
    std::vector<std::string> recommends;
    std::vector<std::string> breaks;
    std::vector<std::string> conffiles;
    std::vector<std::string> oci_entrypoint;
    std::vector<std::string> oci_command;
    std::filesystem::path entitlements;
    std::string installer_url;
    std::string ostree_ref;
    ostree_repository_mode ostree_mode{ostree_repository_mode::archive_z2};
};

struct LIQUID_EXPORT projection_request {
    std::vector<projection_source> sources;
    package_metadata metadata;
    projection_target target{projection_target::oci_image};
    projection_options options;
    std::vector<projection_service> services;
    std::optional<install_intent> install;
};

struct LIQUID_EXPORT generated_maintainer_scripts {
    std::string pre_install;
    std::string post_install;
    std::string pre_remove;
    std::string post_remove;
};

struct LIQUID_EXPORT projection_replacement {
    std::string package;
    std::vector<std::string> paths;
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
    std::vector<projection_replacement> replacements;
    std::optional<digest> ostree_commit;
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
[[nodiscard]] LIQUID_EXPORT runtime::task<std::vector<projection_report>>
project(repository& source, std::vector<projection_request> requests);
[[nodiscard]] LIQUID_EXPORT result<generated_maintainer_scripts>
generate_maintainer_scripts(projection_target target, const install_intent& intent);
[[nodiscard]] LIQUID_EXPORT result<std::string>
projection_report_json(const projection_report& report);
[[nodiscard]] LIQUID_EXPORT runtime::task<platform_signature_report>
sign_platform(repository& source, std::filesystem::path artifact, platform_signing_options options);
[[nodiscard]] LIQUID_EXPORT runtime::task<platform_signature_verification>
verify_platform_signature(std::filesystem::path artifact);
[[nodiscard]] LIQUID_EXPORT std::string_view to_string(projection_target target) noexcept;
[[nodiscard]] LIQUID_EXPORT std::string_view to_string(platform_signing_system platform) noexcept;
[[nodiscard]] LIQUID_EXPORT std::string_view to_string(ostree_repository_mode mode) noexcept;

} // namespace liquid
