// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "git/sha1.hpp"
#include "liquid/error.hpp"

namespace liquid::git_detail {

enum class object_kind : std::uint8_t { blob, tree, commit, tag };

struct object {
    object_kind kind;
    sha1 id;
    std::vector<std::byte> payload;
};

struct commit {
    sha1 id;
    sha1 tree;
    std::vector<sha1> parents;
    std::string author;
    std::int64_t author_time{};
    std::string message;
};

struct tree_entry {
    std::string name;
    std::uint32_t mode{};
    sha1 id;
};

struct pack_index_entry {
    std::uint64_t offset{};
    sha1 id{};
};

// Internal import limits; fixtures may lower them without allocating large objects.
struct object_read_limits {
    std::uint64_t object_bytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;
    std::uint64_t read_slack_bytes = 64ULL * 1024ULL * 1024ULL;
    unsigned delta_depth = 1024;
};

class object_store {
public:
    explicit object_store(std::filesystem::path directory, object_read_limits limits = {});

    [[nodiscard]] auto read(const sha1& id) -> result<object>;
    [[nodiscard]] auto read_commit(const sha1& id) -> result<commit>;
    [[nodiscard]] auto read_tree(const sha1& id) -> result<std::vector<tree_entry>>;
    [[nodiscard]] auto peel(const sha1& id) -> result<sha1>;
    [[nodiscard]] auto refs() -> result<std::map<std::string, sha1>>;
    [[nodiscard]] auto git_directory() const -> const std::filesystem::path&;

private:
    struct pack_stream {
        std::filesystem::path path;
        std::unique_ptr<std::ifstream> input;
    };
    struct cached_object {
        object value;
        std::list<std::string>::iterator position;
    };

    [[nodiscard]] auto stream_for(const std::filesystem::path& path) -> result<std::ifstream*>;

    std::filesystem::path directory_;
    object_read_limits limits_;
    std::map<std::string, cached_object> delta_cache_;
    std::list<std::string> delta_lru_;
    std::uint64_t delta_cache_bytes_{};
    std::filesystem::path indexed_pack_;
    std::vector<pack_index_entry> indexed_objects_;
    std::vector<pack_stream> streams_;
};

[[nodiscard]] auto git_directory(const std::filesystem::path& source)
    -> result<std::filesystem::path>;
[[nodiscard]] auto write_loose_object(const std::filesystem::path& directory,
                                      object_kind kind,
                                      std::span<const std::byte> payload) -> result<sha1>;

[[nodiscard]] auto kind_name(object_kind kind) -> std::string_view;

// The parsers of untrusted Git bytes, used by object_store and by the fuzz harnesses in fuzz/.
// parse_object takes an inflated loose object ("<kind> <size>\0<payload>") and checks it against
// its identifier; parse_commit and parse_tree read a payload; apply_delta applies a pack delta
// under the default object_read_limits (object_store applies deltas under its own read budget).
[[nodiscard]] auto parse_object(std::span<const std::byte> bytes, const sha1& id) -> result<object>;
[[nodiscard]] auto parse_commit(const object& value) -> result<commit>;
[[nodiscard]] auto parse_tree(const object& value) -> result<std::vector<tree_entry>>;
[[nodiscard]] auto apply_delta(std::span<const std::byte> base, std::span<const std::byte> delta)
    -> result<std::vector<std::byte>>;

} // namespace liquid::git_detail
