#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "liquid/export.hpp"
#include "liquid/hash.hpp"
#include "liquid/identity.hpp"

namespace liquid {

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
};

struct LIQUID_EXPORT tree_entry {
    std::string name;
    std::uint32_t mode;
    node_kind kind;
    digest id;
};

struct LIQUID_EXPORT tree {
    std::vector<tree_entry> entries;
};

struct LIQUID_EXPORT change_id {
    digest value;
};

struct LIQUID_EXPORT conflict {
    std::string path;
};

struct LIQUID_EXPORT change {
    change_id id;
    std::vector<change_id> parents;
    actor author;
    std::string message;
    std::chrono::sys_seconds at;
    digest root;
    std::optional<liquid::conflict> conflict;
};

}  // namespace liquid
