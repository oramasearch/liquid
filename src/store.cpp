#include "liquid/store.hpp"

#include <utility>

#include "store/store_impl.hpp"

namespace liquid {
namespace {

template <class T>
result<T> unavailable(const char* issue) {
    return std::unexpected(error{errc::unsupported, "not implemented", issue});
}

}  // namespace

struct lease::impl {};

lease::lease() noexcept = default;
lease::~lease() = default;
lease::lease(lease&&) noexcept = default;
lease& lease::operator=(lease&&) noexcept = default;

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
    if (!backend) co_return std::unexpected(backend.error());
    store output;
    output.impl_ = std::make_unique<impl>(std::move(*backend));
    co_return output;
}

runtime::task<digest> store::put(std::vector<std::byte> input) {
    if (!impl_) co_return std::unexpected(error{errc::invalid_argument, "store is not open", "store::put"});
    co_return co_await impl_->value->put(std::move(input));
}

runtime::task<std::vector<std::byte>> store::get(digest id) {
    if (!impl_) co_return std::unexpected(error{errc::invalid_argument, "store is not open", "store::get"});
    co_return co_await impl_->value->get(id);
}

runtime::task<std::uint64_t> store::read(digest id, std::uint64_t offset, std::span<std::byte> into) {
    if (!impl_) co_return std::unexpected(error{errc::invalid_argument, "store is not open", "store::read"});
    co_return co_await impl_->value->read(id, offset, into);
}

runtime::task<bool> store::contains(digest id) {
    if (!impl_) co_return std::unexpected(error{errc::invalid_argument, "store is not open", "store::contains"});
    co_return co_await impl_->value->contains(id);
}

runtime::task<lease> store::lease_root(digest, std::chrono::seconds) {
    co_return unavailable<lease>("liquid #8");
}

runtime::task<collection_report> store::collect(collection_options) {
    co_return unavailable<collection_report>("liquid #8");
}

}  // namespace liquid
