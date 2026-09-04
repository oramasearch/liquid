#pragma once

#include <string>
#include <utility>
#include <vector>

#include "liquid/store.hpp"

namespace liquid::repository_detail {

struct store_access {
    [[nodiscard]] static auto blob_for(const store& source, digest id) -> result<blob> {
        return source.blob_for(id);
    }

    [[nodiscard]] static runtime::task<digest> put_plain(store& destination,
                                                         std::vector<std::byte> input) {
        return destination.put_plain(std::move(input));
    }

    [[nodiscard]] static runtime::task<digest>
    put_encrypted(store& destination, std::vector<std::byte> input, std::string key_id) {
        return destination.put_encrypted(std::move(input), std::move(key_id));
    }
};

} // namespace liquid::repository_detail
