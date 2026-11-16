// SPDX-License-Identifier: MIT

#include "projection/projection.hpp"

#include <algorithm>
#include <cctype>

#include "formats/deflate/deflate.hpp"
#include "projection/attestations.hpp"

namespace liquid::projection_detail {
namespace {

struct homebrew_output {
    std::filesystem::path bottle_path;
    std::vector<std::byte> bottle;
    digest bottle_digest;
    std::filesystem::path source_path;
    std::vector<std::byte> source;
    digest source_digest;
    std::filesystem::path formula_path;
    std::vector<std::byte> formula;
    digest formula_digest;
};

auto ruby_quote(std::string_view value) -> std::string {
    auto output = std::string{"\""};
    for (const auto character : value) {
        if (character == '\\' || character == '\"' || character == '#') {
            output.push_back('\\');
        }
        output.push_back(character);
    }
    output.push_back('\"');
    return output;
}

auto formula_class(std::string_view name) -> std::string {
    auto output = std::string{};
    auto capital = true;
    for (const auto character : name) {
        if (!std::isalnum(static_cast<unsigned char>(character))) {
            capital = true;
            continue;
        }
        output.push_back(capital ? static_cast<char>(std::toupper(character)) : character);
        capital = false;
    }
    if (output.empty() || std::isdigit(static_cast<unsigned char>(output.front()))) {
        output.insert(0, "Liquid");
    }
    return output;
}

auto executable_name(std::span<const formats::package::file> files) -> std::string {
    for (const auto& file : files) {
        if (file.path.starts_with("bin/") && file.path.find('/', 4) == std::string::npos &&
            (file.mode & 0111U) != 0) {
            return file.path.substr(4);
        }
    }
    return {};
}

auto generate_homebrew(const prepared_projection& prepared) -> result<homebrew_output> {
    auto source_tar =
        formats::package::tar(prepared.files,
                              prepared.options.source_date_epoch,
                              prepared.metadata.name + "-" + prepared.metadata.version);
    auto bottle_tar =
        formats::package::tar(prepared.files,
                              prepared.options.source_date_epoch,
                              prepared.metadata.name + "/" + prepared.metadata.version);
    if (!source_tar || !bottle_tar) {
        return std::unexpected(source_tar ? bottle_tar.error() : source_tar.error());
    }
    auto source = formats::deflate::gzip(*source_tar);
    auto bottle = formats::deflate::gzip(*bottle_tar);
    auto source_digest = formats::package::sha256(source);
    auto bottle_digest = formats::package::sha256(bottle);
    if (!source_digest || !bottle_digest) {
        return std::unexpected(source_digest ? bottle_digest.error() : source_digest.error());
    }
    const auto source_name = prepared.metadata.name + "-" + prepared.metadata.version + ".tar.gz";
    const auto bottle_name =
        prepared.metadata.name + "-" + prepared.metadata.version + ".all.bottle.tar.gz";
    auto formula = std::string{"class "} + formula_class(prepared.metadata.name) + " < Formula\n";
    formula += "  desc " + ruby_quote(prepared.metadata.description) + "\n";
    formula += "  homepage " +
               ruby_quote(prepared.metadata.homepage.empty() ? "https://example.invalid/"
                                                             : prepared.metadata.homepage) +
               "\n";
    formula += "  url \"https://example.invalid/" + source_name + "\"\n";
    formula += "  sha256 \"" + formats::package::hex_bytes(source_digest->bytes) + "\"\n\n";
    formula += "  bottle do\n";
    formula += "    sha256 cellar: :any_skip_relocation, all: \"" +
               formats::package::hex_bytes(bottle_digest->bytes) + "\"\n";
    formula += "  end\n\n";
    for (const auto& dependency : prepared.metadata.declared_dependencies) {
        formula += "  depends_on " + ruby_quote(dependency) + "\n";
    }
    if (!prepared.metadata.declared_dependencies.empty()) {
        formula += "\n";
    }
    formula += "  def install\n";
    formula += "    prefix.install Dir[\"*\"]\n";
    formula += "  end\n\n";
    formula += "  test do\n";
    const auto executable = executable_name(prepared.files);
    if (executable.empty()) {
        formula += "    assert_predicate prefix, :exist?\n";
    } else {
        formula += "    assert_predicate bin/" + ruby_quote(executable) + ", :executable?\n";
    }
    formula += "  end\n";
    formula += "end\n";
    auto formula_bytes = formats::package::text_bytes(formula);
    auto formula_digest = formats::package::sha256(formula_bytes);
    if (!formula_digest) {
        return std::unexpected(formula_digest.error());
    }
    return homebrew_output{prepared.options.output_directory / bottle_name,
                           std::move(bottle),
                           *bottle_digest,
                           prepared.options.output_directory / source_name,
                           std::move(source),
                           *source_digest,
                           prepared.options.output_directory / (prepared.metadata.name + ".rb"),
                           std::move(formula_bytes),
                           *formula_digest};
}

auto append_attached(repository& source,
                     const prepared_projection& prepared,
                     projection_report& report,
                     const projected_artifact& artifact) -> runtime::task<void> {
    auto attached = co_await attach_sidecar(
        source, prepared, projection_target::homebrew, artifact.path, artifact.digest);
    if (!attached) {
        co_return std::unexpected(attached.error());
    }
    report.artifacts.push_back(artifact);
    report.artifacts.push_back(attached->sidecar);
    report.attestations.push_back(attached->projection_attestation);
    co_return result<void>{};
}

} // namespace

runtime::task<projection_report> project_homebrew(repository& source,
                                                  prepared_projection prepared) {
    auto generated = co_await runtime::offload([prepared] { return generate_homebrew(prepared); });
    if (!generated) {
        co_return std::unexpected(generated.error());
    }
    if (!*generated) {
        co_return std::unexpected(generated->error());
    }
    auto output = std::move(**generated);
    auto stored = co_await runtime::offload([output] {
        for (const auto* value : {&output.bottle_path, &output.source_path, &output.formula_path}) {
            const auto* bytes = value == &output.bottle_path   ? &output.bottle
                                : value == &output.source_path ? &output.source
                                                               : &output.formula;
            auto written = write_file(*value, *bytes);
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
                                    .pinned = {{prepared.metadata.name, output.bottle_digest}},
                                    .abi_break_allowed = prepared.abi_break_allowed};
    const auto artifacts = std::array{
        projected_artifact{output.bottle_path,
                           output.bottle_digest,
                           "application/vnd.homebrew.bottle.v1+tar+gzip"},
        projected_artifact{
            output.formula_path, output.formula_digest, "application/vnd.homebrew.formula.ruby"},
        projected_artifact{output.source_path, output.source_digest, "application/gzip"}};
    for (const auto& artifact : artifacts) {
        auto attached = co_await append_attached(source, prepared, report, artifact);
        if (!attached) {
            co_return std::unexpected(attached.error());
        }
    }
    co_return report;
}

} // namespace liquid::projection_detail
