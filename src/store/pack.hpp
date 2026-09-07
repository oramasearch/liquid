#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "../crypto/crypto.hpp"
#include "liquid/hash.hpp"
#include "liquid/runtime.hpp"

namespace liquid {
class store_backend;
}

namespace liquid::store_detail {

struct chunk_location {
    std::uint32_t pack_id{};
    std::uint64_t offset{};
    std::uint32_t length{};
};

struct pack_entry {
    digest id{};
    chunk_location location{};
    std::uint32_t raw_length{};
};

struct pack_contents {
    std::uint32_t id{};
    std::vector<pack_entry> entries;
    std::uint64_t data_end{};
    bool appendable{true};
};

[[nodiscard]] runtime::task<std::vector<std::byte>> read_file(std::filesystem::path path);
[[nodiscard]] runtime::task<std::vector<std::byte>>
read_file_range(std::filesystem::path path, std::uint64_t offset, std::size_t size);
[[nodiscard]] runtime::task<void> write_file(std::filesystem::path path,
                                             std::vector<std::byte> bytes);
[[nodiscard]] runtime::task<void>
write_file_range(std::filesystem::path path, std::uint64_t offset, std::vector<std::byte> bytes);
// Test instrumentation for the private Corosio file helpers.  The counter is
// intentionally byte based so it measures the I/O performed by index open.
void reset_file_read_counter() noexcept;
[[nodiscard]] std::uint64_t file_read_counter() noexcept;
void note_file_read(std::size_t bytes) noexcept;
void reset_file_write_counter() noexcept;
[[nodiscard]] std::uint64_t file_write_counter() noexcept;
[[nodiscard]] runtime::task<pack_contents>
read_pack(std::shared_ptr<::liquid::store_backend> backend, std::string key, std::uint32_t id);
[[nodiscard]] runtime::task<pack_entry>
append_pack_entry(std::shared_ptr<::liquid::store_backend> backend,
                  std::string key,
                  pack_contents contents,
                  digest id,
                  std::vector<std::byte> raw,
                  std::optional<crypto_detail::key> encryption_key,
                  bool durable);
[[nodiscard]] runtime::task<void> seal_pack(std::shared_ptr<::liquid::store_backend> backend,
                                            std::string key,
                                            const pack_contents& contents);
[[nodiscard]] runtime::task<std::vector<std::byte>>
read_pack_entry(std::shared_ptr<::liquid::store_backend> backend,
                std::string key,
                chunk_location location,
                digest expected,
                std::optional<crypto_detail::key> encryption_key = std::nullopt);
[[nodiscard]] runtime::task<pack_entry>
append_pack_frame(std::shared_ptr<::liquid::store_backend> backend,
                  std::string destination,
                  pack_contents contents,
                  std::string source,
                  pack_entry entry,
                  bool durable);

} // namespace liquid::store_detail
