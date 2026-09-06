#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/export.hpp"
#include "liquid/hash.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

enum class spack_loss_kind : std::uint8_t {
    dynamic_value,
    custom_phase,
    untranslatable_when,
    unknown_build_system,
    unsupported_value,
};

struct LIQUID_EXPORT spack_import_loss {
    spack_loss_kind kind{spack_loss_kind::unsupported_value};
    std::string field;
    std::string reason;

    auto operator==(const spack_import_loss&) const -> bool = default;
};

struct LIQUID_EXPORT spack_package_import {
    std::string name;
    std::vector<std::string> translated_fields;
    std::vector<spack_import_loss> losses;

    auto operator==(const spack_package_import&) const -> bool = default;
};

struct LIQUID_EXPORT spack_import_report {
    std::uint64_t v{1};
    std::uint64_t packages{};
    std::uint64_t translated_fields{};
    std::uint64_t losses{};
    std::uint64_t pinned_patches{};
    std::vector<std::string> roots;
    std::vector<spack_package_import> package_reports;

    auto operator==(const spack_import_report&) const -> bool = default;
};

struct LIQUID_EXPORT spack_import_options {
    // Tests and offline callers may supply the exact bytes for digest-less patch URLs. Production
    // imports fetch any URL absent from this map through liquid's coroutine-native HTTPS client.
    std::map<std::string, std::vector<std::byte>> patch_contents;
    std::optional<digest> toolchain;
};

[[nodiscard]] LIQUID_EXPORT runtime::task<spack_import_report>
import_spack(std::filesystem::path export_document,
             std::filesystem::path packages_repository,
             spack_import_options options = {});

[[nodiscard]] LIQUID_EXPORT result<std::string>
spack_import_report_json(const spack_import_report& report);
[[nodiscard]] LIQUID_EXPORT std::string spack_import_report_text(const spack_import_report& report);
[[nodiscard]] LIQUID_EXPORT result<spack_import_report>
decode_spack_import_report(std::string_view document);

} // namespace liquid
