#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "crdt/schema.hpp"

namespace liquid::crdt {

using head = std::array<std::byte, 32>;
using head_set = std::vector<head>;

class sync_state {
public:
  sync_state();
  ~sync_state();
  sync_state(sync_state &&) noexcept;
  auto operator=(sync_state &&) noexcept -> sync_state &;
  sync_state(const sync_state &) = delete;
  auto operator=(const sync_state &) -> sync_state & = delete;

private:
  struct impl;
  std::unique_ptr<impl> impl_;
  friend class document;
};

class document {
public:
  document(document &&) noexcept;
  auto operator=(document &&) noexcept -> document &;
  ~document();
  document(const document &) = delete;
  auto operator=(const document &) -> document & = delete;

  static auto create(const repo_id &repository, std::string_view taap_id,
                     std::string_view device_id) -> result<document>;
  static auto load(std::span<const std::byte> bytes, std::string_view taap_id,
                   std::string_view device_id) -> result<document>;

  [[nodiscard]] auto actor() const -> actor_id;
  [[nodiscard]] auto save() const -> result<std::vector<std::byte>>;
  [[nodiscard]] auto heads() -> result<head_set>;
  [[nodiscard]] auto transact(std::vector<operation> operations) -> result<std::vector<operation>>;
  [[nodiscard]] auto diff(const head_set &before_heads) const -> result<std::vector<operation>>;
  [[nodiscard]] auto inspect(std::string_view path) const -> result<std::optional<entry>>;
  [[nodiscard]] auto entries() const -> result<std::vector<entry>>;
  [[nodiscard]] auto operations() const -> result<std::vector<operation>>;

  [[nodiscard]] auto generate_sync_message(sync_state &state) const
      -> result<std::optional<std::vector<std::byte>>>;
  [[nodiscard]] auto receive_sync_message(sync_state &state, std::span<const std::byte> bytes)
      -> result<void>;
  [[nodiscard]] auto merge(const document &other) -> result<void>;

private:
  struct impl;
  explicit document(std::unique_ptr<impl> implementation);
  std::unique_ptr<impl> impl_;
};

class directory_document {
public:
  directory_document(directory_document &&) noexcept;
  auto operator=(directory_document &&) noexcept -> directory_document &;
  ~directory_document();
  directory_document(const directory_document &) = delete;
  auto operator=(const directory_document &) -> directory_document & = delete;

  static auto create(const repo_id &repository, std::string_view taap_id,
                     std::string_view device_id) -> result<directory_document>;
  static auto load(std::span<const std::byte> bytes, std::string_view taap_id,
                   std::string_view device_id) -> result<directory_document>;

  struct named_entry {
    std::string name;
    entry value;
  };

  [[nodiscard]] auto entries() const -> result<std::vector<named_entry>>;
  [[nodiscard]] auto entry_at(std::string_view name) const -> result<std::optional<entry>>;
  [[nodiscard]] auto put_entry(std::string name, entry value) -> result<void>;
  [[nodiscard]] auto remove_entry(std::string name) -> result<void>;
  [[nodiscard]] auto rename_entry(std::string from, std::string to) -> result<void>;
  [[nodiscard]] auto save() const -> result<std::vector<std::byte>>;
  [[nodiscard]] auto heads() -> result<head_set>;
  [[nodiscard]] auto generate_sync_message(sync_state &state) const
      -> result<std::optional<std::vector<std::byte>>>;
  [[nodiscard]] auto receive_sync_message(sync_state &state, std::span<const std::byte> bytes)
      -> result<void>;
  [[nodiscard]] auto merge(const directory_document &other) -> result<void>;

private:
  explicit directory_document(document value);
  document document_;
};

[[nodiscard]] auto derive_actor_id(std::string_view taap_id, std::string_view device_id)
    -> result<actor_id>;

} // namespace liquid::crdt
