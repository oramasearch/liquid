#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/export.hpp"
#include "liquid/hash.hpp"

namespace liquid {

struct LIQUID_EXPORT store_options {
    algorithm hash_algorithm{algorithm::blake3};
    std::uint64_t pack_size{};
};

struct LIQUID_EXPORT collection_options {};

struct LIQUID_EXPORT collection_report {};

class LIQUID_EXPORT lease {
public:
    lease() noexcept;
    ~lease();
    lease(lease&&) noexcept;
    lease& operator=(lease&&) noexcept;
    lease(const lease&) = delete;
    lease& operator=(const lease&) = delete;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

class LIQUID_EXPORT store {
public:
    store() noexcept;
    ~store();
    store(store&&) noexcept;
    store& operator=(store&&) noexcept;
    store(const store&) = delete;
    store& operator=(const store&) = delete;

    [[nodiscard]] static result<store> open(
        const std::filesystem::path& path, const store_options& options = {});
    [[nodiscard]] result<digest> put(std::span<const std::byte> input);
    [[nodiscard]] result<std::vector<std::byte>> get(const digest& id);
    [[nodiscard]] result<std::uint64_t> read(
        const digest& id, std::uint64_t offset, std::span<std::byte> into);
    [[nodiscard]] result<bool> contains(const digest& id);
    [[nodiscard]] result<lease> lease_root(const digest& id, std::chrono::seconds ttl);
    [[nodiscard]] result<collection_report> collect(
        const collection_options& options = {});

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

}  // namespace liquid
