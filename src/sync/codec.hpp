// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "liquid/sync.hpp"

namespace liquid::sync_detail {

struct metadata_document {
    journal_cursor cursor;
    std::vector<digest> objects;
    std::vector<sync_item> items;
    std::vector<sync_ref_outcome> outcomes;
    bool atomic{};
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
// One frame of a chunk upload, parsed but not decompressed. `compressed` points into the request.
struct pack_frame {
    digest id;
    std::uint32_t raw_size{};
    std::span<const std::byte> compressed;
};

// Validates every frame header of a chunk upload without decompressing anything. Each frame's
// declared uncompressed length, repeats included, is charged against a 64 MiB request budget, so
// a request over it is refused before any decoded byte is allocated.
[[nodiscard]] auto parse_pack_frames(std::span<const std::byte> bytes)
    -> result<std::vector<pack_frame>>;
// Decompresses one parsed frame and verifies its digest.
[[nodiscard]] auto decode_pack_frame(const pack_frame& frame) -> result<std::vector<std::byte>>;

} // namespace liquid::sync_detail
