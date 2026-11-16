// SPDX-License-Identifier: MIT

#include "write_once_backend.hpp"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <utility>

namespace liquid::sim {
namespace {

runtime::task<std::optional<std::uint64_t>> object_size(std::shared_ptr<store_backend> backend,
                                                        std::string key) {
    auto entries = co_await backend->list(key);
    if (!entries) {
        co_return std::unexpected(entries.error());
    }
    for (const auto& entry : *entries) {
        if (entry.key == key) {
            co_return std::optional<std::uint64_t>{entry.size};
        }
    }
    co_return std::optional<std::uint64_t>{};
}

} // namespace

write_once_store_backend::write_once_store_backend(std::shared_ptr<store_backend> inner)
    : inner_(std::move(inner)), refused_(std::make_shared<std::atomic<std::uint64_t>>(0)) {}

write_once_store_backend::write_once_store_backend(
    std::shared_ptr<store_backend> inner, std::shared_ptr<std::atomic<std::uint64_t>> refused)
    : inner_(std::move(inner)), refused_(std::move(refused)) {}

std::uint64_t write_once_store_backend::refused() const noexcept {
    return refused_->load();
}

runtime::task<void> write_once_store_backend::put(std::string key, std::vector<std::byte> bytes) {
    co_return co_await inner_->put(std::move(key), std::move(bytes));
}

runtime::task<void> write_once_store_backend::put_if_absent(std::string key,
                                                            std::vector<std::byte> bytes) {
    co_return co_await inner_->put_if_absent(std::move(key), std::move(bytes));
}

runtime::task<void> write_once_store_backend::put_range(std::string key,
                                                        std::uint64_t offset,
                                                        std::vector<std::byte> bytes) {
    auto size = co_await object_size(inner_, key);
    if (!size) {
        co_return std::unexpected(size.error());
    }
    const auto end = size->value_or(0);
    if (offset == end) {
        co_return co_await inner_->put_range(std::move(key), offset, std::move(bytes));
    }
    if (offset < end && bytes.size() <= end - offset) {
        auto held = co_await inner_->get_range(key, offset, bytes.size());
        if (!held) {
            co_return std::unexpected(held.error());
        }
        if (*held == bytes) {
            co_return result<void>{};
        }
    }
    refused_->fetch_add(1);
    co_return std::unexpected(error{errc::conflict,
                                    "range at offset " + std::to_string(offset) +
                                        " does not append at the object size " +
                                        std::to_string(end),
                                    key});
}

runtime::task<std::vector<std::byte>>
write_once_store_backend::get_range(std::string key, std::uint64_t offset, std::uint64_t size) {
    co_return co_await inner_->get_range(std::move(key), offset, size);
}

runtime::task<void> write_once_store_backend::remove(std::string key) {
    co_return co_await inner_->remove(std::move(key));
}

runtime::task<std::vector<store_backend_entry>> write_once_store_backend::list(std::string prefix) {
    co_return co_await inner_->list(std::move(prefix));
}

runtime::task<void> write_once_store_backend::barrier() {
    co_return co_await inner_->barrier();
}

result<std::shared_ptr<store_backend>> write_once_store_backend::fenced(std::uint64_t fence) const {
    auto view = inner_->fenced(fence);
    if (!view) {
        return std::unexpected(view.error());
    }
    return std::make_shared<write_once_store_backend>(std::move(*view), refused_);
}

bool write_once_requested() noexcept {
    const auto* value = std::getenv(write_once_environment);
    return value != nullptr && *value != '\0' && std::string_view(value) != "0";
}

std::shared_ptr<store_backend> test_store_backend(const std::filesystem::path& root, bool durable) {
    if (!write_once_requested()) {
        return nullptr;
    }
    return std::make_shared<write_once_store_backend>(make_disk_store_backend(root, durable));
}

} // namespace liquid::sim
