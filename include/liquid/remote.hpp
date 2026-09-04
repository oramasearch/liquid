#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "liquid/export.hpp"
#include "liquid/hash.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

struct LIQUID_EXPORT remote_blob_location {
    std::string source;
    std::string instance;

    auto operator==(const remote_blob_location&) const -> bool = default;
};

class LIQUID_EXPORT remote_blob_source {
public:
    virtual ~remote_blob_source() = default;

    [[nodiscard]] virtual runtime::task<std::vector<digest>>
    find_missing(std::string instance, std::vector<digest> blobs) = 0;
    [[nodiscard]] virtual runtime::task<std::uint64_t> read_range(std::string instance,
                                                                  digest blob,
                                                                  std::uint64_t offset,
                                                                  std::span<std::byte> output) = 0;
    [[nodiscard]] virtual runtime::task<void>
    write(std::string instance, digest blob, std::vector<std::byte> contents) = 0;
};

class LIQUID_EXPORT remote {
public:
    remote() noexcept;
    ~remote();
    remote(remote&&) noexcept;
    remote& operator=(remote&&) noexcept;
    remote(const remote&) = delete;
    remote& operator=(const remote&) = delete;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace liquid
