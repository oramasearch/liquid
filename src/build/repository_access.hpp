#pragma once

#include <chrono>
#include <filesystem>
#include <span>
#include <vector>

#include "liquid/repository.hpp"

namespace liquid::repository_detail {

struct LIQUID_EXPORT build_access {
    [[nodiscard]] static auto get(repository& source, digest id)
        -> runtime::task<std::vector<std::byte>>;
    [[nodiscard]] static auto put_record(repository& source, std::vector<std::byte> bytes)
        -> runtime::task<digest>;
    [[nodiscard]] static auto put_file(repository& source, std::filesystem::path path)
        -> runtime::task<digest>;
    [[nodiscard]] static auto
    read(repository& source, digest id, std::uint64_t offset, std::span<std::byte> output)
        -> runtime::task<std::uint64_t>;
    [[nodiscard]] static auto change_time(repository& source, digest tree)
        -> runtime::task<std::chrono::sys_seconds>;
    [[nodiscard]] static auto path_change(repository& source, std::string path, digest content)
        -> runtime::task<std::optional<change_attribution>>;
    [[nodiscard]] static auto root_path(repository& source) -> result<std::filesystem::path>;
    [[nodiscard]] static auto contains(repository& source, digest id) -> runtime::task<bool>;
    [[nodiscard]] static auto sign_result(repository& source, action_result& value)
        -> runtime::task<void>;
    [[nodiscard]] static auto sign_attestation(repository& source, attestation& value)
        -> runtime::task<void>;
    [[nodiscard]] static auto signer(repository& source) -> result<actor>;
    [[nodiscard]] static auto retain_cache_root(repository& source, digest root)
        -> runtime::task<void>;
    [[nodiscard]] static auto release_cache_root(repository& source) -> runtime::task<void>;
    [[nodiscard]] static bool cache_publish_enabled(repository& source) noexcept;
    [[nodiscard]] static std::uint32_t verification_rate(repository& source) noexcept;
    static void record_cache_lookup(repository& source,
                                    digest result,
                                    cache_status status,
                                    std::optional<cache_miss> why,
                                    std::uint64_t bytes_served);
    static void record_cache_store(repository& source, std::uint64_t bytes);
    static void record_critical_alert(repository& source, critical_cache_alert alert);
    static void apply_cache_lookup(repository& source, digest result, action_result& value);
};

} // namespace liquid::repository_detail
