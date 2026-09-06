#include "projection/projection.hpp"

#include <algorithm>

#include "formats/apk/apk.hpp"
#include "formats/deb/deb.hpp"
#include "formats/rpm/rpm.hpp"

namespace liquid::projection_detail {
namespace {

auto package_architecture(std::string architecture, projection_target target) -> std::string {
    if (target == projection_target::deb) {
        if (architecture == "x86_64" || architecture == "amd64") {
            return "amd64";
        }
        if (architecture == "aarch64" || architecture == "arm64") {
            return "arm64";
        }
    } else {
        if (architecture == "amd64") {
            return "x86_64";
        }
        if (architecture == "arm64") {
            return "aarch64";
        }
    }
    return architecture;
}

auto extension(projection_target target) -> std::string_view {
    if (target == projection_target::deb) {
        return "deb";
    }
    if (target == projection_target::rpm) {
        return "rpm";
    }
    return "apk";
}

} // namespace

runtime::task<projection_report>
project_package(repository& source, projection_target target, prepared_projection prepared) {
    prepared.metadata.architecture = package_architecture(prepared.metadata.architecture, target);
    auto package_dependencies = dependencies(prepared, target);
    prepared.metadata.provides = provides(prepared, target);
    auto written =
        co_await runtime::offload([target,
                                   metadata = prepared.metadata,
                                   options = prepared.options,
                                   dependencies = package_dependencies,
                                   files = prepared.files,
                                   scripts = prepared.scripts]() -> result<std::vector<std::byte>> {
            if (target == projection_target::deb) {
                return formats::deb::write(
                    metadata, options, dependencies, files, scripts ? &*scripts : nullptr);
            }
            if (target == projection_target::rpm) {
                return formats::rpm::write(
                    metadata, options, dependencies, files, scripts ? &*scripts : nullptr);
            }
            return formats::apk::write(
                metadata, options, dependencies, files, scripts ? &*scripts : nullptr);
        });
    if (!written) {
        co_return std::unexpected(written.error());
    }
    if (!*written) {
        co_return std::unexpected(written->error());
    }
    auto digest = formats::package::sha256(**written);
    if (!digest) {
        co_return std::unexpected(digest.error());
    }
    auto filename =
        prepared.metadata.name + "-" + prepared.metadata.version + "-" + prepared.metadata.release;
    if (target != projection_target::apk) {
        filename += "." + prepared.metadata.architecture;
    }
    filename += "." + std::string{extension(target)};
    auto path = prepared.options.output_directory / filename;
    auto stored = co_await runtime::offload(
        [path, bytes = std::move(**written)] { return write_file(path, bytes); });
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    if (!*stored) {
        co_return std::unexpected(stored->error());
    }
    auto attestation = co_await store_projection_attestation(source, prepared, target, *digest);
    if (!attestation) {
        co_return std::unexpected(attestation.error());
    }
    auto report = projection_report{
        .artifacts = {{path,
                       *digest,
                       target == projection_target::deb   ? "application/vnd.debian.binary-package"
                       : target == projection_target::rpm ? "application/x-rpm"
                                                          : "application/vnd.alpine.apk"}},
        .attestations = prepared.source_attestations,
        .pinned = {{prepared.metadata.name, *digest}},
        .replacements = prepared.replacements,
        .abi_break_allowed = prepared.abi_break_allowed};
    report.attestations.push_back(*attestation);
    if (prepared.scripts) {
        auto sbom = co_await store_script_sbom(source, prepared, target, *digest, filename);
        if (!sbom) {
            co_return std::unexpected(sbom.error());
        }
        report.attestations.push_back(*sbom);
    }
    for (const auto& value : package_dependencies) {
        report.dependencies.push_back(
            {value, std::ranges::contains(prepared.metadata.declared_dependencies, value)});
    }
    co_return report;
}

} // namespace liquid::projection_detail
