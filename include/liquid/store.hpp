#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include "liquid/export.hpp"
#include "liquid/hash.hpp"
#include "liquid/runtime.hpp"

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

    [[nodiscard]] static runtime::task<store> open(std::filesystem::path path,
                                                   store_options options = {});
    [[nodiscard]] runtime::task<digest> put(std::vector<std::byte> input);
    [[nodiscard]] runtime::task<std::vector<std::byte>> get(digest id);
    [[nodiscard]] runtime::task<std::uint64_t> read(digest id, std::uint64_t offset,
                                                    std::span<std::byte> into);
    [[nodiscard]] runtime::task<bool> contains(digest id);
    [[nodiscard]] runtime::task<lease> lease_root(digest id, std::chrono::seconds ttl);
    [[nodiscard]] runtime::task<collection_report> collect(collection_options options = {});

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

}  // namespace liquid
