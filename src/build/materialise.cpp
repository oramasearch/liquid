#include "build/materialise.hpp"

#include <fstream>

#include "build/repository_access.hpp"
#include "objects/records.hpp"

namespace liquid::build_detail {
namespace {

auto filesystem_failure(std::string message, const std::filesystem::path& path) -> error {
    return {errc::io, std::move(message), path.string()};
}

auto prepare_directory(const std::filesystem::path& path) -> result<void> {
    auto code = std::error_code{};
    std::filesystem::create_directories(path, code);
    if (code)
        return std::unexpected(filesystem_failure(code.message(), path));
    return {};
}

auto write_file(const std::filesystem::path& path,
                std::span<const std::byte> contents,
                std::uint32_t mode) -> result<void> {
    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
    if (!output)
        return std::unexpected(filesystem_failure("cannot create toolchain file", path));
    output.write(reinterpret_cast<const char*>(contents.data()),
                 static_cast<std::streamsize>(contents.size()));
    output.close();
    if (!output)
        return std::unexpected(filesystem_failure("cannot write toolchain file", path));
    auto code = std::error_code{};
    std::filesystem::permissions(path,
                                 static_cast<std::filesystem::perms>(mode & 0777),
                                 std::filesystem::perm_options::replace,
                                 code);
    if (code)
        return std::unexpected(filesystem_failure(code.message(), path));
    return {};
}

runtime::task<void>
materialise_directory(repository& source, digest root, std::filesystem::path destination) {
    auto bytes = co_await repository_detail::build_access::get(source, root);
    if (!bytes)
        co_return std::unexpected(bytes.error());
    auto tree = objects::decode_tree(*bytes);
    if (!tree)
        co_return std::unexpected(tree.error());
    for (const auto& entry : tree->entries) {
        const auto target = destination / entry.name;
        if (entry.kind == node_kind::tree) {
            auto created =
                co_await runtime::offload([target] { return prepare_directory(target); });
            if (!created)
                co_return std::unexpected(created.error());
            if (!*created)
                co_return std::unexpected(created->error());
            auto nested = co_await materialise_directory(source, entry.ref, target);
            if (!nested)
                co_return std::unexpected(nested.error());
            continue;
        }
        if (entry.kind != node_kind::blob)
            co_return std::unexpected(error{errc::unsupported,
                                            "toolchain trees cannot contain repository entries",
                                            entry.name});
        auto blob_bytes = co_await repository_detail::build_access::get(source, entry.ref);
        if (!blob_bytes)
            co_return std::unexpected(blob_bytes.error());
        auto blob = objects::decode_blob(*blob_bytes);
        if (!blob)
            co_return std::unexpected(blob.error());
        auto contents = std::vector<std::byte>{};
        contents.reserve(static_cast<std::size_t>(blob->size));
        for (const auto& chunk : blob->chunks) {
            auto chunk_bytes = co_await repository_detail::build_access::get(source, chunk);
            if (!chunk_bytes)
                co_return std::unexpected(chunk_bytes.error());
            contents.insert(contents.end(), chunk_bytes->begin(), chunk_bytes->end());
        }
        if (contents.size() != blob->size)
            co_return std::unexpected(
                error{errc::corrupt, "toolchain blob size does not match chunks", entry.name});
        auto written =
            co_await runtime::offload([target, contents = std::move(contents), mode = entry.mode] {
                return write_file(target, contents, mode);
            });
        if (!written)
            co_return std::unexpected(written.error());
        if (!*written)
            co_return std::unexpected(written->error());
    }
    co_return result<void>{};
}

} // namespace

runtime::task<void>
materialise_tree(repository& source, digest root, std::filesystem::path destination) {
    auto created =
        co_await runtime::offload([destination] { return prepare_directory(destination); });
    if (!created)
        co_return std::unexpected(created.error());
    if (!*created)
        co_return std::unexpected(created->error());
    co_return co_await materialise_directory(source, root, std::move(destination));
}

} // namespace liquid::build_detail
