// SPDX-License-Identifier: MIT

#pragma once

// An in-memory store backend for the store harness and the seed generator (liquid #463). The
// pack and index readers take their bytes from a store_backend, so the harness puts the input
// there instead of writing it to disk. Every call completes without suspending.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "liquid/store.hpp"

namespace liquid::fuzz {

class memory_backend final : public store_backend {
public:
    runtime::task<void> put(std::string key, std::vector<std::byte> bytes) override {
        objects_[std::move(key)] = std::move(bytes);
        co_return result<void>{};
    }

    runtime::task<void>
    put_range(std::string key, std::uint64_t offset, std::vector<std::byte> bytes) override {
        auto& object = objects_[std::move(key)];
        if (object.size() < offset + bytes.size()) {
            object.resize(static_cast<std::size_t>(offset + bytes.size()));
        }
        std::copy(bytes.begin(), bytes.end(), object.begin() + static_cast<std::ptrdiff_t>(offset));
        co_return result<void>{};
    }

    runtime::task<std::vector<std::byte>>
    get_range(std::string key, std::uint64_t offset, std::uint64_t size) override {
        const auto found = objects_.find(key);
        if (found == objects_.end()) {
            co_return std::unexpected(error{errc::not_found, "object is not present", key});
        }
        const auto& object = found->second;
        if (offset > object.size() || size > object.size() - offset) {
            co_return std::unexpected(error{errc::io, "range is outside the object", key});
        }
        const auto first = object.begin() + static_cast<std::ptrdiff_t>(offset);
        co_return std::vector<std::byte>(first, first + static_cast<std::ptrdiff_t>(size));
    }

    runtime::task<void> remove(std::string key) override {
        if (objects_.erase(key) == 0) {
            co_return std::unexpected(error{errc::not_found, "object is not present", key});
        }
        co_return result<void>{};
    }

    runtime::task<std::vector<store_backend_entry>> list(std::string prefix) override {
        auto output = std::vector<store_backend_entry>{};
        for (const auto& [key, bytes] : objects_) {
            if (key.starts_with(prefix)) {
                output.push_back({key, bytes.size()});
            }
        }
        co_return output;
    }

    runtime::task<void> barrier() override {
        co_return result<void>{};
    }

    [[nodiscard]] auto object(const std::string& key) const -> std::vector<std::byte> {
        const auto found = objects_.find(key);
        return found == objects_.end() ? std::vector<std::byte>{} : found->second;
    }

    void clear() {
        objects_.clear();
    }

private:
    std::map<std::string, std::vector<std::byte>> objects_;
};

} // namespace liquid::fuzz
