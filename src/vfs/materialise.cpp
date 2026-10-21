#include "liquid/vfs.hpp"

#include "fs/contained.hpp"
#include "objects/entry_name.hpp"
#include "vfs/repository_access.hpp"

namespace liquid::vfs {
namespace {

auto filesystem_error(std::string message, const std::filesystem::path& path) -> error {
    return {errc::io, std::move(message), path.string()};
}

auto prepare_target(const std::filesystem::path& target) -> result<void> {
    auto code = std::error_code{};
    if (std::filesystem::exists(target, code)) {
        if (code)
            return std::unexpected(filesystem_error(code.message(), target));
        if (!std::filesystem::is_directory(target, code))
            return std::unexpected(error{errc::already_exists,
                                         "materialisation target is not a directory",
                                         target.string()});
        if (!std::filesystem::is_empty(target, code))
            return std::unexpected(error{
                errc::already_exists, "materialisation target is not empty", target.string()});
    } else {
        std::filesystem::create_directories(target, code);
    }
    if (code)
        return std::unexpected(filesystem_error(code.message(), target));
    return {};
}

auto set_permissions(const std::filesystem::path& path, std::uint32_t mode) -> result<void> {
    auto code = std::error_code{};
    std::filesystem::permissions(path,
                                 static_cast<std::filesystem::perms>(mode & 0777),
                                 std::filesystem::perm_options::replace,
                                 code);
    if (code)
        return std::unexpected(filesystem_error(code.message(), path));
    return {};
}

// Materialised entries are created exclusively and without following a symbolic link (liquid
// #386): an existing entry means two names collide on this filesystem, by case or by Unicode
// normalisation, and is refused.
auto write_file(const std::filesystem::path& path,
                std::span<const std::byte> bytes,
                std::uint32_t mode) -> result<void> {
    return fs_detail::write_new_file(path, bytes, mode & 0777);
}

auto make_directory(const std::filesystem::path& path) -> result<void> {
    return fs_detail::make_new_directory(path);
}

auto joined(std::string_view parent, std::string_view name) -> std::string {
    return parent.empty() ? std::string{name} : std::string{parent} + "/" + std::string{name};
}

runtime::task<void>
materialise_directory(repository& source, std::string source_path, std::filesystem::path target) {
    auto entries = co_await repository_detail::vfs_access::children(source, source_path);
    if (!entries)
        co_return std::unexpected(entries.error());
    for (const auto& entry : *entries) {
        if (auto valid = objects::validate_entry_name(entry.name, errc::corrupt); !valid)
            co_return std::unexpected(valid.error());
        const auto child_source = joined(source_path, entry.name);
        const auto child_target = target / entry.name;
        if (entry.kind == node_kind::blob) {
            auto contents = co_await source.read(child_source);
            if (!contents)
                co_return std::unexpected(contents.error());
            auto written = co_await runtime::offload(
                [child_target, contents = std::move(*contents), mode = entry.mode] {
                    return write_file(child_target, contents, mode);
                });
            if (!written)
                co_return std::unexpected(written.error());
            if (!*written)
                co_return std::unexpected(written->error());
            continue;
        }
        auto created =
            co_await runtime::offload([child_target] { return make_directory(child_target); });
        if (!created)
            co_return std::unexpected(created.error());
        if (!*created)
            co_return std::unexpected(created->error());
        auto nested = co_await materialise_directory(source, child_source, child_target);
        if (!nested)
            co_return std::unexpected(nested.error());
        auto permissions = co_await runtime::offload([child_target, mode = entry.mode] {
            return fs_detail::set_mode(child_target, mode & 0777);
        });
        if (!permissions)
            co_return std::unexpected(permissions.error());
        if (!*permissions)
            co_return std::unexpected(permissions->error());
    }
    co_return result<void>{};
}

} // namespace

runtime::task<void> materialise(repository& source,
                                std::filesystem::path target,
                                std::optional<std::string> path_prefix) {
    auto prepared = co_await runtime::offload([target] { return prepare_target(target); });
    if (!prepared)
        co_return std::unexpected(prepared.error());
    if (!*prepared)
        co_return std::unexpected(prepared->error());
    const auto prefix = path_prefix.value_or("");
    if (prefix.empty()) {
        auto root = co_await repository_detail::vfs_access::entry(source, "");
        if (!root)
            co_return std::unexpected(root.error());
        auto complete = co_await materialise_directory(source, "", target);
        if (!complete)
            co_return std::unexpected(complete.error());
        auto permissions = co_await runtime::offload(
            [target, mode = root->mode] { return set_permissions(target, mode); });
        if (!permissions)
            co_return std::unexpected(permissions.error());
        if (!*permissions)
            co_return std::unexpected(permissions->error());
        co_return result<void>{};
    }
    auto selected = co_await repository_detail::vfs_access::entry(source, prefix);
    if (!selected)
        co_return std::unexpected(selected.error());
    if (selected->kind == node_kind::blob) {
        if (auto valid = objects::validate_entry_name(selected->name, errc::corrupt); !valid)
            co_return std::unexpected(valid.error());
        auto contents = co_await source.read(prefix);
        if (!contents)
            co_return std::unexpected(contents.error());
        auto written = co_await runtime::offload(
            [path = target / selected->name,
             contents = std::move(*contents),
             mode = selected->mode] { return write_file(path, contents, mode); });
        if (!written)
            co_return std::unexpected(written.error());
        if (!*written)
            co_return std::unexpected(written->error());
        co_return result<void>{};
    }
    auto complete = co_await materialise_directory(source, prefix, target);
    if (!complete)
        co_return std::unexpected(complete.error());
    auto permissions = co_await runtime::offload(
        [target, mode = selected->mode] { return set_permissions(target, mode); });
    if (!permissions)
        co_return std::unexpected(permissions.error());
    if (!*permissions)
        co_return std::unexpected(permissions->error());
    co_return result<void>{};
}

} // namespace liquid::vfs
