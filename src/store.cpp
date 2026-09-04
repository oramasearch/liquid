#include "liquid/store.hpp"

#include <exception>
#include <utility>

#include "store/store_impl.hpp"

namespace liquid {
namespace {

template <class T>
result<T> unavailable(const char* issue) {
    return std::unexpected(error{errc::unsupported, "not implemented", issue});
}

template <class T>
result<T> internal_error(const char* operation, const std::exception& exception) {
    return std::unexpected(error{errc::internal, exception.what(), operation});
}

template <class T>
result<T> unknown_error(const char* operation) {
    return std::unexpected(error{errc::internal, "unknown exception", operation});
}

template <class T>
result<T> unopened(const char* operation) {
    return std::unexpected(error{errc::invalid_argument, "store is not open", operation});
}

}  // namespace

struct lease::impl {};

lease::lease() noexcept = default;
lease::~lease() = default;
lease::lease(lease&&) noexcept = default;
lease& lease::operator=(lease&&) noexcept = default;

struct store::impl {
    std::unique_ptr<store_detail::store_impl> value;
};

store::store() noexcept = default;
store::~store() = default;
store::store(store&&) noexcept = default;
store& store::operator=(store&&) noexcept = default;

result<store> store::open(const std::filesystem::path& path, const store_options& options) {
    try {
        auto opened = store_detail::store_impl::open(path, options);
        if (!opened) return std::unexpected(opened.error());
        store output;
        output.impl_ = std::make_unique<impl>();
        output.impl_->value = std::move(*opened);
        return output;
    } catch (const std::exception& exception) {
        return internal_error<store>("store::open", exception);
    } catch (...) {
        return unknown_error<store>("store::open");
    }
}

result<digest> store::put(std::span<const std::byte> input) {
    if (!impl_) return unopened<digest>("store::put");
    try {
        return impl_->value->put(input);
    } catch (const std::exception& exception) {
        return internal_error<digest>("store::put", exception);
    } catch (...) {
        return unknown_error<digest>("store::put");
    }
}

result<std::vector<std::byte>> store::get(const digest& id) {
    if (!impl_) return unopened<std::vector<std::byte>>("store::get");
    try {
        return impl_->value->get(id);
    } catch (const std::exception& exception) {
        return internal_error<std::vector<std::byte>>("store::get", exception);
    } catch (...) {
        return unknown_error<std::vector<std::byte>>("store::get");
    }
}

result<std::uint64_t> store::read(
    const digest& id, std::uint64_t offset, std::span<std::byte> into) {
    if (!impl_) return unopened<std::uint64_t>("store::read");
    try {
        return impl_->value->read(id, offset, into);
    } catch (const std::exception& exception) {
        return internal_error<std::uint64_t>("store::read", exception);
    } catch (...) {
        return unknown_error<std::uint64_t>("store::read");
    }
}

result<bool> store::contains(const digest& id) {
    if (!impl_) return unopened<bool>("store::contains");
    try {
        return impl_->value->contains(id);
    } catch (const std::exception& exception) {
        return internal_error<bool>("store::contains", exception);
    } catch (...) {
        return unknown_error<bool>("store::contains");
    }
}

result<lease> store::lease_root(const digest&, std::chrono::seconds) {
    return unavailable<lease>("liquid #8");
}

result<collection_report> store::collect(const collection_options&) {
    return unavailable<collection_report>("liquid #8");
}

}  // namespace liquid
