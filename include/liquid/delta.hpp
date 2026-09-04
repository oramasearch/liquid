#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "liquid/export.hpp"
#include "liquid/hash.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

class store;

enum class delta_algorithm : std::uint8_t {
    zstd_patch = 1,
    executable_sections = 2,
};

struct LIQUID_EXPORT delta_record {
    digest source{};
    digest target{};
    delta_algorithm algorithm{delta_algorithm::zstd_patch};
    digest blob{};
};

struct LIQUID_EXPORT delta_creation {
    delta_record value;
    digest record{};
    digest attestation{};
    std::uint64_t source_bytes{};
    std::uint64_t target_bytes{};
    std::uint64_t delta_bytes{};
};

enum class delta_fetch_method : std::uint8_t {
    delta,
    chunks,
};

struct LIQUID_EXPORT delta_fetch_result {
    delta_fetch_method method{delta_fetch_method::chunks};
    digest source{};
    digest target{};
    std::optional<digest> record;
    std::optional<digest> attestation;
};

[[nodiscard]] LIQUID_EXPORT std::string_view to_string(delta_algorithm value) noexcept;
[[nodiscard]] LIQUID_EXPORT std::string_view to_string(delta_fetch_method value) noexcept;
[[nodiscard]] LIQUID_EXPORT result<std::vector<std::byte>> encode_delta(const delta_record& value);
[[nodiscard]] LIQUID_EXPORT result<delta_record> decode_delta(std::span<const std::byte> bytes);
[[nodiscard]] LIQUID_EXPORT result<std::vector<std::byte>>
make_delta(delta_algorithm algorithm,
           std::span<const std::byte> source,
           std::span<const std::byte> target);
[[nodiscard]] LIQUID_EXPORT result<std::vector<std::byte>> apply_delta(
    const delta_record& value, std::span<const std::byte> source, std::span<const std::byte> delta);
[[nodiscard]] LIQUID_EXPORT runtime::task<delta_creation>
create_delta(store& destination,
             std::filesystem::path source,
             std::filesystem::path target,
             delta_algorithm algorithm = delta_algorithm::zstd_patch);
[[nodiscard]] LIQUID_EXPORT runtime::task<void> apply_delta(store& source,
                                                            digest record,
                                                            std::filesystem::path base,
                                                            std::filesystem::path destination);
[[nodiscard]] LIQUID_EXPORT runtime::task<delta_fetch_result> fetch_delta(
    store& local, digest target, std::filesystem::path source, std::filesystem::path destination);

} // namespace liquid
