// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/runtime.hpp"

namespace liquid {
class store_backend;
}

namespace liquid::store_detail {

class mapped_file {
public:
    mapped_file() = default;
    ~mapped_file();
    mapped_file(mapped_file&& other) noexcept;
    mapped_file& operator=(mapped_file&& other) noexcept;
    mapped_file(const mapped_file&) = delete;
    mapped_file& operator=(const mapped_file&) = delete;

    [[nodiscard]] result<void> open(const std::filesystem::path& path);
    [[nodiscard]] runtime::task<void> open(std::shared_ptr<::liquid::store_backend> backend,
                                           std::string key);
    void close() noexcept;
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;

private:
    const std::byte* data_{};
    std::size_t size_{};
    std::vector<std::byte> owned_;
#if defined(_WIN32)
    void* file_{};
    void* mapping_{};
#endif
};

} // namespace liquid::store_detail
