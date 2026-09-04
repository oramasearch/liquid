#include "liquid/repository.hpp"

namespace liquid {
namespace {

template <class T>
result<T> unavailable(const char* issue) {
    return std::unexpected(error{errc::unsupported, "not implemented", issue});
}

}  // namespace

struct repository::impl {};

repository::repository() noexcept = default;
repository::~repository() = default;
repository::repository(repository&&) noexcept = default;
repository& repository::operator=(repository&&) noexcept = default;

result<repository> repository::create(const std::filesystem::path&, const repository_options&) {
    return unavailable<repository>("liquid #2");
}

result<repository> repository::open(const std::filesystem::path&) {
    return unavailable<repository>("liquid #2");
}

result<tree> repository::root() { return unavailable<tree>("liquid #2"); }

result<change_id> repository::head() { return unavailable<change_id>("liquid #2"); }

result<std::vector<std::byte>> repository::read(std::string_view) {
    return unavailable<std::vector<std::byte>>("liquid #2");
}

result<void> repository::write(std::string_view, std::span<const std::byte>) {
    return unavailable<void>("liquid #2");
}

result<std::vector<change>> repository::log(const log_options&) {
    return unavailable<std::vector<change>>("liquid #2");
}

result<snapshot> repository::at(const change_id&) {
    return unavailable<snapshot>("liquid #2");
}

result<void> repository::nest(const repository&, std::string_view) {
    return unavailable<void>("liquid #2");
}

result<repository> repository::split(std::string_view, const std::filesystem::path&) {
    return unavailable<repository>("liquid #2");
}

result<void> repository::combine(const repository&, std::optional<std::string_view>) {
    return unavailable<void>("liquid #2");
}

result<std::vector<conflict>> repository::conflicts() {
    return unavailable<std::vector<conflict>>("liquid #40");
}

result<void> repository::undo(const op_id&) { return unavailable<void>("liquid #41"); }

result<void> repository::lock(std::string_view) { return unavailable<void>("liquid #42"); }

result<void> repository::unlock(std::string_view) {
    return unavailable<void>("liquid #42");
}

}  // namespace liquid
