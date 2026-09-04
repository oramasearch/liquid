#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <vector>

#include <boost/capy/ex/async_mutex.hpp>

#include "index.hpp"
#include "liquid/store.hpp"

namespace liquid::store_detail {

struct stored_object {
  digest id{};
  std::uint64_t size{};
  std::vector<digest> chunks;
};

class store_backend {
public:
  static runtime::task<std::unique_ptr<store_backend>> open(std::filesystem::path root,
                                                            store_options options);

  [[nodiscard]] runtime::task<digest> put(std::vector<std::byte> input);
  [[nodiscard]] runtime::task<std::vector<std::byte>> get(digest id);
  [[nodiscard]] runtime::task<std::uint64_t> read(digest id, std::uint64_t offset,
                                                  std::span<std::byte> into);
  [[nodiscard]] runtime::task<bool> contains(digest id);

private:
  explicit store_backend(std::filesystem::path root, store_options options);
  [[nodiscard]] runtime::task<void> load_objects();
  [[nodiscard]] runtime::task<void> save_objects();
  [[nodiscard]] runtime::task<void> index_packs(bool rebuild);
  [[nodiscard]] runtime::task<pack_entry> store_chunk(digest id, std::vector<std::byte> raw);
  [[nodiscard]] runtime::task<std::vector<std::byte>> load_chunk(digest id);
  [[nodiscard]] runtime::task<void> add_object(stored_object object);

  std::filesystem::path root_;
  store_options options_;
  chunk_index index_;
  std::map<std::uint32_t, pack_contents> packs_;
  std::map<std::string, stored_object> objects_;
  std::uint32_t current_pack_{};
  boost::capy::async_mutex mutation_;
};

} // namespace liquid::store_detail
