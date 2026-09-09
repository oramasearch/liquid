#include "conformance_backend.hpp"

#include <array>
#include <utility>

namespace liquid::test::vfs_conformance {
namespace {

runtime::task<std::shared_ptr<vfs_detail::backend>> mount_libfuse(mount_request request) {
    if (request.input_tree) {
        co_return co_await vfs_detail::start_observed_fuse(request.source,
                                                           *request.input_tree,
                                                           std::move(request.mountpoint),
                                                           std::move(request.outputs),
                                                           std::move(request.observe),
                                                           request.read_only);
    }
    co_return co_await vfs_detail::start_fuse(
        request.source,
        std::move(request.mountpoint),
        {.read_only = request.read_only, .attr_timeout = std::chrono::seconds{0}});
}

runtime::task<backend_instance> start_libfuse(mount_request request) {
    auto mounted = co_await mount_libfuse(std::move(request));
    if (!mounted) {
        co_return std::unexpected(mounted.error());
    }
#if !defined(_WIN32)
    co_return backend_instance{std::move(*mounted), posix_operations()};
#else
    co_return backend_instance{std::move(*mounted)};
#endif
}

} // namespace

auto backends() -> std::span<const backend_driver> {
    static const auto drivers = std::array{backend_driver{"direct", start_direct},
                                           backend_driver{"libfuse", start_libfuse}};
    return drivers;
}

const backend_driver* selected_backend{};
bool allow_unavailable{};

} // namespace liquid::test::vfs_conformance
