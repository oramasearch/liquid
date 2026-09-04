#include "liquid/vfs.hpp"

#include "vfs/backend.hpp"

namespace liquid {

struct mount::impl {
    std::shared_ptr<vfs_detail::backend> backend;
};

mount::mount() noexcept = default;
mount::~mount() {
    if (impl_ && impl_->backend)
        impl_->backend->request_unmount();
}
mount::mount(mount&&) noexcept = default;
mount& mount::operator=(mount&& other) noexcept {
    if (this == &other)
        return *this;
    if (impl_ && impl_->backend)
        impl_->backend->request_unmount();
    impl_ = std::move(other.impl_);
    return *this;
}

runtime::task<void> mount::unmount() {
    if (!impl_ || !impl_->backend)
        co_return std::unexpected(error{errc::invalid_argument, "mount handle is empty", {}});
    co_return co_await impl_->backend->unmount();
}

result<mount_status> mount::status() const {
    if (!impl_ || !impl_->backend)
        return std::unexpected(error{errc::invalid_argument, "mount handle is empty", {}});
    return impl_->backend->status();
}

auto vfs_detail::mount_factory::make(std::shared_ptr<vfs_detail::backend> implementation)
    -> ::liquid::mount {
    auto output = ::liquid::mount{};
    output.impl_ = std::make_unique<::liquid::mount::impl>();
    output.impl_->backend = std::move(implementation);
    return output;
}

namespace vfs {

runtime::task<::liquid::mount>
mount(repository& source, std::filesystem::path mountpoint, mount_options options) {
    auto started = co_await vfs_detail::start_fuse(source, std::move(mountpoint), options);
    if (!started)
        co_return std::unexpected(started.error());
    co_return vfs_detail::mount_factory::make(std::move(*started));
}

} // namespace vfs

} // namespace liquid
