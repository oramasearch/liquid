// SPDX-License-Identifier: MIT

#include "projection/projection.hpp"

#include <algorithm>
#include <array>
#include <cctype>

#include "formats/deflate/deflate.hpp"
#include "projection/attestations.hpp"

namespace liquid::projection_detail {
namespace {

struct pkgbuild_output {
    std::filesystem::path archive_path;
    std::vector<std::byte> archive;
    digest archive_digest;
    std::filesystem::path pkgbuild_path;
    std::vector<std::byte> pkgbuild;
    digest pkgbuild_digest;
    std::filesystem::path srcinfo_path;
    std::vector<std::byte> srcinfo;
    digest srcinfo_digest;
    std::vector<std::string> dependencies;
};

auto shell_quote(std::string_view value) -> std::string {
    auto output = std::string{"'"};
    for (const auto character : value) {
        if (character == '\'') {
            output += "'\\''";
        } else {
            output.push_back(character);
        }
    }
    output.push_back('\'');
    return output;
}

auto valid_identity(const package_metadata& metadata) -> bool {
    const auto valid_name = [](unsigned char character) {
        return std::islower(character) || std::isdigit(character) || character == '@' ||
               character == '.' || character == '_' || character == '+' || character == '-';
    };
    return !metadata.name.empty() && std::ranges::all_of(metadata.name, valid_name) &&
           metadata.version.find_first_of("-:") == std::string::npos &&
           metadata.release.find_first_not_of("0123456789") == std::string::npos;
}

auto arch_name(std::string_view architecture) -> std::string {
    if (architecture == "amd64") {
        return "x86_64";
    }
    if (architecture == "arm64") {
        return "aarch64";
    }
    return std::string{architecture};
}

auto generate_pkgbuild(const prepared_projection& prepared) -> result<pkgbuild_output> {
    if (!valid_identity(prepared.metadata)) {
        return std::unexpected(error{errc::invalid_argument,
                                     "package identity is not valid for PKGBUILD",
                                     prepared.metadata.name});
    }
    auto archive_tar =
        formats::package::tar(prepared.files,
                              prepared.options.source_date_epoch,
                              prepared.metadata.name + "-" + prepared.metadata.version);
    if (!archive_tar) {
        return std::unexpected(archive_tar.error());
    }
    auto archive = formats::deflate::gzip(*archive_tar);
    auto archive_digest = formats::package::sha256(archive);
    if (!archive_digest) {
        return std::unexpected(archive_digest.error());
    }
    const auto archive_name = prepared.metadata.name + "-" + prepared.metadata.version + ".tar.gz";
    const auto package_dependencies = dependencies(prepared, projection_target::pkgbuild);
    auto pkgbuild = std::string{"pkgname="} + shell_quote(prepared.metadata.name) + "\n";
    pkgbuild += "pkgver=" + shell_quote(prepared.metadata.version) + "\n";
    pkgbuild += "pkgrel=" + shell_quote(prepared.metadata.release) + "\n";
    pkgbuild += "pkgdesc=" + shell_quote(prepared.metadata.description) + "\n";
    pkgbuild += "arch=(" + shell_quote(arch_name(prepared.metadata.architecture)) + ")\n";
    pkgbuild += "url=" + shell_quote(prepared.metadata.homepage) + "\n";
    pkgbuild += "license=(" + shell_quote(prepared.metadata.licence) + ")\n";
    pkgbuild += "depends=(";
    for (const auto& dependency : package_dependencies) {
        pkgbuild += shell_quote(dependency);
        pkgbuild.push_back(' ');
    }
    if (!package_dependencies.empty()) {
        pkgbuild.pop_back();
    }
    pkgbuild += ")\n";
    pkgbuild += "options=(!strip)\n";
    pkgbuild += "source=(" + shell_quote(archive_name) + ")\n";
    pkgbuild +=
        "sha256sums=(" + shell_quote(formats::package::hex_bytes(archive_digest->bytes)) + ")\n\n";
    pkgbuild += "package() {\n";
    pkgbuild += "    install -d \"$pkgdir/usr\"\n";
    pkgbuild += "    cp -a \"$srcdir/$pkgname-$pkgver/.\" \"$pkgdir/usr/\"\n";
    pkgbuild += "}\n";
    auto srcinfo = std::string{"pkgbase = "} + prepared.metadata.name + "\n";
    srcinfo += "\tpkgdesc = " + prepared.metadata.description + "\n";
    srcinfo += "\tpkgver = " + prepared.metadata.version + "\n";
    srcinfo += "\tpkgrel = " + prepared.metadata.release + "\n";
    srcinfo += "\turl = " + prepared.metadata.homepage + "\n";
    srcinfo += "\tarch = " + arch_name(prepared.metadata.architecture) + "\n";
    srcinfo += "\tlicense = " + prepared.metadata.licence + "\n";
    for (const auto& dependency : package_dependencies) {
        srcinfo += "\tdepends = " + dependency + "\n";
    }
    srcinfo += "\toptions = !strip\n";
    srcinfo += "\tsource = " + archive_name + "\n";
    srcinfo += "\tsha256sums = " + formats::package::hex_bytes(archive_digest->bytes) + "\n\n";
    srcinfo += "pkgname = " + prepared.metadata.name + "\n";
    auto pkgbuild_bytes = formats::package::text_bytes(pkgbuild);
    auto srcinfo_bytes = formats::package::text_bytes(srcinfo);
    auto pkgbuild_digest = formats::package::sha256(pkgbuild_bytes);
    auto srcinfo_digest = formats::package::sha256(srcinfo_bytes);
    if (!pkgbuild_digest || !srcinfo_digest) {
        return std::unexpected(pkgbuild_digest ? srcinfo_digest.error() : pkgbuild_digest.error());
    }
    return pkgbuild_output{prepared.options.output_directory / archive_name,
                           std::move(archive),
                           *archive_digest,
                           prepared.options.output_directory / "PKGBUILD",
                           std::move(pkgbuild_bytes),
                           *pkgbuild_digest,
                           prepared.options.output_directory / ".SRCINFO",
                           std::move(srcinfo_bytes),
                           *srcinfo_digest,
                           package_dependencies};
}

auto append_attached(repository& source,
                     const prepared_projection& prepared,
                     projection_report& report,
                     const projected_artifact& artifact) -> runtime::task<void> {
    auto attached = co_await attach_sidecar(
        source, prepared, projection_target::pkgbuild, artifact.path, artifact.digest);
    if (!attached) {
        co_return std::unexpected(attached.error());
    }
    report.artifacts.push_back(artifact);
    report.artifacts.push_back(attached->sidecar);
    report.attestations.push_back(attached->projection_attestation);
    co_return result<void>{};
}

} // namespace

runtime::task<projection_report> project_pkgbuild(repository& source,
                                                  prepared_projection prepared) {
    auto generated = co_await runtime::offload([prepared] { return generate_pkgbuild(prepared); });
    if (!generated) {
        co_return std::unexpected(generated.error());
    }
    if (!*generated) {
        co_return std::unexpected(generated->error());
    }
    auto output = std::move(**generated);
    auto stored = co_await runtime::offload([output] {
        const auto values =
            std::array{std::pair{output.archive_path, std::span<const std::byte>{output.archive}},
                       std::pair{output.pkgbuild_path, std::span<const std::byte>{output.pkgbuild}},
                       std::pair{output.srcinfo_path, std::span<const std::byte>{output.srcinfo}}};
        for (const auto& [path, bytes] : values) {
            auto written = write_file(path, bytes);
            if (!written) {
                return written;
            }
        }
        return result<void>{};
    });
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    if (!*stored) {
        co_return std::unexpected(stored->error());
    }
    auto report = projection_report{.attestations = prepared.source_attestations,
                                    .pinned = {{prepared.metadata.name, output.archive_digest}},
                                    .abi_break_allowed = prepared.abi_break_allowed};
    for (const auto& dependency : output.dependencies) {
        report.dependencies.push_back(
            {dependency,
             std::ranges::contains(prepared.metadata.declared_dependencies, dependency)});
    }
    const auto artifacts = std::array{
        projected_artifact{
            output.pkgbuild_path, output.pkgbuild_digest, "application/vnd.archlinux.pkgbuild"},
        projected_artifact{
            output.srcinfo_path, output.srcinfo_digest, "application/vnd.archlinux.srcinfo"},
        projected_artifact{output.archive_path, output.archive_digest, "application/gzip"}};
    for (const auto& artifact : artifacts) {
        auto attached = co_await append_attached(source, prepared, report, artifact);
        if (!attached) {
            co_return std::unexpected(attached.error());
        }
    }
    co_return report;
}

} // namespace liquid::projection_detail
