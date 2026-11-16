// SPDX-License-Identifier: MIT

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "liquid/store.hpp"

namespace liquid::store_detail {

// The persistence an injected owner's store writes through (liquid #327). It wraps the backend's
// fenced view and refuses every mutation with errc::locked once the grant is lost, and every read
// too unless the caller opted in with store_options::read_after_owner_lost. A refusal of the
// fence by the remote (errc::locked from the fenced view) revokes the grant, so the store refuses
// later work without asking the remote again.
class LIQUID_EXPORT owned_store_backend final : public ::liquid::store_backend {
public:
    owned_store_backend(std::shared_ptr<::liquid::store_backend> inner,
                        owner_grant grant,
                        bool read_after_loss);

    runtime::task<void> put(std::string key, std::vector<std::byte> bytes) override;
    runtime::task<void> put_if_absent(std::string key, std::vector<std::byte> bytes) override;
    runtime::task<void>
    put_range(std::string key, std::uint64_t offset, std::vector<std::byte> bytes) override;
    runtime::task<std::vector<std::byte>>
    get_range(std::string key, std::uint64_t offset, std::uint64_t size) override;
    runtime::task<void> remove(std::string key) override;
    runtime::task<std::vector<store_backend_entry>> list(std::string prefix) override;
    runtime::task<void> barrier() override;
    result<std::shared_ptr<::liquid::store_backend>> fenced(std::uint64_t fence) const override;

    [[nodiscard]] const std::shared_ptr<::liquid::store_backend>& inner() const noexcept;

private:
    [[nodiscard]] result<void> admit(bool mutation, const std::string& key) const;
    template <class T> [[nodiscard]] result<T> observe(result<T> outcome) const;

    std::shared_ptr<::liquid::store_backend> inner_;
    owner_grant grant_;
    bool read_after_loss_{};
};

// The owner an open store holds. An injected owner that is still held when the store is destroyed
// without close() has its grant revoked and its release started on the runtime; the disk owner the
// store creates for exclusive_owner is released synchronously by its own destructor.
class owner_hold {
public:
    owner_hold() noexcept = default;
    owner_hold(std::shared_ptr<::liquid::store_owner> owner, owner_grant grant, bool injected);
    ~owner_hold();
    owner_hold(owner_hold&& other) noexcept;
    owner_hold& operator=(owner_hold&& other) noexcept;
    owner_hold(const owner_hold&) = delete;
    owner_hold& operator=(const owner_hold&) = delete;

    // Revokes the grant and awaits the owner's release.
    [[nodiscard]] runtime::task<void> release();

private:
    void abandon() noexcept;

    std::shared_ptr<::liquid::store_owner> owner_;
    owner_grant grant_;
    bool injected_{};
};

} // namespace liquid::store_detail
