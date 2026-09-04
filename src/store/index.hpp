#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/hash.hpp"

namespace rocksdb {
class DB;
class ColumnFamilyHandle;
}  // namespace rocksdb

namespace liquid::store_detail {

struct chunk_location {
    std::uint32_t pack_id;
    std::uint64_t offset;
    std::uint32_t length;
};

class chunk_index {
public:
    static result<std::unique_ptr<chunk_index>> open(
        const std::filesystem::path& path, std::uint64_t cache_size);
    ~chunk_index();

    chunk_index(const chunk_index&) = delete;
    chunk_index& operator=(const chunk_index&) = delete;

    result<bool> contains_chunk(const digest& id) const;
    result<chunk_location> find_chunk(const digest& id) const;
    result<void> put_chunk(const digest& id, const chunk_location& location);

    result<bool> contains_object(const digest& id) const;
    result<std::vector<std::byte>> find_object(const digest& id) const;
    result<void> put_object(const digest& id, std::span<const std::byte> descriptor);

private:
    chunk_index(
        std::unique_ptr<rocksdb::DB> database,
        rocksdb::ColumnFamilyHandle* chunks,
        rocksdb::ColumnFamilyHandle* objects);

    std::unique_ptr<rocksdb::DB> database_;
    rocksdb::ColumnFamilyHandle* chunks_{};
    rocksdb::ColumnFamilyHandle* objects_{};
};

}  // namespace liquid::store_detail
