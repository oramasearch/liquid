#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "liquid/error.hpp"

namespace liquid::http2 {

struct header_field {
    std::string name;
    std::string value;
    bool sensitive{};

    auto operator==(const header_field&) const -> bool = default;
};

[[nodiscard]] auto huffman_encode(std::string_view input) -> std::vector<std::byte>;
[[nodiscard]] auto huffman_decode(std::span<const std::byte> input) -> result<std::string>;

class hpack_encoder {
public:
    explicit hpack_encoder(std::size_t table_size = 4096);

    [[nodiscard]] auto encode(std::span<const header_field> fields)
        -> result<std::vector<std::byte>>;
    // Sets the size of the table this encoder uses, which must not exceed the peer's
    // SETTINGS_HEADER_TABLE_SIZE. The next block starts with a dynamic table size update.
    void set_max_table_size(std::size_t size);
    [[nodiscard]] auto max_table_size() const noexcept -> std::size_t {
        return max_size_;
    }

private:
    struct entry {
        header_field field;
    };
    std::vector<entry> dynamic_;
    std::size_t size_{};
    std::size_t max_size_{};
    bool emit_size_update_{};

    void insert(header_field field);
};

// The size of a header list as RFC 9113 section 6.5.2 defines it for
// SETTINGS_MAX_HEADER_LIST_SIZE: each field's name and value octets plus 32.
[[nodiscard]] auto header_list_size(std::span<const header_field> fields) -> std::size_t;

class hpack_decoder {
public:
    explicit hpack_decoder(std::size_t table_size = 4096);

    // Decodes one header block. A block whose decoded list would exceed max_list_size fails, so a
    // few bytes of indexed references cannot expand into an unbounded list.
    [[nodiscard]] auto decode(std::span<const std::byte> block,
                              std::size_t max_list_size = static_cast<std::size_t>(-1))
        -> result<std::vector<header_field>>;
    // Sets the limit this endpoint advertised as SETTINGS_HEADER_TABLE_SIZE. The peer's encoder
    // may move the table size anywhere up to this limit with dynamic table size updates, never
    // above it.
    void set_max_table_size(std::size_t size);
    [[nodiscard]] auto table_size() const noexcept -> std::size_t {
        return size_;
    }
    [[nodiscard]] auto max_table_size() const noexcept -> std::size_t {
        return max_size_;
    }

private:
    struct entry {
        header_field field;
    };
    std::vector<entry> dynamic_;
    std::size_t size_{};
    std::size_t max_size_{};
    std::size_t limit_{};

    void insert(header_field field);
};

} // namespace liquid::http2
