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
    std::filesystem::path source;
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

struct planner_inputs {
    index_statistics working_set;
    std::uint64_t drips_since_index_update{};
    std::uint64_t scan_nanoseconds_per_blob{};
    std::uint64_t scan_parallelism{1};
    double trigram_selectivity{1.0};
    bool index_available{};
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
[[nodiscard]] auto query(const std::filesystem::path& directory,
                         const std::string& slot,
                         const std::string& text,
                         std::uint64_t limit,
                         search_executor executor) -> result<std::vector<search_hit>>;
[[nodiscard]] auto scan(const std::vector<document>& documents,
                        const std::string& text,
                        std::uint64_t limit) -> result<std::vector<search_hit>>;
[[nodiscard]] auto classify(const std::string& text) -> result<search_query_kind>;
[[nodiscard]] auto selectivity(const std::filesystem::path& directory,
                               const std::string& slot,
                               const std::string& text) -> result<double>;
[[nodiscard]] auto make_plan(const planner_inputs& inputs, search_query_kind kind) -> search_plan;
[[nodiscard]] auto calibration(const std::filesystem::path& directory,
                               const std::vector<document>& sample) -> result<std::uint64_t>;
[[nodiscard]] auto checkout_documents(const std::filesystem::path& checkout,
                                      bool retain_contents = false)
    -> result<std::vector<document>>;
[[nodiscard]] auto indexed_documents(const std::filesystem::path& directory,
                                     const std::string& slot) -> result<std::vector<document>>;
[[nodiscard]] auto current_generation(const std::filesystem::path& directory)
    -> result<std::string>;
[[nodiscard]] auto freshness(const std::filesystem::path& directory, const std::string& slot)
    -> result<std::uint64_t>;
[[nodiscard]] auto slot_generation(const std::filesystem::path& directory, const std::string& slot)
    -> result<std::optional<std::string>>;
[[nodiscard]] auto statistics(const std::filesystem::path& directory, const std::string& slot)
    -> result<index_statistics>;
[[nodiscard]] auto publish_checkout(const std::filesystem::path& checkout,
                                    const std::filesystem::path& directory,
                                    const std::string& generation) -> result<void>;
[[nodiscard]] auto make_daemon_target(std::filesystem::path checkout)
    -> std::shared_ptr<daemon_target>;

} // namespace liquid::search_detail
