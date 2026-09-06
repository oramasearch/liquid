#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "liquid/hash.hpp"
#include "liquid/runtime.hpp"
#include "liquid/transparency.hpp"

namespace {

namespace transparency = liquid::transparency;
using bytes = std::vector<std::byte>;

auto digest(std::span<const std::byte> value) -> transparency::hash {
    auto calculated = liquid::hash(liquid::algorithm::sha256, value);
    if (!calculated) {
        throw std::runtime_error(calculated.error().message);
    }
    return calculated->bytes;
}

auto leaf_hash(std::span<const std::byte> value) -> transparency::hash {
    auto input = bytes{};
    input.reserve(value.size() + 1);
    input.push_back(std::byte{0});
    input.insert(input.end(), value.begin(), value.end());
    return digest(input);
}

auto node_hash(const transparency::hash& left, const transparency::hash& right)
    -> transparency::hash {
    auto input = bytes{};
    input.reserve(1 + left.size() + right.size());
    input.push_back(std::byte{1});
    input.insert(input.end(), left.begin(), left.end());
    input.insert(input.end(), right.begin(), right.end());
    return digest(input);
}

auto append_text(bytes& output, std::string_view value) -> void {
    output.insert(output.end(),
                  reinterpret_cast<const std::byte*>(value.data()),
                  reinterpret_cast<const std::byte*>(value.data() + value.size()));
}

auto encoded_entry(std::uint64_t value) -> bytes {
    auto output = bytes(8);
    for (auto index = std::size_t{}; index < output.size(); ++index) {
        const auto shift = static_cast<unsigned>((output.size() - index - 1) * 8);
        output[index] = std::byte{static_cast<unsigned char>(value >> shift)};
    }
    return output;
}

auto entry_tile_bytes(std::uint64_t first,
                      std::uint64_t width,
                      std::vector<transparency::hash>& leaves) -> bytes {
    auto output = bytes{};
    append_text(output, "LAKE-TILE-1\n");
    for (auto offset = std::uint64_t{}; offset < width; ++offset) {
        const auto entry = encoded_entry(first + offset);
        output.push_back(std::byte{0});
        output.push_back(std::byte{0});
        output.push_back(std::byte{0});
        output.push_back(std::byte{8});
        output.insert(output.end(), entry.begin(), entry.end());
        leaves.push_back(leaf_hash(entry));
    }
    return output;
}

auto hash_tile_bytes(std::span<const transparency::hash> hashes) -> bytes {
    auto output = bytes{};
    append_text(output, "LAKE-HASH-TILE-1\n");
    for (const auto& value : hashes) {
        output.insert(output.end(), value.begin(), value.end());
    }
    return output;
}

class synthetic_source final : public transparency::log_source {
public:
    explicit synthetic_source(std::uint64_t leaf_count) {
        build(leaf_count);
    }

    [[nodiscard]] auto name() const -> std::string override {
        return "transparency measurement";
    }

    [[nodiscard]] auto identity() -> liquid::runtime::task<liquid::actor> override {
        co_return liquid::actor{};
    }

    [[nodiscard]] auto configuration()
        -> liquid::runtime::task<transparency::log_configuration> override {
        co_return transparency::log_configuration{};
    }

    [[nodiscard]] auto current_checkpoint()
        -> liquid::runtime::task<transparency::checkpoint> override {
        co_return checkpoint_;
    }

    [[nodiscard]] auto checkpoint() const -> const transparency::checkpoint& {
        return checkpoint_;
    }

    [[nodiscard]] auto get_tile(std::uint32_t level, std::uint64_t index, std::uint64_t width)
        -> liquid::runtime::task<transparency::tile> override {
        const auto found = std::ranges::find_if(tiles_, [=](const auto& candidate) {
            return candidate.level == level && candidate.index == index && candidate.width == width;
        });
        if (found == tiles_.end()) {
            co_return std::unexpected(liquid::error{
                liquid::errc::not_found, "measurement tile is missing", "transparency"});
        }
        ++source_reads;
        source_bytes += found->bytes.size();
        co_return *found;
    }

    [[nodiscard]] auto get_inclusion_proof(std::uint64_t, std::uint64_t)
        -> liquid::runtime::task<transparency::inclusion_proof> override {
        co_return std::unexpected(liquid::error{
            liquid::errc::unsupported, "measurement has no inclusion proof", "transparency"});
    }

    [[nodiscard]] auto get_consistency_proof(std::uint64_t, std::uint64_t)
        -> liquid::runtime::task<transparency::consistency_proof> override {
        co_return std::unexpected(liquid::error{
            liquid::errc::unsupported, "measurement has no consistency proof", "transparency"});
    }

    std::uint64_t source_reads{};
    std::uint64_t source_bytes{};

private:
    auto append_tile(std::uint32_t level, std::uint64_t index, std::uint64_t width, bytes encoded)
        -> void {
        tiles_.push_back(
            transparency::tile{level, index, width, digest(encoded), std::move(encoded)});
    }

    auto build(std::uint64_t leaf_count) -> void {
        auto level_values = std::vector<transparency::hash>{};
        level_values.reserve(static_cast<std::size_t>(leaf_count));
        for (auto first = std::uint64_t{}; first < leaf_count; first += transparency::tile_width) {
            const auto width =
                std::min<std::uint64_t>(transparency::tile_width, leaf_count - first);
            auto encoded = entry_tile_bytes(first, width, level_values);
            append_tile(0, first / transparency::tile_width, width, std::move(encoded));
        }
        for (auto level = std::uint32_t{1}; level_values.size() >= 2; ++level) {
            auto parents = std::vector<transparency::hash>{};
            parents.reserve(level_values.size() / 2);
            for (auto index = std::size_t{}; index + 1 < level_values.size(); index += 2) {
                parents.push_back(node_hash(level_values[index], level_values[index + 1]));
            }
            for (auto first = std::size_t{}; first < parents.size();
                 first += transparency::tile_width) {
                const auto width =
                    std::min<std::size_t>(transparency::tile_width, parents.size() - first);
                auto encoded = hash_tile_bytes(std::span{parents}.subspan(first, width));
                append_tile(level, first / transparency::tile_width, width, std::move(encoded));
            }
            level_values = std::move(parents);
        }
        if (level_values.size() != 1) {
            throw std::runtime_error("measurement leaf count must be a nonzero power of two");
        }
        checkpoint_.tree_size = leaf_count;
        checkpoint_.entry_tile_width = transparency::tile_width;
        checkpoint_.hash_tile_width = transparency::tile_width;
        checkpoint_.root_hash = level_values.front();
    }

    transparency::checkpoint checkpoint_;
    std::vector<transparency::tile> tiles_;
};

auto elapsed_microseconds(std::chrono::steady_clock::time_point start) -> std::uint64_t {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now() - start)
                                          .count());
}

} // namespace

int main() {
    constexpr auto leaf_count = std::uint64_t{1} << 16U;
    auto source = synthetic_source{leaf_count};
    auto client = transparency::log_client{source};

    const auto cold_start = std::chrono::steady_clock::now();
    auto cold = liquid::runtime::block_on(client.verify_tiles(source.checkpoint()));
    const auto cold_us = elapsed_microseconds(cold_start);
    if (!cold) {
        std::cerr << cold.error().message << '\n';
        return 1;
    }
    const auto cold_reads = source.source_reads;
    const auto cold_bytes = source.source_bytes;

    const auto cached_start = std::chrono::steady_clock::now();
    auto cached = liquid::runtime::block_on(client.verify_tiles(source.checkpoint()));
    const auto cached_us = elapsed_microseconds(cached_start);
    if (!cached) {
        std::cerr << cached.error().message << '\n';
        return 1;
    }

    std::cout << "transparency verification: leaves=" << leaf_count << " cold_us=" << cold_us
              << " cached_us=" << cached_us << " cold_source_reads=" << cold_reads
              << " cached_source_reads=" << source.source_reads - cold_reads
              << " source_bytes=" << cold_bytes << '\n';
    return 0;
}
