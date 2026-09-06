#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/export.hpp"
#include "liquid/format_abi.h"
#include "liquid/hash.hpp"

namespace liquid {

enum class format_tier : std::uint8_t {
    canonical_round_trip = 1,
    semantic_verbatim = 2,
    chunk_only = 3,
};

struct LIQUID_EXPORT format_limits {
    std::uint32_t maximum_depth{8};
    std::uint64_t byte_budget{256ULL * 1024ULL * 1024ULL};
    std::uint64_t maximum_entries{100'000};
};

struct LIQUID_EXPORT format_descriptor {
    std::string name;
    format_tier tier{format_tier::chunk_only};
    std::vector<std::string> extensions;
    std::vector<std::vector<std::byte>> magic;
    std::vector<std::string> volatile_fields;
    format_limits limits;
};

struct LIQUID_EXPORT format_recognition {
    format_tier tier{format_tier::chunk_only};
    std::string reason;
};

enum class format_entry_kind : std::uint8_t {
    file = 1,
    directory = 2,
    dataset = 3,
    table = 4,
    member = 5,
    attribute = 6,
};

struct format_tree;

struct LIQUID_EXPORT format_byte_range {
    std::uint64_t offset{};
    std::uint64_t size{};
};

struct LIQUID_EXPORT format_entry {
    std::string path;
    format_entry_kind kind{format_entry_kind::file};
    std::uint64_t size{};
    digest content;
    std::vector<std::byte> attributes;
    std::vector<std::byte> bytes;
    std::optional<format_byte_range> source_range;
    std::shared_ptr<format_tree> nested;
    std::string opaque_reason;
};

struct LIQUID_EXPORT format_tree {
    std::string codec;
    format_tier tier{format_tier::chunk_only};
    std::vector<format_entry> entries;
    std::vector<std::byte> original;
    bool truncated{};
    std::vector<std::string> limits;
};

enum class format_diff_kind : std::uint8_t {
    added = 1,
    removed = 2,
    modified = 3,
};

struct format_diff;

struct LIQUID_EXPORT format_diff_entry {
    std::string path;
    format_diff_kind kind{format_diff_kind::modified};
    std::optional<digest> old_digest;
    std::optional<digest> new_digest;
    std::shared_ptr<format_diff> nested;
};

struct LIQUID_EXPORT format_diff {
    std::string codec;
    format_tier tier{format_tier::chunk_only};
    std::vector<format_diff_entry> entries;
    bool truncated{};
};

struct LIQUID_EXPORT format_callbacks {
    std::function<void(std::string_view, std::uint64_t, std::uint64_t)> progress;
    std::function<bool()> cancelled;
};

struct LIQUID_EXPORT format_diff_options {
    double absolute_tolerance{};
    double relative_tolerance{};
};

class LIQUID_EXPORT format_codec {
public:
    format_codec() noexcept;
    ~format_codec();
    format_codec(format_codec&&) noexcept;
    auto operator=(format_codec&&) noexcept -> format_codec&;
    format_codec(const format_codec&) = delete;
    auto operator=(const format_codec&) -> format_codec& = delete;

    static auto load(std::filesystem::path shared_object) -> result<format_codec>;
    static auto linked(const liquid_format_module_v1* descriptor) -> result<format_codec>;

    [[nodiscard]] auto descriptor() const -> const format_descriptor&;
    [[nodiscard]] auto recognise(std::span<const std::byte> blob,
                                 std::string_view path = {},
                                 format_callbacks callbacks = {}) const
        -> result<std::optional<format_recognition>>;
    [[nodiscard]] auto unpack(std::span<const std::byte> blob,
                              format_limits limits = {},
                              format_callbacks callbacks = {}) const -> result<format_tree>;
    [[nodiscard]] auto canonicalise(const format_tree& tree, format_callbacks callbacks = {}) const
        -> result<format_tree>;
    [[nodiscard]] auto diff(const format_tree& old_tree,
                            const format_tree& new_tree,
                            format_callbacks callbacks = {}) const -> result<format_diff>;
    [[nodiscard]] auto diff(const format_tree& old_tree,
                            const format_tree& new_tree,
                            format_diff_options options,
                            format_callbacks callbacks = {}) const -> result<format_diff>;
    [[nodiscard]] auto merge(const format_tree& base,
                             const format_tree& left,
                             const format_tree& right,
                             format_callbacks callbacks = {}) const -> result<format_tree>;
    [[nodiscard]] auto repack(const format_tree& tree, format_callbacks callbacks = {}) const
        -> result<std::vector<std::byte>>;

private:
    struct impl;
    explicit format_codec(std::unique_ptr<impl> implementation);
    std::unique_ptr<impl> impl_;
};

class LIQUID_EXPORT format_registry {
public:
    format_registry();
    ~format_registry();
    format_registry(format_registry&&) noexcept;
    auto operator=(format_registry&&) noexcept -> format_registry&;
    format_registry(const format_registry&) = delete;
    auto operator=(const format_registry&) -> format_registry& = delete;

    [[nodiscard]] static auto global() -> format_registry&;

    auto add(format_codec codec) -> result<void>;
    auto load(std::filesystem::path shared_object) -> result<void>;
    [[nodiscard]] auto find_by_magic(std::span<const std::byte> blob) const
        -> std::vector<const format_codec*>;
    [[nodiscard]] auto find_by_extension(std::string_view path) const
        -> std::vector<const format_codec*>;
    [[nodiscard]] auto codecs() const -> std::vector<const format_codec*>;
    [[nodiscard]] auto recognise(std::span<const std::byte> blob, std::string_view path = {}) const
        -> result<std::pair<const format_codec*, format_recognition>>;
    [[nodiscard]] auto unpack(std::span<const std::byte> blob,
                              std::string_view path = {},
                              format_limits limits = {}) const -> result<format_tree>;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace liquid
