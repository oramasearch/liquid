#include "liquid/remote.hpp"

namespace liquid {

struct remote::impl {};

remote::remote() noexcept = default;
remote::~remote() = default;
remote::remote(remote&&) noexcept = default;
remote& remote::operator=(remote&&) noexcept = default;

}  // namespace liquid
