#include "projection/projection.hpp"

#include "formats/ostree/ostree.hpp"

namespace liquid::projection_detail {

runtime::task<projection_report> project_ostree(repository& source, prepared_projection prepared) {
    auto built = co_await runtime::offload([prepared]() {
        return formats::ostree::build(prepared.files,
                                      prepared.directory_modes,
                                      prepared.components,
                                      prepared.options.ostree_ref,
                                      prepared.options.source_date_epoch);
    });
    if (!built) {
        co_return std::unexpected(built.error());
    }
    if (!*built) {
        co_return std::unexpected(built->error());
    }
    auto written = co_await runtime::offload([value = **built,
                                              output = prepared.options.output_directory,
                                              mode = prepared.options.ostree_mode] {
        return formats::ostree::write(value, output, mode);
    });
    if (!written) {
        co_return std::unexpected(written.error());
    }
    if (!*written) {
        co_return std::unexpected(written->error());
    }
    const auto projection = co_await store_projection_attestation(
        source, prepared, projection_target::ostree_commit, (**built).commit);
    if (!projection) {
        co_return std::unexpected(projection.error());
    }
    auto report = projection_report{.artifacts = {{prepared.options.output_directory,
                                                   (**built).commit,
                                                   "application/vnd.ostree.repository.v1"}},
                                    .attestations = prepared.source_attestations,
                                    .pinned = {{prepared.options.ostree_ref, (**built).commit}},
                                    .ostree_commit = (**built).commit,
                                    .abi_break_allowed = prepared.abi_break_allowed};
    report.attestations.push_back(*projection);
    co_return report;
}

} // namespace liquid::projection_detail
