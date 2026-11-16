// SPDX-License-Identifier: MIT

#pragma once

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/hash.hpp"

namespace liquid::store_detail {

using reference_extractor = std::function<std::vector<digest>(std::span<const std::byte>)>;

struct extracted_references {
    std::string kind;
    std::vector<digest> digests;
    bool known{};
};

class reference_registry {
public:
    reference_registry();

    void register_kind(std::string kind, reference_extractor extractor);
    [[nodiscard]] extracted_references extract(std::string_view kind,
                                               std::span<const std::byte> bytes) const;

private:
    std::vector<std::pair<std::string, reference_extractor>> extractors_;
};

[[nodiscard]] std::vector<std::byte>
encode_chunk_list(std::uint64_t size, std::string_view key_id, std::span<const digest> chunks);
[[nodiscard]] std::string detect_object_kind(std::span<const std::byte> bytes);

} // namespace liquid::store_detail
