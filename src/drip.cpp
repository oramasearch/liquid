#include "liquid/drip.hpp"

namespace liquid {
namespace {

template <class T>
result<T> unavailable(const char* issue) {
    return std::unexpected(error{errc::unsupported, "not implemented", issue});
}

}  // namespace

struct subscription::impl {};

subscription::subscription() noexcept = default;
subscription::~subscription() = default;
subscription::subscription(subscription&&) noexcept = default;
subscription& subscription::operator=(subscription&&) noexcept = default;

struct capture::impl {};

capture::capture() noexcept = default;
capture::~capture() = default;
capture::capture(capture&&) noexcept = default;
capture& capture::operator=(capture&&) noexcept = default;

result<subscription> capture::subscribe(std::function<void(const drip&)>) {
    return unavailable<subscription>("liquid #5");
}

runtime::task<void> capture::pause() {
    co_return unavailable<void>("liquid #48");
}

runtime::task<void> capture::resume() {
    co_return unavailable<void>("liquid #48");
}

result<capture_status> capture::status() {
    return unavailable<capture_status>("liquid #48");
}

}  // namespace liquid
