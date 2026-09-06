#pragma once

#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "formats/package/common.hpp"
#include "liquid/projection.hpp"

namespace liquid::formats::ostree {

enum class object_kind : std::uint8_t {
    file,
    dirtree,
    dirmeta,
    commit,
};

struct object {
    object_kind kind;
    digest checksum;
    std::vector<std::byte> bytes;
};

struct committed_file {
    std::string path;
    std::vector<std::byte> bytes;
    std::uint32_t mode;
    digest checksum;
    std::string component;
};

struct committed_directory {
    std::string path;
    std::uint32_t mode;
};

struct repository {
    digest commit;
    digest root_tree;
    digest root_meta;
    std::vector<object> objects;
    std::vector<committed_file> files;
    std::vector<committed_directory> directories;
    std::vector<std::byte> summary;
    std::string ref;
    std::uint64_t source_date_epoch{};
};

[[nodiscard]] auto build(std::span<const package::file> files,
                         const std::map<std::string, std::uint32_t>& directory_modes,
                         const std::map<std::string, std::string>& components,
                         std::string ref,
                         std::uint64_t source_date_epoch) -> result<repository>;
[[nodiscard]] auto write(const repository& value,
                         const std::filesystem::path& output,
                         ostree_repository_mode mode) -> result<void>;
[[nodiscard]] auto object_suffix(object_kind kind) -> std::string_view;

} // namespace liquid::formats::ostree
