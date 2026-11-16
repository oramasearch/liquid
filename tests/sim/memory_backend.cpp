// SPDX-License-Identifier: MIT

#include "memory_backend.hpp"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <utility>

#include "faults.hpp"

namespace liquid::sim {

struct memory_store_backend::state {
    std::mutex mutex;
    std::map<std::string, std::vector<std::byte>> objects;
    std::optional<error> failure;
    std::chrono::milliseconds latency{};
    bool frozen{};
};

memory_store_backend::memory_store_backend(std::chrono::milliseconds latency)
    : state_(std::make_shared<state>()) {
    state_->latency = latency;
}

void memory_store_backend::set_latency(std::chrono::milliseconds latency) noexcept {
    auto lock = std::scoped_lock(state_->mutex);
    state_->latency = latency;
}

void memory_store_backend::fail_next(error failure) {
    auto lock = std::scoped_lock(state_->mutex);
    state_->failure = std::move(failure);
}

runtime::task<void> memory_store_backend::wait() {
    auto latency = std::chrono::milliseconds{};
    {
        auto lock = std::scoped_lock(state_->mutex);
        latency = state_->latency;
    }
    if (latency != std::chrono::milliseconds::zero()) {
        auto [delay_error] = co_await runtime::delay(latency);
        if (delay_error) {
            co_return std::unexpected(
                error{errc::unavailable, delay_error.message(), "memory store backend latency"});
        }
    }
    co_return result<void>{};
}

std::optional<error> memory_store_backend::take_failure() {
    auto lock = std::scoped_lock(state_->mutex);
    return std::exchange(state_->failure, std::nullopt);
}

void memory_store_backend::set_frozen(bool frozen) noexcept {
    auto lock = std::scoped_lock(state_->mutex);
    state_->frozen = frozen;
}

std::optional<error> memory_store_backend::take_mutation_failure(const std::string& key) {
    if (auto failure = take_failure()) {
        return failure;
    }
    auto lock = std::scoped_lock(state_->mutex);
    if (state_->frozen) {
        return error{errc::unavailable, "memory store backend is frozen", key};
    }
    return std::nullopt;
}

runtime::task<void> memory_store_backend::put(std::string key, std::vector<std::byte> bytes) {
    auto waited = co_await wait();
    if (!waited) {
        co_return std::unexpected(waited.error());
    }
    if (auto failure = take_mutation_failure(key)) {
        co_return std::unexpected(std::move(*failure));
    }
    const auto context = std::filesystem::path(key);
    store_detail::visit_fault(
        {store_detail::fault_operation::write, store_detail::fault_phase::before, context});
    {
        auto lock = std::scoped_lock(state_->mutex);
        state_->objects.insert_or_assign(std::move(key), std::move(bytes));
    }
    store_detail::visit_fault(
        {store_detail::fault_operation::write, store_detail::fault_phase::after, context});
    co_return result<void>{};
}

runtime::task<void> memory_store_backend::put_if_absent(std::string key,
                                                        std::vector<std::byte> bytes) {
    auto waited = co_await wait();
    if (!waited) {
        co_return std::unexpected(waited.error());
    }
    if (auto failure = take_mutation_failure(key)) {
        co_return std::unexpected(std::move(*failure));
    }
    const auto context = std::filesystem::path(key);
    store_detail::visit_fault(
        {store_detail::fault_operation::write, store_detail::fault_phase::before, context});
    {
        auto lock = std::scoped_lock(state_->mutex);
        if (!state_->objects.try_emplace(key, std::move(bytes)).second) {
            co_return std::unexpected(
                error{errc::already_exists, "backend object already exists", key});
        }
    }
    store_detail::visit_fault(
        {store_detail::fault_operation::write, store_detail::fault_phase::after, context});
    co_return result<void>{};
}

std::vector<std::string> memory_store_backend::keys() const {
    auto lock = std::scoped_lock(state_->mutex);
    auto output = std::vector<std::string>{};
    output.reserve(state_->objects.size());
    for (const auto& [key, bytes] : state_->objects) {
        output.push_back(key);
    }
    return output;
}

runtime::task<void> memory_store_backend::put_range(std::string key,
                                                    std::uint64_t offset,
                                                    std::vector<std::byte> bytes) {
    auto waited = co_await wait();
    if (!waited) {
        co_return std::unexpected(waited.error());
    }
    if (auto failure = take_mutation_failure(key)) {
        co_return std::unexpected(std::move(*failure));
    }
    if (offset > std::numeric_limits<std::size_t>::max() ||
        bytes.size() > std::numeric_limits<std::size_t>::max() - offset) {
        co_return std::unexpected(
            error{errc::invalid_argument, "memory backend range is too large", key});
    }
    const auto context = std::filesystem::path(key);
    store_detail::visit_fault(
        {store_detail::fault_operation::write, store_detail::fault_phase::before, context});
    {
        auto lock = std::scoped_lock(state_->mutex);
        auto& object = state_->objects[key];
        if (object.size() < offset) {
            object.resize(static_cast<std::size_t>(offset));
        }
        const auto end = static_cast<std::size_t>(offset) + bytes.size();
        if (object.size() < end) {
            object.resize(end);
        }
        std::copy(bytes.begin(), bytes.end(), object.begin() + static_cast<std::size_t>(offset));
    }
    store_detail::visit_fault(
        {store_detail::fault_operation::write, store_detail::fault_phase::after, context});
    co_return result<void>{};
}

runtime::task<std::vector<std::byte>>
memory_store_backend::get_range(std::string key, std::uint64_t offset, std::uint64_t size) {
    auto waited = co_await wait();
    if (!waited) {
        co_return std::unexpected(waited.error());
    }
    if (auto failure = take_failure()) {
        co_return std::unexpected(std::move(*failure));
    }
    auto lock = std::scoped_lock(state_->mutex);
    const auto found = state_->objects.find(key);
    if (found == state_->objects.end()) {
        co_return std::unexpected(error{errc::not_found, "backend object was not found", key});
    }
    if (offset > found->second.size() || size > found->second.size() - offset) {
        co_return std::unexpected(error{errc::corrupt, "backend range is outside the object", key});
    }
    const auto begin = static_cast<std::size_t>(offset);
    const auto end = begin + static_cast<std::size_t>(size);
    co_return std::vector<std::byte>(found->second.begin() + begin, found->second.begin() + end);
}

runtime::task<void> memory_store_backend::remove(std::string key) {
    auto waited = co_await wait();
    if (!waited) {
        co_return std::unexpected(waited.error());
    }
    if (auto failure = take_mutation_failure(key)) {
        co_return std::unexpected(std::move(*failure));
    }
    const auto context = std::filesystem::path(key);
    store_detail::visit_fault(
        {store_detail::fault_operation::remove, store_detail::fault_phase::before, context});
    {
        auto lock = std::scoped_lock(state_->mutex);
        if (state_->objects.erase(key) == 0) {
            co_return std::unexpected(error{errc::not_found, "backend object was not found", key});
        }
    }
    store_detail::visit_fault(
        {store_detail::fault_operation::remove, store_detail::fault_phase::after, context});
    co_return result<void>{};
}

runtime::task<std::vector<store_backend_entry>> memory_store_backend::list(std::string prefix) {
    auto waited = co_await wait();
    if (!waited) {
        co_return std::unexpected(waited.error());
    }
    if (auto failure = take_failure()) {
        co_return std::unexpected(std::move(*failure));
    }
    auto entries = std::vector<store_backend_entry>{};
    auto lock = std::scoped_lock(state_->mutex);
    for (const auto& [key, bytes] : state_->objects) {
        if (key.starts_with(prefix)) {
            entries.push_back({key, bytes.size()});
        }
    }
    co_return entries;
}

runtime::task<void> memory_store_backend::barrier() {
    auto waited = co_await wait();
    if (!waited) {
        co_return std::unexpected(waited.error());
    }
    if (auto failure = take_failure()) {
        co_return std::unexpected(std::move(*failure));
    }
    co_return result<void>{};
}

} // namespace liquid::sim
