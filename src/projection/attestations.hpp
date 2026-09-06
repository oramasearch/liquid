#pragma once

#include "projection/projection.hpp"

namespace liquid::projection_detail {

struct sidecar_attachment {
    digest projection_attestation;
    projected_artifact sidecar;
};

[[nodiscard]] auto attach_sidecar(repository& source,
                                  const prepared_projection& prepared,
                                  projection_target target,
                                  const std::filesystem::path& artifact,
                                  digest subject) -> runtime::task<sidecar_attachment>;

} // namespace liquid::projection_detail
