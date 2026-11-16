// SPDX-License-Identifier: MIT

#include "hydration.hpp"

#include <algorithm>
#include <fstream>
#include <numeric>
#include <system_error>

#include "repository/testing.hpp"

namespace liquid::vfs_detail {
namespace {

auto io_failure(std::string message, const std::filesystem::path& path) -> error {
    return {errc::io, std::move(message), path.string()};
}

auto same_chunks(const std::vector<digest>& left, const std::vector<digest>& right) -> bool {
    return std::ranges::equal(left, right, [](const digest& first, const digest& second) {
        return first.alg == second.alg && first.bytes == second.bytes;
    });
}

auto read_cached(const std::filesystem::path& path, std::uint64_t expected, digest id)
    -> result<std::optional<std::vector<std::byte>>> {
    if (id.alg == algorithm::blake3_keyed)
        return std::optional<std::vector<std::byte>>{};
    auto code = std::error_code{};
    if (!std::filesystem::exists(path, code)) {
        if (code)
            return std::unexpected(io_failure(code.message(), path));
        return std::optional<std::vector<std::byte>>{};
    }
    const auto size = std::filesystem::file_size(path, code);
    if (code)
        return std::unexpected(io_failure(code.message(), path));
    if (size != expected)
        return std::unexpected(
            error{errc::corrupt, "hydrated cache chunk has the wrong size", to_string(id)});
    auto input = std::ifstream{path, std::ios::binary};
    if (!input)
        return std::unexpected(io_failure("cannot open hydrated chunk", path));
    auto bytes = std::vector<std::byte>(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input)
        return std::unexpected(io_failure("cannot read hydrated chunk", path));
    auto actual = hash(id.alg, bytes);
    if (!actual || actual->alg != id.alg || actual->bytes != id.bytes)
        return std::unexpected(
            error{errc::corrupt, "hydrated cache chunk failed verification", to_string(id)});
    std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), code);
    return std::optional<std::vector<std::byte>>{std::move(bytes)};
}

auto trim_cache(const std::filesystem::path& root,
                const std::filesystem::path& keep,
                std::uint64_t capacity) -> result<void> {
    struct cached_file {
        std::filesystem::path path;
        std::uint64_t size{};
        std::filesystem::file_time_type used;
    };
    auto files = std::vector<cached_file>{};
    auto total = std::uint64_t{};
    auto code = std::error_code{};
    for (auto iterator = std::filesystem::directory_iterator{root, code};
         !code && iterator != std::filesystem::directory_iterator{};
         iterator.increment(code)) {
        if (!iterator->is_regular_file(code))
            continue;
        const auto size = iterator->file_size(code);
        const auto used = iterator->last_write_time(code);
        if (code)
            break;
        files.push_back({iterator->path(), size, used});
        total += size;
    }
    if (code)
        return std::unexpected(io_failure(code.message(), root));
    std::ranges::sort(files, {}, &cached_file::used);
    for (const auto& file : files) {
        if (total <= capacity)
            break;
        if (file.path == keep)
            continue;
        std::filesystem::remove(file.path, code);
        if (code)
            return std::unexpected(io_failure(code.message(), file.path));
        total -= file.size;
    }
    return {};
}

auto write_cached(const std::filesystem::path& root,
                  const std::filesystem::path& path,
                  std::span<const std::byte> bytes,
                  std::uint64_t capacity) -> result<void> {
    auto temporary = path;
    temporary += ".tmp";
    {
        auto output = std::ofstream{temporary, std::ios::binary | std::ios::trunc};
        if (!output)
            return std::unexpected(io_failure("cannot open hydrated chunk", temporary));
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output)
            return std::unexpected(io_failure("cannot write hydrated chunk", temporary));
    }
    auto code = std::error_code{};
    std::filesystem::rename(temporary, path, code);
    if (code) {
        std::filesystem::remove(path, code);
        code.clear();
        std::filesystem::rename(temporary, path, code);
    }
    if (code)
        return std::unexpected(io_failure(code.message(), path));
    return trim_cache(root, path, capacity);
}

} // namespace

hydration_cache::hydration_cache(std::filesystem::path root) : root_(std::move(root)) {}

runtime::task<void> hydration_cache::initialise() {
    auto created = co_await runtime::offload([root = root_]() -> result<void> {
        auto code = std::error_code{};
        std::filesystem::create_directories(root, code);
        if (code)
            return std::unexpected(io_failure(code.message(), root));
        return {};
    });
    if (!created)
        co_return std::unexpected(created.error());
    co_return std::move(*created);
}

auto hydration_cache::chunk_path(const digest& id) const -> std::filesystem::path {
    auto name = to_string(id);
    std::ranges::replace(name, ':', '-');
    return root_ / name;
}

void hydration_cache::record_chunk(const repository_detail::vfs_entry& file, std::size_t index) {
    auto lock = std::scoped_lock{mutex_};
    auto& state = files_[file.node];
    if (!same_chunks(state.chunks, file.chunks)) {
        state.chunks = file.chunks;
        state.chunk_sizes = file.chunk_sizes;
        state.hydrated.assign(file.chunks.size(), false);
    }
    if (state.hydrated[index]) {
        return;
    }
    const auto first = std::ranges::none_of(state.hydrated, std::identity{});
    state.hydrated[index] = true;
    status_.hydrated_bytes += file.chunk_sizes[index];
    if (first) {
        ++status_.hydrated_files;
    }
}

runtime::task<std::vector<std::byte>> hydration_cache::load_chunk(
    repository& source, digest content, digest id, std::uint64_t offset, std::uint64_t size) {
    const auto path = chunk_path(id);
    auto cached =
        co_await runtime::offload([path, size, id] { return read_cached(path, size, id); });
    if (!cached)
        co_return std::unexpected(cached.error());
    if (!*cached)
        co_return std::unexpected(cached->error());
    if (**cached)
        co_return std::move(***cached);

    auto bytes = std::vector<std::byte>(static_cast<std::size_t>(size));
    const auto content_is_valid = content.alg == algorithm::blake3 ||
                                  content.alg == algorithm::sha256 ||
                                  content.alg == algorithm::blake3_keyed;
    auto read = co_await repository_detail::vfs_access::read_chunk(
        source, content_is_valid ? content : id, content_is_valid ? offset : 0, bytes);
    if (!read)
        co_return std::unexpected(read.error());
    if (*read != size)
        co_return std::unexpected(
            error{errc::corrupt, "hydrated chunk length differs from metadata", to_string(id)});
    if (id.alg != algorithm::blake3_keyed) {
        auto stored =
            co_await runtime::offload([root = root_, path, bytes, cache_capacity = capacity] {
                return write_cached(root, path, bytes, cache_capacity);
            });
        if (!stored)
            co_return std::unexpected(stored.error());
        if (!*stored)
            co_return std::unexpected(stored->error());
    }
    {
        auto lock = std::scoped_lock{mutex_};
        ++status_.misses_served;
    }
    co_return bytes;
}

runtime::task<std::vector<std::byte>>
hydration_cache::read(repository& source,
                      const repository_detail::vfs_entry& file,
                      std::uint64_t offset,
                      std::size_t size) {
    if (file.kind != node_kind::blob)
        co_return std::unexpected(error{errc::invalid_argument, "path is not a file", file.name});
    if (file.chunks.size() != file.chunk_sizes.size())
        co_return std::unexpected(
            error{errc::corrupt, "blob chunk metadata is inconsistent", file.name});
    if (offset >= file.size || size == 0)
        co_return std::vector<std::byte>{};
    const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(size, file.size - offset));
    auto output = std::vector<std::byte>(wanted);
    auto position = std::uint64_t{};
    auto copied = std::size_t{};
    for (std::size_t index = 0; index < file.chunks.size(); ++index) {
        const auto chunk_size = file.chunk_sizes[index];
        const auto end = position + chunk_size;
        if (end > offset && copied < wanted) {
            auto chunk =
                co_await load_chunk(source, file.content, file.chunks[index], position, chunk_size);
            if (!chunk)
                co_return std::unexpected(chunk.error());
            const auto begin = static_cast<std::size_t>(std::max(position, offset) - position);
            const auto amount = static_cast<std::size_t>(std::min<std::uint64_t>(
                chunk_size - begin, static_cast<std::uint64_t>(wanted - copied)));
            std::copy_n(chunk->begin() + begin, amount, output.begin() + copied);
            copied += amount;
            record_chunk(file, index);
        }
        position = end;
        if (copied == wanted)
            break;
    }
    if (copied != wanted)
        co_return std::unexpected(
            error{errc::corrupt, "blob chunk metadata does not cover requested range", file.name});
    co_return output;
}

runtime::task<void> hydration_cache::hydrate(repository& source,
                                             const repository_detail::vfs_entry& file) {
    if (file.kind != node_kind::blob) {
        co_return std::unexpected(error{errc::invalid_argument, "path is not a file", file.name});
    }
    if (file.chunks.size() != file.chunk_sizes.size()) {
        co_return std::unexpected(
            error{errc::corrupt, "blob chunk metadata is inconsistent", file.name});
    }
    auto offset = std::uint64_t{};
    for (auto index = std::size_t{}; index < file.chunks.size(); ++index) {
        auto chunk = co_await load_chunk(
            source, file.content, file.chunks[index], offset, file.chunk_sizes[index]);
        if (!chunk) {
            co_return std::unexpected(chunk.error());
        }
        record_chunk(file, index);
        offset += file.chunk_sizes[index];
    }
    co_return result<void>{};
}

void hydration_cache::invalidate(node_id node) {
    auto lock = std::scoped_lock{mutex_};
    const auto found = files_.find(node);
    if (found == files_.end())
        return;
    status_.hydrated_bytes -= std::inner_product(found->second.chunk_sizes.begin(),
                                                 found->second.chunk_sizes.end(),
                                                 found->second.hydrated.begin(),
                                                 std::uint64_t{});
    --status_.hydrated_files;
    files_.erase(found);
}

auto hydration_cache::status() const -> mount_status {
    auto lock = std::scoped_lock{mutex_};
    return status_;
}

} // namespace liquid::vfs_detail

namespace liquid::repository_detail {

runtime::task<vfs_read_result> testing::cached_vfs_read(repository& value,
                                                        std::string path,
                                                        std::filesystem::path cache_path,
                                                        std::uint64_t offset,
                                                        std::size_t size) {
    auto file = co_await vfs_access::entry(value, std::move(path));
    if (!file)
        co_return std::unexpected(file.error());
    auto cache = vfs_detail::hydration_cache{std::move(cache_path)};
    auto initialised = co_await cache.initialise();
    if (!initialised)
        co_return std::unexpected(initialised.error());
    auto bytes = co_await cache.read(value, *file, offset, size);
    if (!bytes)
        co_return std::unexpected(bytes.error());
    co_return vfs_read_result{std::move(*bytes), cache.status()};
}

} // namespace liquid::repository_detail
