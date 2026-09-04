#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include "liquid/store.hpp"
#include "store/index.hpp"
#include "store/pack.hpp"

namespace liquid::store_detail {

class store_impl {
public:
    struct chunk_reference {
        digest id;
        std::uint32_t length;
    };

    struct object_manifest {
        std::uint64_t size;
        std::vector<chunk_reference> chunks;
    };

    static result<std::unique_ptr<store_impl>> open(
        const std::filesystem::path& root, const store_options& options);

    result<digest> put(std::span<const std::byte> input);
    result<std::vector<std::byte>> get(const digest& id);
    result<std::uint64_t> read(
        const digest& id, std::uint64_t offset, std::span<std::byte> into);
    result<bool> contains(const digest& id) const;

private:
    store_impl(
        algorithm hash_algorithm,
        std::unique_ptr<chunk_index> index,
        pack_store packs);

    result<std::vector<chunk_reference>> write_chunks(std::span<const std::byte> input);
    result<std::vector<std::byte>> read_chunk(const chunk_reference& reference) const;
    result<object_manifest> load_manifest(const digest& object) const;

    algorithm hash_algorithm_;
    std::unique_ptr<chunk_index> index_;
    pack_store packs_;
};

}  // namespace liquid::store_detail
