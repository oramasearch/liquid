// SPDX-License-Identifier: MIT

#include "reapi/uploads.hpp"

#include <algorithm>
#include <limits>

namespace liquid::reapi {
namespace {
auto upload_error(errc code, std::string message) -> error {
    return {code, std::move(message), {}};
}
} // namespace

auto validate_upload_limits(const reapi_upload_limits& limits) -> result<void> {
    if (limits.max_upload_bytes == 0 || limits.max_buffered_bytes == 0 || limits.max_uploads == 0 ||
        limits.max_resource_bytes == 0 || limits.idle_timeout <= std::chrono::milliseconds{} ||
        limits.max_upload_bytes >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return std::unexpected(upload_error(errc::invalid_argument, "invalid REAPI upload limits"));
    }
    return {};
}

void partial_uploads::erase(uploads::iterator entry) {
    buffered_ -= entry->second.capacity;
    uploads_.erase(entry);
}

void partial_uploads::expire_locked(clock::time_point now) {
    for (auto it = uploads_.begin(); it != uploads_.end();) {
        if (!it->second.active && std::chrono::duration_cast<std::chrono::milliseconds>(
                                      now - it->second.touched) >= limits_.idle_timeout) {
            auto expired = it++;
            erase(expired);
        } else {
            ++it;
        }
    }
}

void partial_uploads::expire(clock::time_point now) {
    auto lock = std::scoped_lock{mutex_};
    expire_locked(now);
}

auto partial_uploads::begin(std::string_view name,
                            std::int64_t expected_size,
                            clock::time_point now) -> result<std::unique_ptr<write>> {
    auto lock = std::scoped_lock{mutex_};
    expire_locked(now);
    if (name.empty() || name.size() > limits_.max_resource_bytes || expected_size < 0) {
        return std::unexpected(upload_error(errc::invalid_argument, "invalid upload resource"));
    }
    if (static_cast<std::uint64_t>(expected_size) > limits_.max_upload_bytes) {
        return std::unexpected(upload_error(errc::busy, "upload exceeds per-upload byte limit"));
    }
    auto it = uploads_.find(name);
    if (it != uploads_.end() && it->second.active) {
        return std::unexpected(upload_error(errc::busy, "upload already has an active Write"));
    }
    if (it == uploads_.end()) {
        if (uploads_.size() >= limits_.max_uploads) {
            return std::unexpected(upload_error(errc::busy, "active upload count limit exceeded"));
        }
        it = uploads_
                 .emplace(std::string{name},
                          upload{.expected_size = static_cast<std::size_t>(expected_size),
                                 .touched = now})
                 .first;
    }
    auto& pending = it->second;
    pending.generation = ++generation_;
    auto owned = std::unique_ptr<write>{new write{*this, it->first, pending.generation}};
    pending.active = true;
    return owned;
}

auto partial_uploads::committed(std::string_view name, clock::time_point now)
    -> result<std::int64_t> {
    auto lock = std::scoped_lock{mutex_};
    expire_locked(now);
    const auto it = uploads_.find(name);
    if (it == uploads_.end()) {
        return std::unexpected(upload_error(errc::not_found, "write resource is unknown"));
    }
    return static_cast<std::int64_t>(it->second.committed);
}

partial_uploads::write::~write() {
    if (retained_) {
        return;
    }
    auto lock = std::scoped_lock{owner_.mutex_};
    const auto it = owner_.uploads_.find(name_);
    if (it != owner_.uploads_.end() && it->second.generation == generation_) {
        owner_.erase(it);
    }
}

auto partial_uploads::write::append(std::int64_t offset,
                                    std::span<const std::byte> bytes,
                                    clock::time_point now) -> result<std::int64_t> {
    auto lock = std::scoped_lock{owner_.mutex_};
    auto& pending = owner_.uploads_.at(name_);
    if (retained_ || pending.generation != generation_ || pending.finished || offset < 0 ||
        static_cast<std::uint64_t>(offset) != pending.committed ||
        bytes.size() > pending.expected_size - pending.committed) {
        return std::unexpected(
            upload_error(errc::invalid_argument, "invalid upload offset or size"));
    }
    const auto needed = pending.committed + bytes.size();
    if (needed > pending.capacity) {
        // Charge the new allocation while the old one is still alive, including spare capacity.
        // Clamping growth lets small appends use the remaining budget without integer overflow.
        const auto available =
            std::min(owner_.limits_.max_buffered_bytes - owner_.buffered_, pending.expected_size);
        if (needed > available) {
            return std::unexpected(upload_error(errc::busy, "total upload byte limit exceeded"));
        }
        const auto growth =
            pending.capacity + std::min(pending.capacity, available - pending.capacity);
        const auto capacity = std::max(needed, growth);
        auto replacement = std::vector<std::byte>(capacity);
        if (replacement.capacity() > available) {
            return std::unexpected(upload_error(errc::busy, "total upload byte limit exceeded"));
        }
        std::ranges::copy(pending.data, replacement.begin());
        replacement.resize(pending.committed);
        owner_.buffered_ = owner_.buffered_ - pending.capacity + replacement.capacity();
        pending.capacity = replacement.capacity();
        pending.data = std::move(replacement);
    }
    pending.data.insert(pending.data.end(), bytes.begin(), bytes.end());
    pending.committed = needed;
    pending.touched = now;
    return static_cast<std::int64_t>(needed);
}

auto partial_uploads::write::finish() -> result<std::vector<std::byte>> {
    auto lock = std::scoped_lock{owner_.mutex_};
    auto& pending = owner_.uploads_.at(name_);
    if (retained_ || pending.generation != generation_ || pending.finished ||
        pending.committed != pending.expected_size) {
        return std::unexpected(
            upload_error(errc::invalid_argument, "upload does not match declared size"));
    }
    pending.finished = true;
    // Keep its capacity charged until storage and the response complete (or this Write is
    // cancelled).
    return std::move(pending.data);
}

void partial_uploads::write::retain(clock::time_point now) {
    auto lock = std::scoped_lock{owner_.mutex_};
    auto& pending = owner_.uploads_.at(name_);
    if (pending.generation != generation_ || retained_) {
        return;
    }
    if (pending.finished) {
        owner_.erase(owner_.uploads_.find(name_));
    } else {
        pending.active = false;
        pending.touched = now;
    }
    retained_ = true;
}

} // namespace liquid::reapi
