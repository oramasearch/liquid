#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/export.hpp"
#include "liquid/object.hpp"

namespace liquid {

struct LIQUID_EXPORT repository_options {};

struct LIQUID_EXPORT log_options {};

struct LIQUID_EXPORT snapshot {
    change_id id;
    tree root;
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

    [[nodiscard]] static result<repository> create(
        const std::filesystem::path& path, const repository_options& options = {});
    [[nodiscard]] static result<repository> open(const std::filesystem::path& path);
    [[nodiscard]] result<tree> root();
    [[nodiscard]] result<change_id> head();
    [[nodiscard]] result<std::vector<std::byte>> read(std::string_view path);
    [[nodiscard]] result<void> write(
        std::string_view path, std::span<const std::byte> contents);
    [[nodiscard]] result<std::vector<change>> log(const log_options& options = {});
    [[nodiscard]] result<snapshot> at(const change_id& id);
    [[nodiscard]] result<void> nest(const repository& source, std::string_view path);
    [[nodiscard]] result<repository> split(
        std::string_view path, const std::filesystem::path& into);
    [[nodiscard]] result<void> combine(
        const repository& other, std::optional<std::string_view> at);
    [[nodiscard]] result<std::vector<conflict>> conflicts();
    [[nodiscard]] result<void> undo(const op_id& operation);
    [[nodiscard]] result<void> lock(std::string_view path);
    [[nodiscard]] result<void> unlock(std::string_view path);

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

}  // namespace liquid
