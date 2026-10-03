#pragma once

#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../crypto/crypto.hpp"
#include "index.hpp"
#include "leases.hpp"
#include "liquid/store.hpp"
#include "mutation_mutex.hpp"
#include "owner.hpp"
#include "subject_index.hpp"

namespace liquid::store_detail {

struct collection_manifest;

struct stored_object {
    digest id{};
    std::uint64_t size{};
    std::string key_id;
    digest descriptor{};
    bool has_descriptor{};
    std::uint64_t descriptor_resemblance{};
    std::string kind{"unknown"};
    std::vector<digest> chunks;
    std::vector<std::uint64_t> chunk_sizes;
    std::vector<std::uint64_t> chunk_resemblances;
};

class store_backend {
public:
    static runtime::task<std::unique_ptr<store_backend>> open(std::filesystem::path root,
                                                              store_options options);

    [[nodiscard]] runtime::task<digest> put(std::vector<std::byte> input);
    [[nodiscard]] runtime::task<digest> put_file(std::filesystem::path source);
    [[nodiscard]] runtime::task<void> get_to_file(digest id, std::filesystem::path destination);
    [[nodiscard]] result<blob> blob_for(digest id) const;
    [[nodiscard]] runtime::task<digest> put_plain(std::vector<std::byte> input);
    [[nodiscard]] runtime::task<digest> put_plain(std::vector<std::byte> input,
                                                  std::string delta_scope);
    [[nodiscard]] runtime::task<digest> put_plain(std::vector<std::byte> input,
                                                  algorithm hash_algorithm);
    [[nodiscard]] runtime::task<digest> put_encrypted(std::vector<std::byte> input,
                                                      std::string key_id);
    void begin_object_batch();
    void abort_object_batch();
    [[nodiscard]] runtime::task<void> commit_object_batch();
    [[nodiscard]] runtime::task<std::vector<std::byte>> get(digest id);
    [[nodiscard]] runtime::task<std::uint64_t>
    read(digest id, std::uint64_t offset, std::span<std::byte> into);
    [[nodiscard]] runtime::task<bool> contains(digest id);
    [[nodiscard]] runtime::task<blob_availability> has(digest id);
    [[nodiscard]] runtime::task<void>
    register_remote_source(std::string id, std::shared_ptr<remote_blob_source> source);
    [[nodiscard]] runtime::task<void> register_remote(blob value);
    [[nodiscard]] runtime::task<void> evict(digest id);
    [[nodiscard]] runtime::task<std::uint64_t> local_bytes();
    [[nodiscard]] result<std::uint64_t> remote_bytes(digest blob_record) const;
    [[nodiscard]] std::vector<digest> attestations(digest subject) const;
    [[nodiscard]] std::vector<digest> object_ids(std::string_view kind) const;
    [[nodiscard]] runtime::task<std::optional<object_key_version>> key_version(digest id);
    [[nodiscard]] runtime::task<std::shared_ptr<lease_state>> lease_root(digest id,
                                                                         std::chrono::seconds ttl);
    [[nodiscard]] runtime::task<collection_report> collect(collection_options options);
    [[nodiscard]] runtime::task<void> redact(std::vector<digest> ids);
    [[nodiscard]] const std::filesystem::path& root_path() const noexcept;
    [[nodiscard]] const store_options& configuration() const noexcept;
    [[nodiscard]] runtime::task<std::shared_ptr<lease_state>>
    lease_root(digest id, std::chrono::seconds ttl, std::string owner);
    // Detaches the owner so store::close can destroy the store and then await the release.
    [[nodiscard]] owner_hold take_owner() noexcept;

private:
    explicit store_backend(std::filesystem::path root, store_options options);
    [[nodiscard]] runtime::task<void> load_objects();
    [[nodiscard]] runtime::task<void> load_remotes();
    [[nodiscard]] runtime::task<void> save_remotes();
    [[nodiscard]] runtime::task<void> rebuild_remote_record_index();
    [[nodiscard]] runtime::task<void> rebuild_subject_index();
    [[nodiscard]] runtime::task<void> save_objects();
    [[nodiscard]] runtime::task<void> stage_objects();
    [[nodiscard]] runtime::task<void> publish_staged_objects();
    [[nodiscard]] runtime::task<void>
    recover_pending_compactions(const collection_manifest& manifest);
    [[nodiscard]] runtime::task<void> index_packs(bool rebuild);
    [[nodiscard]] runtime::task<pack_entry>
    store_chunk(digest id,
                std::vector<std::byte> raw,
                std::uint64_t resemblance,
                std::optional<crypto_detail::domain_keys> encryption = std::nullopt,
                std::string_view delta_scope = {});
    [[nodiscard]] runtime::task<std::vector<std::byte>>
    load_chunk(digest id, std::optional<crypto_detail::domain_keys> encryption = std::nullopt);
    [[nodiscard]] runtime::task<std::vector<std::byte>>
    load_chunk(digest id, std::optional<crypto_detail::domain_keys> encryption, std::size_t depth);
    [[nodiscard]] runtime::task<std::vector<std::byte>>
    load_snapshot_object(const stored_object& object,
                         const std::map<std::string, chunk_location>& locations);
    [[nodiscard]] runtime::task<std::vector<std::byte>>
    hydrate_remote_chunk(const blob& value, std::size_t index, std::uint64_t offset);
    [[nodiscard]] runtime::task<std::vector<std::byte>> get_remote(const blob& value);
    [[nodiscard]] runtime::task<std::uint64_t>
    read_remote(const blob& value, std::uint64_t offset, std::span<std::byte> into);
    [[nodiscard]] runtime::task<void> add_object(stored_object object);
    [[nodiscard]] runtime::task<stored_object>
    scan_file(std::filesystem::path source,
              std::optional<crypto_detail::domain_keys> encryption,
              std::string key_id,
              bool store_chunks);
    [[nodiscard]] runtime::task<digest> put_selected(std::vector<std::byte> input,
                                                     std::optional<std::string> key_id,
                                                     algorithm hash_algorithm,
                                                     std::string delta_scope = {});

    friend class collector;
    friend class compactor;

    // Declared first so it is released last, after every member that may still write.
    owner_hold owner_;
    std::filesystem::path root_;
    store_options options_;
    std::shared_ptr<::liquid::store_backend> persistence_;
    std::map<std::string, std::shared_ptr<remote_blob_source>> remote_sources_;
    chunk_index index_;
    std::map<std::uint32_t, pack_contents> packs_;
    std::map<std::string, stored_object> objects_;
    std::map<std::string, blob> remotes_;
    std::map<std::string, digest> remote_records_;
    std::map<std::string, std::string> chunk_keys_;
    std::map<std::string, digest> chunk_bases_;
    struct delta_candidate {
        digest id{};
        std::vector<std::byte> bytes;
    };
    struct delta_candidate_key {
        std::string scope;
        std::uint64_t resemblance{};
        digest id{};
    };
    void remember_delta_candidate(std::string_view scope,
                                  std::uint64_t resemblance,
                                  digest id,
                                  std::span<const std::byte> bytes);
    std::map<std::string, std::map<std::uint64_t, delta_candidate>> delta_candidates_;
    std::deque<delta_candidate_key> delta_candidate_order_;
    std::size_t delta_candidate_bytes_{};
    subject_index subjects_;
    std::uint32_t current_pack_{};
    std::uint64_t object_batch_depth_{};
    bool objects_dirty_{};
    mutation_mutex mutation_;
};

} // namespace liquid::store_detail
