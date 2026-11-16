// SPDX-License-Identifier: MIT

#include "liquid/store.hpp"

#include <utility>

#include "store/store_impl.hpp"

namespace liquid {
namespace {

// A read that missed may have used a view another process has since changed (liquid #526): an
// object it published after this handle loaded, or a pack its collection or compaction rewrote.
bool retry_after_refresh(const error& failure) {
    return failure.code == errc::not_found || failure.code == errc::corrupt ||
           failure.code == errc::io;
}

// True when the handle reloaded its view, so the read is worth one more attempt.
runtime::task<bool> refreshed(store_detail::store_backend& backend) {
    auto stale = co_await backend.refresh_if_stale();
    co_return stale.value_or(false);
}

} // namespace

struct lease::impl {
    explicit impl(std::shared_ptr<store_detail::lease_state> value) : value(std::move(value)) {}
    std::shared_ptr<store_detail::lease_state> value;
};

lease::lease() noexcept = default;
lease::~lease() = default;
lease::lease(lease&&) noexcept = default;
lease& lease::operator=(lease&&) noexcept = default;

runtime::task<void> lease::renew(std::chrono::seconds ttl) {
    if (!impl_)
        co_return std::unexpected(
            error{errc::invalid_argument, "lease is not active", "lease::renew"});
    co_return co_await store_detail::renew_lease(impl_->value, ttl);
}

runtime::task<void> lease::release() {
    if (!impl_)
        co_return result<void>{};
    co_return co_await store_detail::release_lease(impl_->value);
}

struct store::impl {
    explicit impl(std::unique_ptr<store_detail::store_backend> value) : value(std::move(value)) {}
    std::unique_ptr<store_detail::store_backend> value;
};

store::store() noexcept = default;
store::~store() = default;
store::store(store&&) noexcept = default;
store& store::operator=(store&&) noexcept = default;

runtime::task<store> store::open(std::filesystem::path path, store_options options) {
    auto backend = co_await store_detail::store_backend::open(std::move(path), options);
    if (!backend)
        co_return std::unexpected(backend.error());
    store output;
    output.impl_ = std::make_unique<impl>(std::move(*backend));
    co_return output;
}

store_detail::store_backend* store::implementation() const noexcept {
    return impl_ ? impl_->value.get() : nullptr;
}

runtime::task<void> store::close() {
    if (!impl_)
        co_return result<void>{};
    auto value = std::move(impl_);
    auto owner = value->value->take_owner();
    value.reset();
    co_return co_await owner.release();
}

result<std::filesystem::path> store::root_path() const {
    if (!impl_) {
        return std::unexpected(
            error{errc::invalid_argument, "store is not open", "store::root_path"});
    }
    return impl_->value->root_path();
}

result<store_options> store::configuration() const {
    if (!impl_) {
        return std::unexpected(
            error{errc::invalid_argument, "store is not open", "store::configuration"});
    }
    return impl_->value->configuration();
}

runtime::task<digest> store::put(std::vector<std::byte> input) {
    if (!impl_)
        co_return std::unexpected(error{errc::invalid_argument, "store is not open", "store::put"});
    co_return co_await impl_->value->put(std::move(input));
}

runtime::task<digest> store::put_file(std::filesystem::path source) {
    if (!impl_)
        co_return std::unexpected(
            error{errc::invalid_argument, "store is not open", "store::put_file"});
    co_return co_await impl_->value->put_file(std::move(source));
}

runtime::task<void> store::get_to_file(digest id, std::filesystem::path destination) {
    if (!impl_)
        co_return std::unexpected(
            error{errc::invalid_argument, "store is not open", "store::get_to_file"});
    auto value = co_await impl_->value->get_to_file(id, destination);
    if (!value && retry_after_refresh(value.error()) &&
        (co_await refreshed(*impl_->value)).value_or(false)) {
        value = co_await impl_->value->get_to_file(id, std::move(destination));
    }
    co_return value;
}

std::vector<std::string> store::recoveries() const {
    return impl_ ? impl_->value->recoveries() : std::vector<std::string>{};
}

std::vector<std::string> store::recorded_recoveries() const {
    return impl_ ? impl_->value->recorded_recoveries() : std::vector<std::string>{};
}

result<blob> store::blob_for(digest id) const {
    if (!impl_) {
        return std::unexpected(
            error{errc::invalid_argument, "store is not open", "store::blob_for"});
    }
    return impl_->value->blob_for(id);
}

result<std::uint64_t> store::remote_bytes(digest blob_record) const {
    if (!impl_) {
        return std::unexpected(
            error{errc::invalid_argument, "store is not open", "store::remote_bytes"});
    }
    return impl_->value->remote_bytes(blob_record);
}

runtime::task<digest> store::put_plain(std::vector<std::byte> input) {
    if (!impl_) {
        co_return std::unexpected(error{errc::invalid_argument, "store is not open", "store::put"});
    }
    co_return co_await impl_->value->put_plain(std::move(input));
}

runtime::task<digest> store::put_plain(std::vector<std::byte> input, std::string delta_scope) {
    if (!impl_) {
        co_return std::unexpected(error{errc::invalid_argument, "store is not open", "store::put"});
    }
    co_return co_await impl_->value->put_plain(std::move(input), std::move(delta_scope));
}

runtime::task<digest> store::put_plain(std::vector<std::byte> input, algorithm hash_algorithm) {
    if (!impl_) {
        co_return std::unexpected(error{errc::invalid_argument, "store is not open", "store::put"});
    }
    co_return co_await impl_->value->put_plain(std::move(input), hash_algorithm);
}

runtime::task<digest> store::put_verified(digest id, std::vector<std::byte> input) {
    if (!impl_) {
        co_return std::unexpected(
            error{errc::invalid_argument, "store is not open", "store::put_verified"});
    }
    auto actual = hash(id.alg, input);
    if (!actual || actual->alg != id.alg || actual->bytes != id.bytes) {
        co_return std::unexpected(
            error{errc::corrupt, "object digest verification failed", to_string(id)});
    }
    co_return co_await impl_->value->put_plain(std::move(input), id.alg);
}

runtime::task<digest> store::put_encrypted(std::vector<std::byte> input, std::string key_id) {
    if (!impl_) {
        co_return std::unexpected(error{errc::invalid_argument, "store is not open", "store::put"});
    }
    co_return co_await impl_->value->put_encrypted(std::move(input), std::move(key_id));
}

void store::begin_object_batch() {
    if (impl_) {
        impl_->value->begin_object_batch();
    }
}

void store::abort_object_batch() {
    if (impl_) {
        impl_->value->abort_object_batch();
    }
}

runtime::task<void> store::commit_object_batch() {
    if (!impl_) {
        co_return std::unexpected(
            error{errc::invalid_argument, "store is not open", "store::commit_object_batch"});
    }
    co_return co_await impl_->value->commit_object_batch();
}

std::vector<digest> store::attestations(digest subject) const {
    return impl_ ? impl_->value->attestations(subject) : std::vector<digest>{};
}

std::vector<digest> store::object_ids(std::string_view kind) const {
    return impl_ ? impl_->value->object_ids(kind) : std::vector<digest>{};
}

runtime::task<std::vector<std::byte>> store::get(digest id) {
    if (!impl_)
        co_return std::unexpected(error{errc::invalid_argument, "store is not open", "store::get"});
    auto value = co_await impl_->value->get(id);
    if (!value && retry_after_refresh(value.error()) &&
        (co_await refreshed(*impl_->value)).value_or(false)) {
        value = co_await impl_->value->get(id);
    }
    co_return value;
}

runtime::task<std::uint64_t>
store::read(digest id, std::uint64_t offset, std::span<std::byte> into) {
    if (!impl_)
        co_return std::unexpected(
            error{errc::invalid_argument, "store is not open", "store::read"});
    auto value = co_await impl_->value->read(id, offset, into);
    if (!value && retry_after_refresh(value.error()) &&
        (co_await refreshed(*impl_->value)).value_or(false)) {
        value = co_await impl_->value->read(id, offset, into);
    }
    co_return value;
}

runtime::task<bool> store::contains(digest id) {
    if (!impl_)
        co_return std::unexpected(
            error{errc::invalid_argument, "store is not open", "store::contains"});
    auto value = co_await impl_->value->contains(id);
    if (value && !*value && (co_await refreshed(*impl_->value)).value_or(false)) {
        value = co_await impl_->value->contains(id);
    }
    co_return value;
}

runtime::task<blob_availability> store::has(digest id) {
    if (!impl_) {
        co_return std::unexpected(error{errc::invalid_argument, "store is not open", "store::has"});
    }
    auto value = co_await impl_->value->has(id);
    if (value && *value != blob_availability::local &&
        (co_await refreshed(*impl_->value)).value_or(false)) {
        value = co_await impl_->value->has(id);
    }
    co_return value;
}

runtime::task<void> store::register_remote_source(std::string id,
                                                  std::shared_ptr<remote_blob_source> source) {
    if (!impl_) {
        co_return std::unexpected(
            error{errc::invalid_argument, "store is not open", "store::register_remote_source"});
    }
    co_return co_await impl_->value->register_remote_source(std::move(id), std::move(source));
}

runtime::task<void> store::register_remote(blob value) {
    if (!impl_) {
        co_return std::unexpected(
            error{errc::invalid_argument, "store is not open", "store::register_remote"});
    }
    co_return co_await impl_->value->register_remote(std::move(value));
}

runtime::task<void> store::evict(digest id) {
    if (!impl_) {
        co_return std::unexpected(
            error{errc::invalid_argument, "store is not open", "store::evict"});
    }
    co_return co_await impl_->value->evict(id);
}

runtime::task<std::uint64_t> store::local_bytes() {
    if (!impl_) {
        co_return std::unexpected(
            error{errc::invalid_argument, "store is not open", "store::local_bytes"});
    }
    co_return co_await impl_->value->local_bytes();
}

runtime::task<std::optional<object_key_version>> store::key_version(digest id) {
    if (!impl_) {
        co_return std::unexpected(
            error{errc::invalid_argument, "store is not open", "store::key_version"});
    }
    co_return co_await impl_->value->key_version(id);
}

runtime::task<lease> store::lease_root(digest id, std::chrono::seconds ttl) {
    if (!impl_)
        co_return std::unexpected(
            error{errc::invalid_argument, "store is not open", "store::lease_root"});
    auto state = co_await impl_->value->lease_root(id, ttl);
    if (!state)
        co_return std::unexpected(state.error());
    lease output;
    output.impl_ = std::make_unique<lease::impl>(std::move(*state));
    co_return output;
}

runtime::task<lease> store::lease_root(digest id, std::chrono::seconds ttl, std::string owner) {
    if (!impl_) {
        co_return std::unexpected(
            error{errc::invalid_argument, "store is not open", "store::lease_root"});
    }
    auto state = co_await impl_->value->lease_root(id, ttl, std::move(owner));
    if (!state) {
        co_return std::unexpected(state.error());
    }
    lease output;
    output.impl_ = std::make_unique<lease::impl>(std::move(*state));
    co_return output;
}

runtime::task<collection_report> store::collect(collection_options options) {
    if (!impl_)
        co_return std::unexpected(
            error{errc::invalid_argument, "store is not open", "store::collect"});
    co_return co_await impl_->value->collect(options);
}

runtime::task<void> store::redact(std::vector<digest> ids) {
    if (!impl_)
        co_return std::unexpected(
            error{errc::invalid_argument, "store is not open", "store::redact"});
    co_return co_await impl_->value->redact(std::move(ids));
}

} // namespace liquid
