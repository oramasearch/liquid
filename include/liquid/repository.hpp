#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/export.hpp"
#include "liquid/object.hpp"
#include "liquid/runtime.hpp"
#include "liquid/store.hpp"

namespace liquid {

namespace repository_detail {
struct testing;
struct vfs_access;
} // namespace repository_detail

struct LIQUID_EXPORT repository_options {
    std::uint64_t checkpoint_interval{64};
    store_options store{};
};

struct LIQUID_EXPORT log_options {};

struct LIQUID_EXPORT ref {
    std::string name;
    change_id id;
};

struct LIQUID_EXPORT repository_status {
    std::uint64_t pending_drips{};
    bool paused{};
    std::vector<conflict> conflicts;
    std::vector<std::string> oversized_directories;
    std::optional<change_id> head;
};

struct LIQUID_EXPORT snapshot {
    change_id id;
    tree root;
};

struct LIQUID_EXPORT key_readability {
    std::string key_id;
    std::uint64_t version{};
    bool readable{};
};

struct LIQUID_EXPORT revision_readability {
    change_id revision;
    std::vector<key_readability> keys;
};

struct LIQUID_EXPORT readability_report {
    std::vector<revision_readability> revisions;
    std::vector<std::string> limits;
};

struct LIQUID_EXPORT op_id {
    digest value;
};

class LIQUID_EXPORT repository {
public:
    repository() noexcept;
    ~repository();
    repository(repository&&) noexcept;
    repository& operator=(repository&&) noexcept;
    repository(const repository&) = delete;
    repository& operator=(const repository&) = delete;

    [[nodiscard]] static runtime::task<repository> create(std::filesystem::path path,
                                                          repository_options options = {});
    [[nodiscard]] static runtime::task<repository> open(std::filesystem::path path,
                                                        repository_options options = {});
    [[nodiscard]] runtime::task<tree> root();
    [[nodiscard]] runtime::task<change_id> head();
    [[nodiscard]] runtime::task<std::vector<std::byte>> read(std::string path);
    [[nodiscard]] runtime::task<void> write(std::string path, std::vector<std::byte> contents);
    [[nodiscard]] runtime::task<void> write_file(std::string path, std::filesystem::path source);
    [[nodiscard]] runtime::task<void> redact(std::string path,
                                             std::optional<change_id> change = std::nullopt);
    [[nodiscard]] runtime::task<void> redact(std::string path,
                                             std::vector<std::byte> replacement,
                                             std::optional<change_id> change = std::nullopt);
    [[nodiscard]] runtime::task<void> move(std::string from, std::string to);
    [[nodiscard]] runtime::task<void> remove(std::string path);
    [[nodiscard]] runtime::task<change_id> cut(std::string message);
    [[nodiscard]] runtime::task<std::vector<ref>> refs();
    [[nodiscard]] runtime::task<void> set_ref(std::string name, change_id id);
    [[nodiscard]] result<repository_status> status();
    [[nodiscard]] runtime::task<std::vector<change>> log(log_options options = {});
    [[nodiscard]] runtime::task<snapshot> at(change_id id);
    [[nodiscard]] runtime::task<readability_report>
    readability(std::optional<change_id> revision = {});
    [[nodiscard]] runtime::task<void> nest(repository source, std::string path);
    [[nodiscard]] runtime::task<repository> split(std::string path, std::filesystem::path into);
    [[nodiscard]] runtime::task<void> combine(repository other, std::optional<std::string> at);
    [[nodiscard]] runtime::task<std::vector<conflict>> conflicts();
    [[nodiscard]] runtime::task<void> undo(op_id operation);
    [[nodiscard]] runtime::task<void> lock(std::string path);
    [[nodiscard]] runtime::task<void> unlock(std::string path);

private:
    [[nodiscard]] runtime::task<void>
    redact_impl(std::string path, std::vector<std::byte> replacement, digest target);

    friend struct repository_detail::testing;
    friend struct repository_detail::vfs_access;
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace liquid
