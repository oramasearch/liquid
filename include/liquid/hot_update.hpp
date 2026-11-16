// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/build.hpp"
#include "liquid/export.hpp"
#include "liquid/frontend.hpp"
#include "liquid/hash.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

class repository;

enum class hot_update_tier : std::uint8_t {
    tier_0 = 0,
    tier_1 = 1,
    tier_2 = 2,
    tier_3 = 3,
    tier_4 = 4,
};

enum class hot_update_outcome : std::uint8_t {
    patched,
    restart,
};

struct LIQUID_EXPORT hot_update_unit {
    frontend_unit_kind kind{frontend_unit_kind::definition};
    std::string name;
    std::string signature;
    digest source{};
    digest output{};
};

struct LIQUID_EXPORT hot_update_build_evidence {
    result_id result;
    digest artifact{};
    std::string changed_input;
    std::vector<hot_update_unit> units;
    std::optional<digest> abi_descriptor;
};

struct LIQUID_EXPORT hot_update_plan {
    result_id supersedes;
    result_id target;
    hot_update_tier tier{hot_update_tier::tier_4};
    hot_update_outcome outcome{hot_update_outcome::restart};
    std::vector<std::string> symbols;
    std::vector<digest> evidence;
    std::string reason;
};

struct LIQUID_EXPORT hot_update_patch {
    std::uint64_t v{1};
    result_id supersedes;
    result_id target;
    hot_update_tier tier{hot_update_tier::tier_1};
    std::vector<std::string> symbols;
    digest object{};
};

struct LIQUID_EXPORT hot_update_patch_creation {
    hot_update_patch value;
    digest record{};
    digest attestation{};
};

struct LIQUID_EXPORT hot_update_composition {
    result_id artifact;
    std::vector<digest> records;
    std::vector<hot_update_patch> patches;
};

struct LIQUID_EXPORT hot_update_apply_result {
    hot_update_outcome outcome{hot_update_outcome::restart};
    digest record{};
    result_id target;
    std::vector<std::string> symbols;
};

[[nodiscard]] LIQUID_EXPORT std::string_view to_string(hot_update_tier value) noexcept;
[[nodiscard]] LIQUID_EXPORT std::string_view to_string(hot_update_outcome value) noexcept;
[[nodiscard]] LIQUID_EXPORT result<std::vector<std::byte>>
encode_hot_update_evidence(const hot_update_build_evidence& value);
[[nodiscard]] LIQUID_EXPORT result<hot_update_build_evidence>
decode_hot_update_evidence(std::span<const std::byte> bytes);
[[nodiscard]] LIQUID_EXPORT result<std::vector<std::byte>>
encode_hot_update_patch(const hot_update_patch& value);
[[nodiscard]] LIQUID_EXPORT result<hot_update_patch>
decode_hot_update_patch(std::span<const std::byte> bytes);
[[nodiscard]] LIQUID_EXPORT result<std::string> hot_update_plan_json(const hot_update_plan& value);
[[nodiscard]] LIQUID_EXPORT result<std::string>
hot_update_status_json(const hot_update_composition& value);

namespace hot_update {

[[nodiscard]] LIQUID_EXPORT runtime::task<digest> record_evidence(repository& source,
                                                                  hot_update_build_evidence value);
[[nodiscard]] LIQUID_EXPORT runtime::task<hot_update_plan>
classify(repository& source, result_id old_result, result_id new_result);
[[nodiscard]] LIQUID_EXPORT runtime::task<hot_update_patch_creation>
create_patch(repository& source, hot_update_plan plan, std::vector<std::byte> object);
[[nodiscard]] LIQUID_EXPORT runtime::task<hot_update_patch_creation>
create_patch(repository& source, hot_update_plan plan);
[[nodiscard]] LIQUID_EXPORT runtime::task<hot_update_patch> verify_patch(repository& source,
                                                                         digest record);
[[nodiscard]] LIQUID_EXPORT runtime::task<hot_update_apply_result>
apply(repository& source, digest record, std::filesystem::path endpoint);
[[nodiscard]] LIQUID_EXPORT runtime::task<hot_update_apply_result>
record_outcome(repository& source, hot_update_plan plan);
[[nodiscard]] LIQUID_EXPORT runtime::task<hot_update_apply_result>
restart(repository& source,
        hot_update_plan plan,
        std::filesystem::path working_tree,
        std::vector<std::string> hook);
[[nodiscard]] LIQUID_EXPORT runtime::task<hot_update_composition> status(repository& source,
                                                                         result_id target);

} // namespace hot_update

} // namespace liquid
