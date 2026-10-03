#include "backend.hpp"

#include <algorithm>
#include <cerrno>
#include <format>
#include <limits>
#include <random>
#include <ranges>
#include <system_error>

#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/this_coro.hpp>

#include "../runtime/net.hpp"
#include "faults.hpp"

namespace liquid {
namespace {

error backend_error(errc code, std::string message, std::string context) {
    return {code, std::move(message), std::move(context)};
}

result<std::filesystem::path> checked_path(const std::filesystem::path& root,
                                           std::string_view key) {
    if (key.empty() || key.front() == '/' || key.find('\\') != std::string_view::npos) {
        return std::unexpected(
            backend_error(errc::invalid_argument, "invalid store backend key", std::string(key)));
    }
    const auto relative = std::filesystem::path(key).lexically_normal();
    if (relative.empty() || relative.is_absolute()) {
        return std::unexpected(
            backend_error(errc::invalid_argument, "invalid store backend key", std::string(key)));
    }
    for (const auto& component : relative) {
        if (component == "..") {
            return std::unexpected(backend_error(
                errc::invalid_argument, "store backend key escapes its root", std::string(key)));
        }
    }
    return root / relative;
}

class disk_store_backend final : public store_backend {
public:
    disk_store_backend(std::filesystem::path root, bool durable)
        : root_(std::move(root)), durable_(durable) {}

    runtime::task<void> put(std::string key, std::vector<std::byte> bytes) override;
    runtime::task<void> put_if_absent(std::string key, std::vector<std::byte> bytes) override;
    runtime::task<void>
    put_range(std::string key, std::uint64_t offset, std::vector<std::byte> bytes) override;
    runtime::task<std::vector<std::byte>>
    get_range(std::string key, std::uint64_t offset, std::uint64_t size) override;
    runtime::task<void> remove(std::string key) override;
    runtime::task<std::vector<store_backend_entry>> list(std::string prefix) override;
    runtime::task<void> barrier() override;

    result<std::filesystem::path> path(std::string_view key) const {
        return checked_path(root_, key);
    }

private:
    std::filesystem::path root_;
    bool durable_{};
};

runtime::task<void> write_path(std::filesystem::path path,
                               std::uint64_t offset,
                               std::vector<std::byte> bytes,
                               bool truncate,
                               bool sync) {
    try {
        auto prepared = co_await runtime::offload(
            [parent = path.parent_path()] { std::filesystem::create_directories(parent); });
        if (!prepared) {
            co_return std::unexpected(
                backend_error(errc::io, prepared.error().message, path.string()));
        }
        store_detail::visit_fault(
            {store_detail::fault_operation::write, store_detail::fault_phase::before, path});
        const auto executor = co_await boost::capy::this_coro::executor;
        boost::corosio::random_access_file file(executor.context());
        auto flags = boost::corosio::file_base::read_write | boost::corosio::file_base::create;
        if (truncate) {
            flags |= boost::corosio::file_base::truncate;
        }
        file.open(path, flags);
        auto written = std::size_t{};
        while (written < bytes.size()) {
            auto [ec, count] = co_await file.write_some_at(
                offset + written,
                boost::capy::const_buffer(bytes.data() + written, bytes.size() - written));
            if (ec || count == 0) {
                co_return std::unexpected(backend_error(
                    errc::io, ec ? ec.message() : "short backend write", path.string()));
            }
            written += count;
        }
        if (sync) {
            file.sync_data();
        }
        file.close();
        store_detail::visit_fault(
            {store_detail::fault_operation::write, store_detail::fault_phase::after, path});
        co_return result<void>{};
    } catch (const std::system_error& caught) {
        co_return std::unexpected(backend_error(errc::io, caught.what(), path.string()));
    } catch (const std::exception& caught) {
        co_return std::unexpected(backend_error(errc::internal, caught.what(), path.string()));
    }
}

runtime::task<void> disk_store_backend::put(std::string key, std::vector<std::byte> bytes) {
    auto destination = path(key);
    if (!destination) {
        co_return std::unexpected(destination.error());
    }
    auto temporary = *destination;
    temporary += ".backend-tmp";
    auto written = co_await write_path(temporary, 0, std::move(bytes), true, durable_);
    if (!written) {
        co_return std::unexpected(written.error());
    }
    try {
        store_detail::visit_fault(
            {store_detail::fault_operation::rename, store_detail::fault_phase::before, temporary});
        auto renamed = co_await runtime::offload([temporary, destination = *destination] {
            std::filesystem::rename(temporary, destination);
        });
        if (!renamed) {
            co_return std::unexpected(
                backend_error(errc::io, renamed.error().message, destination->string()));
        }
        store_detail::visit_fault({store_detail::fault_operation::rename,
                                   store_detail::fault_phase::after,
                                   *destination});
        co_return result<void>{};
    } catch (const std::system_error& caught) {
        co_return std::unexpected(backend_error(errc::io, caught.what(), destination->string()));
    }
}

// The bytes are written to a temporary name unique to this call, then hard-linked to the key:
// link(2) refuses an existing destination atomically, so concurrent creators cannot both win and
// a reader never sees a partial object. The temporary carries the .backend-tmp suffix list skips.
runtime::task<void> disk_store_backend::put_if_absent(std::string key,
                                                      std::vector<std::byte> bytes) {
    auto destination = path(key);
    if (!destination) {
        co_return std::unexpected(destination.error());
    }
    auto random = std::random_device{};
    auto temporary = *destination;
    temporary += std::format(".{:08x}{:08x}.backend-tmp", random(), random());
    auto written = co_await write_path(temporary, 0, std::move(bytes), true, durable_);
    if (!written) {
        co_return std::unexpected(written.error());
    }
    try {
        store_detail::visit_fault(
            {store_detail::fault_operation::rename, store_detail::fault_phase::before, temporary});
        auto linked = co_await runtime::offload([temporary, destination = *destination] {
            auto code = std::error_code{};
            std::filesystem::create_hard_link(temporary, destination, code);
            auto ignored = std::error_code{};
            std::filesystem::remove(temporary, ignored);
            return code;
        });
        if (!linked) {
            co_return std::unexpected(
                backend_error(errc::io, linked.error().message, destination->string()));
        }
        if (*linked == std::errc::file_exists) {
            co_return std::unexpected(
                backend_error(errc::already_exists, "backend object already exists", key));
        }
        if (*linked) {
            co_return std::unexpected(
                backend_error(errc::io, linked->message(), destination->string()));
        }
        store_detail::visit_fault({store_detail::fault_operation::rename,
                                   store_detail::fault_phase::after,
                                   *destination});
        co_return result<void>{};
    } catch (const std::system_error& caught) {
        co_return std::unexpected(backend_error(errc::io, caught.what(), destination->string()));
    }
}

runtime::task<void>
disk_store_backend::put_range(std::string key, std::uint64_t offset, std::vector<std::byte> bytes) {
    auto destination = path(key);
    if (!destination) {
        co_return std::unexpected(destination.error());
    }
    auto written = co_await write_path(*destination, offset, std::move(bytes), false, durable_);
    if (!written) {
        co_return std::unexpected(written.error());
    }
    co_return result<void>{};
}

runtime::task<std::vector<std::byte>>
disk_store_backend::get_range(std::string key, std::uint64_t offset, std::uint64_t size) {
    auto source = path(key);
    if (!source) {
        co_return std::unexpected(source.error());
    }
    if (size > std::numeric_limits<std::size_t>::max()) {
        co_return std::unexpected(
            backend_error(errc::invalid_argument, "backend range is too large", key));
    }
    try {
        const auto executor = co_await boost::capy::this_coro::executor;
        boost::corosio::random_access_file file(executor.context());
        file.open(*source, boost::corosio::file_base::read_only);
        if (offset > file.size() || size > file.size() - offset) {
            co_return std::unexpected(
                backend_error(errc::corrupt, "backend range is outside the object", key));
        }
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        auto read = std::size_t{};
        while (read < bytes.size()) {
            auto [ec, count] = co_await file.read_some_at(
                offset + read,
                boost::capy::mutable_buffer(bytes.data() + read, bytes.size() - read));
            if (ec || count == 0) {
                co_return std::unexpected(
                    backend_error(errc::io, ec ? ec.message() : "short backend read", key));
            }
            read += count;
        }
        file.close();
        co_return bytes;
    } catch (const std::system_error& caught) {
        const auto code =
            caught.code() == std::errc::no_such_file_or_directory ? errc::not_found : errc::io;
        co_return std::unexpected(backend_error(code, caught.what(), key));
    }
}

runtime::task<void> disk_store_backend::remove(std::string key) {
    auto target = path(key);
    if (!target) {
        co_return std::unexpected(target.error());
    }
    try {
        store_detail::visit_fault(
            {store_detail::fault_operation::remove, store_detail::fault_phase::before, *target});
        auto removed = co_await runtime::offload(
            [target = *target] { return std::filesystem::remove(target); });
        if (!removed) {
            co_return std::unexpected(
                backend_error(errc::io, removed.error().message, target->string()));
        }
        if (!*removed) {
            co_return std::unexpected(
                backend_error(errc::not_found, "backend object was not found", key));
        }
        store_detail::visit_fault(
            {store_detail::fault_operation::remove, store_detail::fault_phase::after, *target});
        co_return result<void>{};
    } catch (const std::system_error& caught) {
        co_return std::unexpected(backend_error(errc::io, caught.what(), key));
    }
}

runtime::task<std::vector<store_backend_entry>> disk_store_backend::list(std::string prefix) {
    auto checked = prefix.empty() ? result<std::filesystem::path>{root_} : path(prefix);
    if (!checked) {
        co_return std::unexpected(checked.error());
    }
    try {
        auto listed = co_await runtime::offload([this, prefix] {
            std::vector<store_backend_entry> entries;
            const auto add = [&](const std::filesystem::directory_entry& entry) {
                if (!entry.is_regular_file()) {
                    return;
                }
                const auto key = entry.path().lexically_relative(root_).generic_string();
                if (key.ends_with(".backend-tmp") || !key.starts_with(prefix)) {
                    return;
                }
                entries.push_back({key, entry.file_size()});
            };
            // Every key with the prefix lies under the prefix's directory part, in an entry whose
            // name starts with its last component. Walk only those, so a backend rooted at a
            // working tree can answer for LIQUID_REPO without visiting the tree (liquid #326).
            const auto split = prefix.rfind('/');
            const auto directory = split == std::string::npos
                                       ? root_
                                       : root_ / std::filesystem::path(prefix.substr(0, split));
            const auto leading = split == std::string::npos
                                     ? std::string_view{prefix}
                                     : std::string_view{prefix}.substr(split + 1);
            if (!std::filesystem::is_directory(directory)) {
                return entries;
            }
            for (const auto& entry : std::filesystem::directory_iterator(directory)) {
                if (!entry.path().filename().generic_string().starts_with(leading)) {
                    continue;
                }
                if (entry.is_directory() && !entry.is_symlink()) {
                    for (const auto& nested :
                         std::filesystem::recursive_directory_iterator(entry.path())) {
                        add(nested);
                    }
                    continue;
                }
                add(entry);
            }
            std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
                return left.key < right.key;
            });
            return entries;
        });
        if (!listed) {
            co_return std::unexpected(
                backend_error(errc::io, listed.error().message, std::move(prefix)));
        }
        co_return std::move(*listed);
    } catch (const std::system_error& caught) {
        co_return std::unexpected(backend_error(errc::io, caught.what(), prefix));
    }
}

runtime::task<void> disk_store_backend::barrier() {
    co_return result<void>{};
}

} // namespace

runtime::task<void> store_backend::put_if_absent(std::string key, std::vector<std::byte>) {
    co_return std::unexpected(backend_error(
        errc::unsupported, "store backend does not implement a conditional create", key));
}

std::shared_ptr<store_backend> make_disk_store_backend(std::filesystem::path root, bool durable) {
    return std::make_shared<disk_store_backend>(std::move(root), durable);
}

namespace store_detail {

std::optional<std::filesystem::path>
backend_local_path(const std::shared_ptr<::liquid::store_backend>& backend, std::string_view key) {
    const auto disk = std::dynamic_pointer_cast<disk_store_backend>(backend);
    if (!disk) {
        return std::nullopt;
    }
    auto path = disk->path(key);
    if (!path) {
        return std::nullopt;
    }
    return *path;
}

runtime::task<bool> backend_exists(const std::shared_ptr<::liquid::store_backend>& backend,
                                   std::string key) {
    auto entries = co_await backend->list(key);
    if (!entries) {
        co_return std::unexpected(entries.error());
    }
    co_return std::ranges::any_of(*entries, [&key](const auto& entry) { return entry.key == key; });
}

runtime::task<std::uint64_t> backend_size(const std::shared_ptr<::liquid::store_backend>& backend,
                                          std::string key) {
    auto entries = co_await backend->list(key);
    if (!entries) {
        co_return std::unexpected(entries.error());
    }
    const auto found = std::ranges::find(*entries, key, &store_backend_entry::key);
    if (found == entries->end()) {
        co_return std::unexpected(
            backend_error(errc::not_found, "backend object was not found", key));
    }
    co_return found->size;
}

runtime::task<std::vector<std::byte>>
read_backend_object(const std::shared_ptr<::liquid::store_backend>& backend, std::string key) {
    auto size = co_await backend_size(backend, key);
    if (!size) {
        co_return std::unexpected(size.error());
    }
    co_return co_await backend->get_range(std::move(key), 0, *size);
}

runtime::task<void> remove_backend_prefix(const std::shared_ptr<::liquid::store_backend>& backend,
                                          std::string prefix) {
    auto entries = co_await backend->list(prefix);
    if (!entries) {
        co_return std::unexpected(entries.error());
    }
    for (const auto& entry : *entries) {
        auto removed = co_await backend->remove(entry.key);
        if (!removed && removed.error().code != errc::not_found) {
            co_return std::unexpected(removed.error());
        }
    }
    co_return result<void>{};
}

} // namespace store_detail
} // namespace liquid
