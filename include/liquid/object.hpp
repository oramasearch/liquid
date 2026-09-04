#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "liquid/attestation.hpp"
#include "liquid/export.hpp"
#include "liquid/hash.hpp"
#include "liquid/identity.hpp"

namespace liquid {

using node_id = std::array<std::byte, 16>;

enum class node_kind : std::uint8_t {
    blob,
    tree,
    change,
    action,
    result_,
    attestation,
    repo,
};

struct LIQUID_EXPORT blob {
    std::vector<digest> chunks;
    std::uint64_t size;
    std::vector<std::uint64_t> chunk_sizes;
};

struct LIQUID_EXPORT tree_entry {
    std::string name;
    std::uint32_t mode;
    node_kind kind;
    digest id;
    node_id node{};
    digest ref{};
};

struct LIQUID_EXPORT tree {
    std::vector<tree_entry> entries;
};

struct LIQUID_EXPORT change_id {
    digest value;
};

struct LIQUID_EXPORT change_ref {
    node_id repo_id{};
    change_id id{};
};

struct LIQUID_EXPORT repo_ref {
    node_id repo_id{};
    change_id id{};
};

struct LIQUID_EXPORT conflict {
    std::string path;
    std::vector<digest> refs;
};

struct LIQUID_EXPORT change {
    change_id id;
    std::vector<change_ref> parents;
    actor author;
    std::string message;
    std::chrono::sys_seconds at;
    digest root;
    std::map<std::string, std::string> metadata;
    std::optional<liquid::conflict> conflict;
    std::vector<liquid::conflict> conflicts;
    std::optional<digest> symbols;
    std::vector<std::byte> sig;
};

} // namespace liquid
