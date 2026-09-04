#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "liquid/remote.hpp"
#include "liquid/store.hpp"
#include "repository/store_access.hpp"

namespace liquid::test_support {

class on_disk_remote_source final : public remote_blob_source {
public:
    explicit on_disk_remote_source(store contents) : contents_(std::move(contents)) {}

    auto put(std::vector<std::byte> contents) -> result<blob> {
        auto stored = runtime::block_on(contents_.put(std::move(contents)));
        if (!stored) {
            return std::unexpected(stored.error());
        }
        return repository_detail::store_access::blob_for(contents_, *stored);
    }

    auto reads() const -> std::uint64_t {
        return reads_;
    }

    auto bytes_read() const -> std::uint64_t {
        return bytes_read_;
    }

    auto read_blobs() const -> const std::vector<digest>& {
        return read_blobs_;
    }

    auto store() -> liquid::store& {
        return contents_;
    }

    runtime::task<std::vector<digest>> find_missing(std::string instance,
                                                    std::vector<digest> blobs) override {
        if (instance != "worker-a") {
            co_return std::unexpected(
                error{errc::invalid_argument, "unknown remote instance", std::move(instance)});
        }
        auto missing = std::vector<digest>{};
        for (const auto& blob : blobs) {
            auto present = co_await contents_.has(blob);
            if (!present) {
                co_return std::unexpected(present.error());
            }
            if (*present == blob_availability::absent) {
                missing.push_back(blob);
            }
        }
        co_return missing;
    }

    runtime::task<std::uint64_t> read_range(std::string instance,
                                            digest blob,
                                            std::uint64_t offset,
                                            std::span<std::byte> output) override {
        if (instance != "worker-a") {
            co_return std::unexpected(
                error{errc::invalid_argument, "unknown remote instance", std::move(instance)});
        }
        ++reads_;
        read_blobs_.push_back(blob);
        auto read = co_await contents_.read(blob, offset, output);
        if (!read) {
            co_return std::unexpected(read.error());
        }
        bytes_read_ += *read;
        co_return *read;
    }

    runtime::task<void>
    write(std::string instance, digest blob, std::vector<std::byte> contents) override {
        if (instance != "worker-a") {
            co_return std::unexpected(
                error{errc::invalid_argument, "unknown remote instance", std::move(instance)});
        }
        auto stored = co_await contents_.put(std::move(contents));
        if (!stored) {
            co_return std::unexpected(stored.error());
        }
        if (stored->alg != blob.alg || stored->bytes != blob.bytes) {
            co_return std::unexpected(
                error{errc::corrupt, "remote write digest mismatch", to_string(blob)});
        }
        co_return result<void>{};
    }

private:
    store contents_;
    std::uint64_t reads_{};
    std::uint64_t bytes_read_{};
    std::vector<digest> read_blobs_;
};

} // namespace liquid::test_support
