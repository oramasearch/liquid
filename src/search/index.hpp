#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "liquid/daemon.hpp"
#include "liquid/hash.hpp"
#include "liquid/repository.hpp"

namespace liquid::search_detail {

struct document {
    std::string path;
    digest blob{};
    std::vector<std::byte> contents;
};

struct document_update {
    std::string path;
    std::optional<document> value;
};

struct index_statistics {
    std::uint64_t paths{};
    std::uint64_t unique_blobs{};
    std::uint64_t bytes{};
};

[[nodiscard]] auto has_slot(const std::filesystem::path& directory, const std::string& slot)
    -> result<bool>;
[[nodiscard]] auto publish_full(const std::filesystem::path& directory,
                                const std::string& slot,
                                const std::string& generation,
                                const std::vector<document>& documents) -> result<void>;
[[nodiscard]] auto publish_delta(const std::filesystem::path& directory,
                                 const std::string& slot,
                                 const std::string& generation,
                                 const std::vector<document_update>& updates) -> result<void>;
[[nodiscard]] auto query(const std::filesystem::path& directory,
                         const std::string& slot,
                         const std::string& text,
                         std::uint64_t limit) -> result<std::vector<search_hit>>;
[[nodiscard]] auto statistics(const std::filesystem::path& directory, const std::string& slot)
    -> result<index_statistics>;
[[nodiscard]] auto publish_checkout(const std::filesystem::path& checkout,
                                    const std::filesystem::path& directory,
                                    const std::string& generation) -> result<void>;
[[nodiscard]] auto make_daemon_target(std::filesystem::path checkout)
    -> std::shared_ptr<daemon_target>;

} // namespace liquid::search_detail
