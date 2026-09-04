#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "liquid/build.hpp"
#include "liquid/error.hpp"
#include "liquid/object.hpp"

namespace liquid::objects {

struct repo_record {
    node_id repo_id{};
    std::string name;
    std::int64_t created{};
    std::optional<repo_ref> root_change;
};

struct action_record {
    action_input input;
    digest sandbox;
};

struct sandbox_record {
    std::string mechanism;
    std::string work_root;
    std::string toolchain_root;
    std::string home_root;
    std::string temporary_root;
    std::vector<std::string> fixed_environment;
    bool network{};
};

struct action_index_entry {
    digest candidate;
    digest result;
    std::uint64_t sequence{};
    bool divergent{};
    std::optional<digest> verification;
};

struct action_index_root {
    std::vector<action_index_entry> entries;
    std::vector<digest> traces;
};

[[nodiscard]] auto encode(const blob& value) -> result<std::vector<std::byte>>;
[[nodiscard]] auto encode(const tree& value) -> result<std::vector<std::byte>>;
[[nodiscard]] auto encode(const change& value) -> result<std::vector<std::byte>>;
[[nodiscard]] auto encode_change_body(const change& value) -> result<std::vector<std::byte>>;
[[nodiscard]] auto encode(const repo_record& value) -> result<std::vector<std::byte>>;
[[nodiscard]] auto encode(const attestation& value) -> result<std::vector<std::byte>>;
[[nodiscard]] auto encode(const action_record& value) -> result<std::vector<std::byte>>;
[[nodiscard]] auto encode(const action_result& value) -> result<std::vector<std::byte>>;
[[nodiscard]] auto encode_result_body(const action_result& value) -> result<std::vector<std::byte>>;
[[nodiscard]] auto encode(const sandbox_record& value) -> result<std::vector<std::byte>>;
[[nodiscard]] auto encode(const action_index_root& value) -> result<std::vector<std::byte>>;
[[nodiscard]] auto encode(const build_trace& value) -> result<std::vector<std::byte>>;
[[nodiscard]] auto encode_attestation_body(const attestation& value)
    -> result<std::vector<std::byte>>;
[[nodiscard]] auto encode_attestation_body_unchecked(const attestation& value)
    -> result<std::vector<std::byte>>;

[[nodiscard]] auto decode_blob(std::span<const std::byte> bytes) -> result<blob>;
[[nodiscard]] auto decode_tree(std::span<const std::byte> bytes) -> result<tree>;
[[nodiscard]] auto decode_change(std::span<const std::byte> bytes,
                                 std::optional<algorithm> hash_algorithm = std::nullopt)
    -> result<change>;
[[nodiscard]] auto decode_repo(std::span<const std::byte> bytes) -> result<repo_record>;
[[nodiscard]] auto decode_attestation(std::span<const std::byte> bytes) -> result<attestation>;
[[nodiscard]] auto decode_action(std::span<const std::byte> bytes) -> result<action_record>;
[[nodiscard]] auto decode_result(std::span<const std::byte> bytes) -> result<action_result>;
[[nodiscard]] auto decode_action_index_root(std::span<const std::byte> bytes)
    -> result<action_index_root>;
[[nodiscard]] auto decode_build_trace(std::span<const std::byte> bytes) -> result<build_trace>;
[[nodiscard]] auto digest_of(algorithm hash_algorithm, std::span<const std::byte> bytes)
    -> result<digest>;

} // namespace liquid::objects
