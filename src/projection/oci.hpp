// SPDX-License-Identifier: MIT

#pragma once

#include "projection/projection.hpp"

namespace liquid::projection_detail {

struct oci_descriptor {
    std::string media_type;
    digest content;
    std::uint64_t size{};
    std::string artifact_type;
    std::map<std::string, std::string> annotations;
};

[[nodiscard]] auto oci_blob(std::filesystem::path root,
                            std::span<const std::byte> bytes,
                            std::string media_type) -> result<oci_descriptor>;
[[nodiscard]] auto oci_manifest(std::filesystem::path root,
                                const oci_descriptor& config,
                                std::span<const oci_descriptor> layers,
                                std::optional<oci_descriptor> subject = {},
                                std::string artifact_type = {},
                                std::map<std::string, std::string> annotations = {})
    -> result<oci_descriptor>;
[[nodiscard]] auto attach_attestations(repository& source,
                                       const prepared_projection& prepared,
                                       std::filesystem::path root,
                                       const oci_descriptor& subject,
                                       digest projection_attestation)
    -> runtime::task<std::vector<oci_descriptor>>;
[[nodiscard]] auto write_oci_index(std::filesystem::path root,
                                   std::span<const oci_descriptor> primary,
                                   std::span<const oci_descriptor> referrers) -> result<void>;

} // namespace liquid::projection_detail
