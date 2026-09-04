#include "liquid/identity.hpp"

namespace liquid {
namespace {

template <class T>
result<T> unavailable(const char* issue) {
    return std::unexpected(error{errc::unsupported, "not implemented", issue});
}

}  // namespace

struct signer::impl {};

signer::signer() noexcept = default;
signer::~signer() = default;
signer::signer(signer&&) noexcept = default;
signer& signer::operator=(signer&&) noexcept = default;

result<std::vector<std::byte>> signer::sign(std::span<const std::byte>) {
    return unavailable<std::vector<std::byte>>("liquid #4");
}

result<bool> verify(const actor&, std::span<const std::byte>, std::span<const std::byte>) {
    return unavailable<bool>("liquid #4");
}

struct key_provider::impl {};

key_provider::key_provider() noexcept = default;
key_provider::~key_provider() = default;
key_provider::key_provider(key_provider&&) noexcept = default;
key_provider& key_provider::operator=(key_provider&&) noexcept = default;

}  // namespace liquid
