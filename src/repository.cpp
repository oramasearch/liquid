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

runtime::task<repository> repository::create(std::filesystem::path, repository_options) {
    co_return unavailable<repository>("liquid #2");
}

runtime::task<repository> repository::open(std::filesystem::path) {
    co_return unavailable<repository>("liquid #2");
}

runtime::task<tree> repository::root() {
    co_return unavailable<tree>("liquid #2");
}

runtime::task<change_id> repository::head() {
    co_return unavailable<change_id>("liquid #2");
}

runtime::task<std::vector<std::byte>> repository::read(std::string) {
    co_return unavailable<std::vector<std::byte>>("liquid #2");
}

runtime::task<void> repository::write(std::string, std::vector<std::byte>) {
    co_return unavailable<void>("liquid #2");
}

runtime::task<std::vector<change>> repository::log(log_options) {
    co_return unavailable<std::vector<change>>("liquid #2");
}

runtime::task<snapshot> repository::at(change_id) {
    co_return unavailable<snapshot>("liquid #2");
}

runtime::task<void> repository::nest(repository, std::string) {
    co_return unavailable<void>("liquid #2");
}

runtime::task<repository> repository::split(std::string, std::filesystem::path) {
    co_return unavailable<repository>("liquid #2");
}

runtime::task<void> repository::combine(repository, std::optional<std::string>) {
    co_return unavailable<void>("liquid #2");
}

runtime::task<std::vector<conflict>> repository::conflicts() {
    co_return unavailable<std::vector<conflict>>("liquid #40");
}

runtime::task<void> repository::undo(op_id) {
    co_return unavailable<void>("liquid #41");
}

runtime::task<void> repository::lock(std::string) {
    co_return unavailable<void>("liquid #42");
}

runtime::task<void> repository::unlock(std::string) {
    co_return unavailable<void>("liquid #42");
}

}  // namespace liquid
