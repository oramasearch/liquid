#include "liquid/vfs.hpp"

namespace liquid {

struct mount::impl {};

mount::mount() noexcept = default;
mount::~mount() = default;
mount::mount(mount&&) noexcept = default;
mount& mount::operator=(mount&&) noexcept = default;

}  // namespace liquid
