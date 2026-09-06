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
#include "liquid/remote.hpp"

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
    digest content{};
    std::optional<remote_blob_location> remote;
};

struct LIQUID_EXPORT change_id {
    digest value;
};

struct LIQUID_EXPORT repo_ref {
    node_id repo_id{};
    change_id id{};
};

struct LIQUID_EXPORT tree_entry {
    std::string name;
    std::uint32_t mode;
    node_kind kind;
    digest id;
    node_id node{};
    digest ref{};
    std::optional<repo_ref> nested;
};

struct LIQUID_EXPORT tree {
    std::vector<tree_entry> entries;
};

struct LIQUID_EXPORT change_ref {
    node_id repo_id{};
    change_id id{};
};

struct LIQUID_EXPORT conflict {
    std::string path;
    std::vector<digest> refs;
};

struct LIQUID_EXPORT document_nested_pointer {
    std::string path;
    repo_ref reference;
};

struct LIQUID_EXPORT document_state {
    std::map<std::string, change_id> refs;
    std::vector<std::string> locks;
    std::vector<document_nested_pointer> nested;
};

struct LIQUID_EXPORT document_heads {
    node_id directory{};
    std::vector<std::array<std::byte, 32>> values;
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
    document_state document;
    std::vector<document_heads> heads;
    std::uint64_t record_version{1};
};

} // namespace liquid
