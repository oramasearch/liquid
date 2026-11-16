// SPDX-License-Identifier: MIT

#include "build/materialise.hpp"

#include "build/repository_access.hpp"
#include "fs/contained.hpp"
#include "objects/entry_name.hpp"
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

runtime::task<void>
materialise_directory(repository& source, digest root, std::filesystem::path destination) {
    auto bytes = co_await repository_detail::build_access::get(source, root);
    if (!bytes)
        co_return std::unexpected(bytes.error());
    auto tree = objects::decode_tree(*bytes);
    if (!tree)
        co_return std::unexpected(tree.error());
    for (const auto& entry : tree->entries) {
        // The name is checked again here, after decoding, and every entry is created exclusively
        // without following a symbolic link (liquid #386).
        if (auto valid = objects::validate_entry_name(entry.name, errc::corrupt); !valid)
            co_return std::unexpected(valid.error());
        const auto target = destination / entry.name;
        if (entry.kind == node_kind::tree) {
            auto created = co_await runtime::offload(
                [target] { return fs_detail::make_new_directory(target); });
            if (!created)
                co_return std::unexpected(created.error());
            if (!*created)
                co_return std::unexpected(created->error());
            auto nested = co_await materialise_directory(source, entry.ref, target);
            if (!nested)
                co_return std::unexpected(nested.error());
            continue;
        }
        if (entry.kind == node_kind::conflict) {
            auto conflict_bytes = co_await repository_detail::build_access::get(source, entry.ref);
            if (!conflict_bytes) {
                co_return std::unexpected(conflict_bytes.error());
            }
            auto value = objects::decode_conflict(*conflict_bytes);
            if (!value) {
                co_return std::unexpected(value.error());
            }
            co_return std::unexpected(
                error{errc::conflict,
                      "build input contains unresolved conflict",
                      value->region.path + " (" + to_string(entry.ref) + ")"});
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
        if (blob->content.alg == algorithm::blake3 || blob->content.alg == algorithm::sha256 ||
            blob->content.alg == algorithm::blake3_keyed) {
            auto complete = co_await repository_detail::build_access::get(source, blob->content);
            if (!complete) {
                co_return std::unexpected(complete.error());
            }
            contents = std::move(*complete);
        } else {
            contents.reserve(static_cast<std::size_t>(blob->size));
            for (const auto& chunk : blob->chunks) {
                auto chunk_bytes = co_await repository_detail::build_access::get(source, chunk);
                if (!chunk_bytes)
                    co_return std::unexpected(chunk_bytes.error());
                contents.insert(contents.end(), chunk_bytes->begin(), chunk_bytes->end());
            }
        }
        if (contents.size() != blob->size)
            co_return std::unexpected(
                error{errc::corrupt, "toolchain blob size does not match chunks", entry.name});
        auto written =
            co_await runtime::offload([target, contents = std::move(contents), mode = entry.mode] {
                return fs_detail::write_new_file(target, contents, mode & 0777);
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
