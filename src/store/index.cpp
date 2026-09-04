#include "store/index.hpp"

#include <array>
#include <limits>
#include <string>
#include <utility>

#include <rocksdb/cache.h>
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/table.h>

namespace liquid::store_detail {
namespace {

constexpr std::uint64_t default_cache_size = 512ULL * 1024ULL * 1024ULL;

std::string key_for(const digest& id) {
    std::string key(33, '\0');
    key[0] = static_cast<char>(id.alg);
    for (std::size_t index = 0; index < id.bytes.size(); ++index) {
        key[index + 1] = static_cast<char>(std::to_integer<unsigned char>(id.bytes[index]));
    }
    return key;
}

template <class Integer>
void append_little_endian(std::string& output, Integer value) {
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        output.push_back(static_cast<char>((value >> (index * 8U)) & 0xffU));
    }
}

template <class Integer>
Integer read_little_endian(std::string_view input, std::size_t offset) {
    Integer value = 0;
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        value |= static_cast<Integer>(static_cast<unsigned char>(input[offset + index]))
                 << (index * 8U);
    }
    return value;
}

error rocks_error(const rocksdb::Status& status, std::string context) {
    return error{errc::io, status.ToString(), std::move(context)};
}

result<bool> contains(
    rocksdb::DB& database, rocksdb::ColumnFamilyHandle* family, const digest& id) {
    const auto key = key_for(id);
    std::string value;
    const auto status = database.Get(rocksdb::ReadOptions{}, family, key, &value);
    if (status.IsNotFound()) return false;
    if (!status.ok()) return std::unexpected(rocks_error(status, to_string(id)));
    return true;
}

}  // namespace

chunk_index::chunk_index(
    std::unique_ptr<rocksdb::DB> database,
    rocksdb::ColumnFamilyHandle* chunks,
    rocksdb::ColumnFamilyHandle* objects)
    : database_(std::move(database)), chunks_(chunks), objects_(objects) {}

chunk_index::~chunk_index() {
    if (database_) {
        if (chunks_) database_->DestroyColumnFamilyHandle(chunks_);
        if (objects_) database_->DestroyColumnFamilyHandle(objects_);
    }
}

result<std::unique_ptr<chunk_index>> chunk_index::open(
    const std::filesystem::path& path, std::uint64_t cache_size) {
    rocksdb::Options options;
    options.create_if_missing = true;
    options.create_missing_column_families = true;
    options.compression = rocksdb::kNoCompression;

    rocksdb::BlockBasedTableOptions table;
    table.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
    table.block_cache = rocksdb::NewLRUCache(
        static_cast<std::size_t>(cache_size == 0 ? default_cache_size : cache_size));

    rocksdb::ColumnFamilyOptions family_options(options);
    family_options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table));
    std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;
    descriptors.emplace_back(rocksdb::kDefaultColumnFamilyName, family_options);
    descriptors.emplace_back("objects", family_options);

    std::vector<rocksdb::ColumnFamilyHandle*> handles;
    rocksdb::DB* raw = nullptr;
    const auto status = rocksdb::DB::Open(
        rocksdb::DBOptions(options), path.string(), descriptors, &handles, &raw);
    if (!status.ok()) return std::unexpected(rocks_error(status, path.string()));
    if (handles.size() != 2) {
        delete raw;
        return std::unexpected(error{errc::internal, "RocksDB opened unexpected column families", path.string()});
    }
    return std::unique_ptr<chunk_index>(
        new chunk_index(std::unique_ptr<rocksdb::DB>(raw), handles[0], handles[1]));
}

result<bool> chunk_index::contains_chunk(const digest& id) const {
    return contains(*database_, chunks_, id);
}

result<chunk_location> chunk_index::find_chunk(const digest& id) const {
    const auto key = key_for(id);
    std::string value;
    const auto status = database_->Get(rocksdb::ReadOptions{}, chunks_, key, &value);
    if (status.IsNotFound()) {
        return std::unexpected(error{errc::not_found, "chunk not found", to_string(id)});
    }
    if (!status.ok()) return std::unexpected(rocks_error(status, to_string(id)));
    if (value.size() != 16) {
        return std::unexpected(error{errc::corrupt, "chunk index value has the wrong size", to_string(id)});
    }
    return chunk_location{
        read_little_endian<std::uint32_t>(value, 0),
        read_little_endian<std::uint64_t>(value, 4),
        read_little_endian<std::uint32_t>(value, 12)};
}

result<void> chunk_index::put_chunk(const digest& id, const chunk_location& location) {
    std::string value;
    value.reserve(16);
    append_little_endian(value, location.pack_id);
    append_little_endian(value, location.offset);
    append_little_endian(value, location.length);
    rocksdb::WriteOptions options;
    options.sync = true;
    const auto status = database_->Put(options, chunks_, key_for(id), value);
    if (!status.ok()) return std::unexpected(rocks_error(status, to_string(id)));
    return {};
}

result<bool> chunk_index::contains_object(const digest& id) const {
    return contains(*database_, objects_, id);
}

result<std::vector<std::byte>> chunk_index::find_object(const digest& id) const {
    const auto key = key_for(id);
    std::string value;
    const auto status = database_->Get(rocksdb::ReadOptions{}, objects_, key, &value);
    if (status.IsNotFound()) {
        return std::unexpected(error{errc::not_found, "object not found", to_string(id)});
    }
    if (!status.ok()) return std::unexpected(rocks_error(status, to_string(id)));
    std::vector<std::byte> output(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        output[index] = std::byte{static_cast<unsigned char>(value[index])};
    }
    return output;
}

result<void> chunk_index::put_object(
    const digest& id, std::span<const std::byte> descriptor) {
    rocksdb::Slice value(
        reinterpret_cast<const char*>(descriptor.data()), descriptor.size());
    rocksdb::WriteOptions options;
    options.sync = true;
    const auto status = database_->Put(options, objects_, key_for(id), value);
    if (!status.ok()) return std::unexpected(rocks_error(status, to_string(id)));
    return {};
}

}  // namespace liquid::store_detail
