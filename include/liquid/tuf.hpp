#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/export.hpp"
#include "liquid/hash.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

class repository;

namespace tuf {

struct LIQUID_EXPORT trusted_root {
    std::uint64_t version{};
    digest sha256{algorithm::sha256, {}};
    std::vector<std::byte> encoded;
};

struct LIQUID_EXPORT metadata_versions {
    std::uint64_t root{};
    std::uint64_t timestamp{};
    std::uint64_t snapshot{};
    std::uint64_t targets{};
};

struct LIQUID_EXPORT target_description {
    std::string path;
    std::uint64_t length{};
    digest sha256{algorithm::sha256, {}};
};

struct LIQUID_EXPORT update_metadata {
    std::vector<std::vector<std::byte>> roots;
    std::vector<std::byte> timestamp;
    std::vector<std::byte> snapshot;
    std::vector<std::byte> targets;
};

struct LIQUID_EXPORT update_result {
    metadata_versions versions;
    std::vector<target_description> targets;
};

class LIQUID_EXPORT client {
public:
    client() noexcept;
    ~client();
    client(client&&) noexcept;
    client& operator=(client&&) noexcept;
    client(const client&) = delete;
    client& operator=(const client&) = delete;

    [[nodiscard]] static runtime::task<trusted_root> pin(repository& subject,
                                                         std::vector<std::byte> encoded_root);
    [[nodiscard]] static runtime::task<trusted_root>
    pin(repository& subject,
        std::vector<std::byte> encoded_root,
        std::chrono::sys_seconds verification_time);
    [[nodiscard]] static runtime::task<trusted_root> trusted(repository& subject);
    [[nodiscard]] static runtime::task<client> open(repository& subject);
    [[nodiscard]] static runtime::task<client> open(repository& subject,
                                                    std::chrono::sys_seconds verification_time);

    [[nodiscard]] runtime::task<update_result> update(update_metadata metadata);
    [[nodiscard]] result<target_description> target(std::string_view path) const;
    [[nodiscard]] result<void> verify_target(std::string_view path,
                                             std::span<const std::byte> contents) const;

private:
    struct impl;
    explicit client(std::unique_ptr<impl> implementation) noexcept;
    std::unique_ptr<impl> impl_;
};

} // namespace tuf
} // namespace liquid
