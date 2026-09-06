#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "liquid/repository.hpp"

namespace liquid::repository_detail {

struct staged_drive_file {
    tree_entry entry;
    digest content;
};

struct drive_access {
    [[nodiscard]] static auto fresh_node() -> node_id;
    [[nodiscard]] static auto objects(repository& source) -> store&;
    [[nodiscard]] static runtime::task<digest> snapshot(repository& source);
    [[nodiscard]] static runtime::task<staged_drive_file> stage(repository& destination,
                                                                std::filesystem::path source,
                                                                std::string name,
                                                                std::uint32_t mode);
    [[nodiscard]] static runtime::task<digest> content(repository& source, tree_entry entry);
    [[nodiscard]] static runtime::task<blob> load_blob(repository& source, tree_entry entry);
    [[nodiscard]] static runtime::task<void>
    write(repository& destination, std::string path, tree_entry entry);
    [[nodiscard]] static runtime::task<void> replace_conflicts(repository& destination,
                                                               std::vector<conflict> conflicts);
    [[nodiscard]] static runtime::task<bool> observe(repository& destination, repository& source);
    [[nodiscard]] static runtime::task<change_id>
    cut(repository& destination, repository& remote, std::string message);
};

} // namespace liquid::repository_detail
