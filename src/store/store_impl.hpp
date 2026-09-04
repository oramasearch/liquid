#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <boost/capy/ex/async_mutex.hpp>

#include "../crypto/crypto.hpp"
#include "index.hpp"
#include "leases.hpp"
#include "liquid/store.hpp"

namespace liquid::store_detail {

struct collection_manifest;

struct stored_object {
    digest id{};
    std::uint64_t size{};
    std::string key_id;
    digest descriptor{};
    bool has_descriptor{};
    std::string kind{"unknown"};
    std::vector<digest> chunks;
    std::vector<std::uint64_t> chunk_sizes;
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
    [[nodiscard]] runtime::task<digest> put_encrypted(std::vector<std::byte> input,
                                                      std::string key_id);
    [[nodiscard]] runtime::task<std::vector<std::byte>> get(digest id);
    [[nodiscard]] runtime::task<std::uint64_t>
    read(digest id, std::uint64_t offset, std::span<std::byte> into);
    [[nodiscard]] runtime::task<bool> contains(digest id);
    [[nodiscard]] runtime::task<std::optional<object_key_version>> key_version(digest id);
    [[nodiscard]] runtime::task<std::shared_ptr<lease_state>> lease_root(digest id,
                                                                         std::chrono::seconds ttl);
    [[nodiscard]] runtime::task<collection_report> collect(collection_options options);
    [[nodiscard]] runtime::task<void> redact(std::vector<digest> ids);

private:
    explicit store_backend(std::filesystem::path root, store_options options);
    [[nodiscard]] runtime::task<void> load_objects();
    [[nodiscard]] runtime::task<void> save_objects();
    [[nodiscard]] runtime::task<void> stage_objects();
    [[nodiscard]] runtime::task<void> publish_staged_objects();
    [[nodiscard]] runtime::task<void>
    recover_pending_compactions(const collection_manifest& manifest);
    [[nodiscard]] runtime::task<void> index_packs(bool rebuild);
    [[nodiscard]] runtime::task<pack_entry>
    store_chunk(digest id,
                std::vector<std::byte> raw,
                std::optional<crypto_detail::domain_keys> encryption = std::nullopt);
    [[nodiscard]] runtime::task<std::vector<std::byte>>
    load_chunk(digest id, std::optional<crypto_detail::domain_keys> encryption = std::nullopt);
    [[nodiscard]] runtime::task<std::vector<std::byte>>
    load_snapshot_object(const stored_object& object,
                         const std::map<std::string, chunk_location>& locations);
    [[nodiscard]] runtime::task<void> add_object(stored_object object);
    [[nodiscard]] runtime::task<digest> put_selected(std::vector<std::byte> input,
                                                     std::optional<std::string> key_id);

    friend class collector;
    friend class compactor;

    std::filesystem::path root_;
    store_options options_;
    chunk_index index_;
    std::map<std::uint32_t, pack_contents> packs_;
    std::map<std::string, stored_object> objects_;
    std::map<std::string, std::string> chunk_keys_;
    std::uint32_t current_pack_{};
    boost::capy::async_mutex mutation_;
};

} // namespace liquid::store_detail
