#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <utility>
#include <string_view>
#include <vector>

#include "crdt/document.hpp"

namespace liquid::crdt {

struct drip_record {
    node_id id{};
    head_set base;
    std::vector<operation> ops;
    actor_id by{};
    std::int64_t at{0};
    std::optional<std::vector<std::byte>> signature;
};

class journal {
  public:
    struct cursor {
        std::uint64_t offset{0};

        auto operator==(const cursor &) const -> bool = default;
    };

    explicit journal(std::filesystem::path path);

    [[nodiscard]] auto append(const drip_record &record) -> result<void>;
    [[nodiscard]] auto read(cursor from, std::size_t max) const
        -> result<std::pair<std::vector<drip_record>, cursor>>;
    [[nodiscard]] auto end() const -> result<cursor>;
    [[nodiscard]] auto cut(const document &value, std::uint64_t epoch) -> result<void>;
    [[nodiscard]] auto replay(std::string_view taap_id, std::string_view device_id) const
        -> result<document>;

  private:
    [[nodiscard]] auto cut_path() const -> std::filesystem::path;
    std::filesystem::path path_;
};

} // namespace liquid::crdt
