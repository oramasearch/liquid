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

    [[nodiscard]] static auto remote_bytes(const store& source, digest id)
        -> result<std::uint64_t> {
        return source.remote_bytes(id);
    }

    [[nodiscard]] static auto register_remote(store& destination, blob value)
        -> runtime::task<void> {
        return destination.register_remote(std::move(value));
    }

    static void begin_object_batch(store& destination) {
        destination.begin_object_batch();
    }

    static void abort_object_batch(store& destination) {
        destination.abort_object_batch();
    }

    [[nodiscard]] static runtime::task<void> commit_object_batch(store& destination) {
        return destination.commit_object_batch();
    }

    [[nodiscard]] static runtime::task<digest> put_plain(store& destination,
                                                         std::vector<std::byte> input) {
        return destination.put_plain(std::move(input));
    }

    [[nodiscard]] static runtime::task<digest>
    put_plain(store& destination, std::vector<std::byte> input, algorithm hash_algorithm) {
        return destination.put_plain(std::move(input), hash_algorithm);
    }

    [[nodiscard]] static runtime::task<digest>
    put_encrypted(store& destination, std::vector<std::byte> input, std::string key_id) {
        return destination.put_encrypted(std::move(input), std::move(key_id));
    }

    [[nodiscard]] static std::vector<digest> attestations(const store& source, digest subject) {
        return source.attestations(subject);
    }
};

} // namespace liquid::repository_detail
