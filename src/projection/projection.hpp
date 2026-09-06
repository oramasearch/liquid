#pragma once

#include <map>
#include <span>

#include "formats/package/common.hpp"
#include "liquid/projection.hpp"
#include "liquid/repository.hpp"

namespace liquid::projection_detail {

struct prepared_projection {
    package_metadata metadata;
    projection_options options;
    std::vector<formats::package::file> files;
    std::vector<result_id> source_results;
    std::vector<std::string> source_ids;
    std::vector<digest> source_attestations;
    std::vector<std::string> needed;
    std::vector<std::string> sonames;
    std::map<std::string, std::uint8_t> stability_layers;
    std::map<std::string, std::uint32_t> directory_modes;
    std::map<std::string, std::string> components;
    std::string base_source;
    std::vector<std::string> overlay_sources;
    bool abi_break_allowed{};
};

[[nodiscard]] auto trace_output_layers(const build_trace& trace, digest target)
    -> std::map<std::string, std::uint8_t>;
[[nodiscard]] auto prepare(repository& source, const projection_request& request)
    -> runtime::task<prepared_projection>;
[[nodiscard]] auto dependencies(const prepared_projection& prepared, projection_target target)
    -> std::vector<std::string>;
[[nodiscard]] auto provides(const prepared_projection& prepared, projection_target target)
    -> std::vector<std::string>;
[[nodiscard]] auto store_projection_attestation(repository& source,
                                                const prepared_projection& prepared,
                                                projection_target target,
                                                digest subject) -> runtime::task<digest>;
[[nodiscard]] auto write_file(std::filesystem::path path, std::span<const std::byte> bytes)
    -> result<void>;
[[nodiscard]] auto
project_package(repository& source, projection_target target, prepared_projection prepared)
    -> runtime::task<projection_report>;
[[nodiscard]] auto project_homebrew(repository& source, prepared_projection prepared)
    -> runtime::task<projection_report>;
[[nodiscard]] auto project_pkgbuild(repository& source, prepared_projection prepared)
    -> runtime::task<projection_report>;
[[nodiscard]] auto project_appimage(repository& source, prepared_projection prepared)
    -> runtime::task<projection_report>;
[[nodiscard]] auto project_flatpak(repository& source, prepared_projection prepared)
    -> runtime::task<projection_report>;
[[nodiscard]] auto project_snap(repository& source, prepared_projection prepared)
    -> runtime::task<projection_report>;
[[nodiscard]] auto project_oci(repository& source, prepared_projection prepared)
    -> runtime::task<projection_report>;
[[nodiscard]] auto project_ostree(repository& source, prepared_projection prepared)
    -> runtime::task<projection_report>;
[[nodiscard]] auto project_bootc(repository& source, prepared_projection prepared)
    -> runtime::task<projection_report>;
[[nodiscard]] auto project_helm(repository& source,
                                prepared_projection prepared,
                                std::span<const projection_service> services)
    -> runtime::task<projection_report>;
[[nodiscard]] auto project_macos(repository& source,
                                 projection_target target,
                                 prepared_projection prepared) -> runtime::task<projection_report>;
[[nodiscard]] auto project_msix(repository& source, prepared_projection prepared)
    -> runtime::task<projection_report>;
[[nodiscard]] auto project_winget(repository& source, const projection_request& request)
    -> runtime::task<projection_report>;
[[nodiscard]] auto appx_manifest(const package_metadata& metadata) -> result<std::string>;
[[nodiscard]] auto winget_manifests(const package_metadata& metadata,
                                    std::string_view installer_url,
                                    digest installer) -> result<std::map<std::string, std::string>>;

} // namespace liquid::projection_detail
