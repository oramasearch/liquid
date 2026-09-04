#include "liquid/store.hpp"

#include <utility>

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

struct store::impl {};

store::store() noexcept = default;
store::~store() = default;
store::store(store&&) noexcept = default;
store& store::operator=(store&&) noexcept = default;

runtime::task<store> store::open(std::filesystem::path, store_options) {
    co_return unavailable<store>("liquid #1");
}

runtime::task<digest> store::put(std::vector<std::byte>) {
    co_return unavailable<digest>("liquid #1");
}

runtime::task<std::vector<std::byte>> store::get(digest) {
    co_return unavailable<std::vector<std::byte>>("liquid #1");
}

runtime::task<std::uint64_t> store::read(digest, std::uint64_t, std::span<std::byte>) {
    co_return unavailable<std::uint64_t>("liquid #1");
}

runtime::task<bool> store::contains(digest) {
    co_return unavailable<bool>("liquid #1");
}

runtime::task<lease> store::lease_root(digest, std::chrono::seconds) {
    co_return unavailable<lease>("liquid #8");
}

runtime::task<collection_report> store::collect(collection_options) {
    co_return unavailable<collection_report>("liquid #8");
}

}  // namespace liquid
