#pragma once

// A remote-shaped store backend for tests (liquid #327, #328): an object service with one owner
// lease whose fence rises with every holder, the backend view that carries a fence, and the owner
// that acquires the lease. Shared by the owner and the repository transfer tests.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "liquid/runtime.hpp"
#include "liquid/store.hpp"

namespace liquid::test {

// A remote object service shaped like Zetta's object capability (oramasearch/zetta#1005): objects
// plus one owner lease whose fence rises with every new holder. A mutation carries its client's
// fence and is refused with errc::locked unless that fence is the current holder's.
struct remote_service {
    std::mutex mutex;
    std::map<std::string, std::vector<std::byte>> objects;
    std::optional<std::string> holder;
    std::uint64_t fence{};
    std::uint64_t issued{};
    std::uint64_t accepted{};
    std::uint64_t refused{};
    std::uint64_t created{};
    // The fence carried by the last accepted write of each key; zero when it carried none.
    std::map<std::string, std::uint64_t> written_with;
    // The lease lapses once this many mutations have been accepted, as if renewal had failed.
    std::optional<std::uint64_t> expire_at;

    // Called with the mutex held.
    liquid::result<void> admit(const std::optional<std::uint64_t>& client, const std::string& key) {
        if (!client && !holder)
            return {};
        if (!holder || client != fence) {
            ++refused;
            return std::unexpected(
                liquid::error{liquid::errc::locked, "stale owner fence refused", key});
        }
        return {};
    }

    void mutated(const std::string& key, const std::optional<std::uint64_t>& client) {
        written_with.insert_or_assign(key, client.value_or(0));
        ++accepted;
        if (expire_at && accepted >= *expire_at) {
            holder.reset();
            expire_at.reset();
        }
    }
};

class remote_backend final : public liquid::store_backend {
public:
    explicit remote_backend(std::shared_ptr<remote_service> service,
                            std::optional<std::uint64_t> fence = std::nullopt)
        : service_(std::move(service)), fence_(fence) {}

    liquid::runtime::task<void> put(std::string key, std::vector<std::byte> bytes) override {
        auto lock = std::scoped_lock(service_->mutex);
        if (auto admitted = service_->admit(fence_, key); !admitted)
            co_return std::unexpected(admitted.error());
        service_->mutated(key, fence_);
        service_->objects.insert_or_assign(std::move(key), std::move(bytes));
        co_return liquid::result<void>{};
    }

    liquid::runtime::task<void> put_if_absent(std::string key,
                                              std::vector<std::byte> bytes) override {
        auto lock = std::scoped_lock(service_->mutex);
        if (auto admitted = service_->admit(fence_, key); !admitted)
            co_return std::unexpected(admitted.error());
        if (service_->objects.contains(key)) {
            co_return std::unexpected(
                liquid::error{liquid::errc::already_exists, "backend object already exists", key});
        }
        service_->mutated(key, fence_);
        ++service_->created;
        service_->objects.emplace(std::move(key), std::move(bytes));
        co_return liquid::result<void>{};
    }

    liquid::runtime::task<void>
    put_range(std::string key, std::uint64_t offset, std::vector<std::byte> bytes) override {
        auto lock = std::scoped_lock(service_->mutex);
        if (auto admitted = service_->admit(fence_, key); !admitted)
            co_return std::unexpected(admitted.error());
        auto& object = service_->objects[key];
        const auto end = static_cast<std::size_t>(offset) + bytes.size();
        if (object.size() < end)
            object.resize(end);
        std::copy(bytes.begin(), bytes.end(), object.begin() + static_cast<std::ptrdiff_t>(offset));
        service_->mutated(key, fence_);
        co_return liquid::result<void>{};
    }

    liquid::runtime::task<std::vector<std::byte>>
    get_range(std::string key, std::uint64_t offset, std::uint64_t size) override {
        auto lock = std::scoped_lock(service_->mutex);
        const auto found = service_->objects.find(key);
        if (found == service_->objects.end()) {
            co_return std::unexpected(
                liquid::error{liquid::errc::not_found, "backend object was not found", key});
        }
        if (offset > found->second.size() || size > found->second.size() - offset) {
            co_return std::unexpected(
                liquid::error{liquid::errc::corrupt, "backend range is outside the object", key});
        }
        const auto begin = found->second.begin() + static_cast<std::ptrdiff_t>(offset);
        co_return std::vector<std::byte>(begin, begin + static_cast<std::ptrdiff_t>(size));
    }

    liquid::runtime::task<void> remove(std::string key) override {
        auto lock = std::scoped_lock(service_->mutex);
        if (auto admitted = service_->admit(fence_, key); !admitted)
            co_return std::unexpected(admitted.error());
        if (service_->objects.erase(key) == 0) {
            co_return std::unexpected(
                liquid::error{liquid::errc::not_found, "backend object was not found", key});
        }
        service_->mutated(key, fence_);
        co_return liquid::result<void>{};
    }

    liquid::runtime::task<std::vector<liquid::store_backend_entry>>
    list(std::string prefix) override {
        auto lock = std::scoped_lock(service_->mutex);
        std::vector<liquid::store_backend_entry> entries;
        for (const auto& [key, bytes] : service_->objects) {
            if (key.starts_with(prefix))
                entries.push_back({key, bytes.size()});
        }
        co_return entries;
    }

    liquid::runtime::task<void> barrier() override {
        co_return liquid::result<void>{};
    }

    liquid::result<std::shared_ptr<liquid::store_backend>>
    fenced(std::uint64_t fence) const override {
        return std::make_shared<remote_backend>(service_, fence);
    }

private:
    std::shared_ptr<remote_service> service_;
    std::optional<std::uint64_t> fence_;
};

// The owner over the service's lease: acquire polls until the lease is free or the wait expires.
class remote_owner final : public liquid::store_owner {
public:
    explicit remote_owner(std::shared_ptr<remote_service> service) : service_(std::move(service)) {}

    liquid::runtime::task<liquid::owner_grant> acquire(std::string holder,
                                                       std::chrono::milliseconds wait) override {
        const auto deadline = std::chrono::steady_clock::now() + wait;
        for (;;) {
            {
                auto lock = std::scoped_lock(service_->mutex);
                if (!service_->holder) {
                    service_->holder = holder;
                    service_->fence = ++service_->issued;
                    grant_ = liquid::owner_grant{service_->fence};
                    co_return grant_;
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    co_return std::unexpected(liquid::error{
                        liquid::errc::locked, "store is owned by " + *service_->holder, "remote"});
                }
            }
            auto [delay_error] = co_await liquid::runtime::delay(std::chrono::milliseconds{5});
            if (delay_error) {
                co_return std::unexpected(
                    liquid::error{liquid::errc::unavailable, "cancelled", "remote"});
            }
        }
    }

    liquid::runtime::task<void> release() override {
        auto lock = std::scoped_lock(service_->mutex);
        if (service_->holder && grant_.valid() && service_->fence == grant_.fence())
            service_->holder.reset();
        ++releases;
        co_return liquid::result<void>{};
    }

    [[nodiscard]] const liquid::owner_grant& grant() const noexcept {
        return grant_;
    }

    std::uint64_t releases{};

private:
    std::shared_ptr<remote_service> service_;
    liquid::owner_grant grant_;
};

struct remote_store {
    liquid::store_options options;
    std::shared_ptr<remote_owner> owner;
};

inline remote_store remote_options(const std::shared_ptr<remote_service>& service,
                                   std::chrono::milliseconds wait = std::chrono::milliseconds{0}) {
    remote_store output;
    output.owner = std::make_shared<remote_owner>(service);
    output.options.backend = std::make_shared<remote_backend>(service);
    output.options.owner = output.owner;
    output.options.owner_wait = wait;
    return output;
}

} // namespace liquid::test
