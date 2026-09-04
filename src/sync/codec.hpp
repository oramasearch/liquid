#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "liquid/sync.hpp"

namespace liquid::sync_detail {

struct metadata_document {
    journal_cursor cursor;
    std::vector<digest> objects;
    std::vector<sync_item> items;
};

[[nodiscard]] auto encode_metadata(const metadata_document& document)
    -> result<std::vector<std::byte>>;
[[nodiscard]] auto decode_metadata(std::span<const std::byte> bytes) -> result<metadata_document>;
[[nodiscard]] auto encode_object_list(std::span<const digest> objects)
    -> result<std::vector<std::byte>>;
[[nodiscard]] auto decode_object_list(std::span<const std::byte> bytes)
    -> result<std::vector<digest>>;
[[nodiscard]] auto encode_pack_frame(digest id, std::span<const std::byte> contents)
    -> result<std::vector<std::byte>>;
[[nodiscard]] auto decode_pack_frames(std::span<const std::byte> bytes)
    -> result<std::vector<std::pair<digest, std::vector<std::byte>>>>;

} // namespace liquid::sync_detail
