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

result<store> store::open(const std::filesystem::path&, const store_options&) {
    return unavailable<store>("liquid #1");
}

result<digest> store::put(std::span<const std::byte>) {
    return unavailable<digest>("liquid #1");
}

result<std::vector<std::byte>> store::get(const digest&) {
    return unavailable<std::vector<std::byte>>("liquid #1");
}

result<std::uint64_t> store::read(const digest&, std::uint64_t, std::span<std::byte>) {
    return unavailable<std::uint64_t>("liquid #1");
}

result<bool> store::contains(const digest&) { return unavailable<bool>("liquid #1"); }

result<lease> store::lease_root(const digest&, std::chrono::seconds) {
    return unavailable<lease>("liquid #8");
}

result<collection_report> store::collect(const collection_options&) {
    return unavailable<collection_report>("liquid #8");
}

}  // namespace liquid
