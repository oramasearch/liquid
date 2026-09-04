#include "liquid/store.hpp"

#include <utility>

#include "store/store_impl.hpp"

namespace liquid {
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
    co_return co_await impl_->value->get_to_file(id, std::move(destination));
}

result<blob> store::blob_for(digest id) const {
    if (!impl_) {
        return std::unexpected(
            error{errc::invalid_argument, "store is not open", "store::blob_for"});
    }
    return impl_->value->blob_for(id);
}

runtime::task<digest> store::put_plain(std::vector<std::byte> input) {
    if (!impl_) {
        co_return std::unexpected(error{errc::invalid_argument, "store is not open", "store::put"});
    }
    co_return co_await impl_->value->put_plain(std::move(input));
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

runtime::task<std::vector<std::byte>> store::get(digest id) {
    if (!impl_)
        co_return std::unexpected(error{errc::invalid_argument, "store is not open", "store::get"});
    co_return co_await impl_->value->get(id);
}

runtime::task<std::uint64_t>
store::read(digest id, std::uint64_t offset, std::span<std::byte> into) {
    if (!impl_)
        co_return std::unexpected(
            error{errc::invalid_argument, "store is not open", "store::read"});
    co_return co_await impl_->value->read(id, offset, into);
}

runtime::task<bool> store::contains(digest id) {
    if (!impl_)
        co_return std::unexpected(
            error{errc::invalid_argument, "store is not open", "store::contains"});
    co_return co_await impl_->value->contains(id);
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
