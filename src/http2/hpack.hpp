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
    void set_max_table_size(std::size_t size);

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

class hpack_decoder {
public:
    explicit hpack_decoder(std::size_t table_size = 4096);

    [[nodiscard]] auto decode(std::span<const std::byte> block)
        -> result<std::vector<header_field>>;
    void set_max_table_size(std::size_t size);

private:
    struct entry {
        header_field field;
    };
    std::vector<entry> dynamic_;
    std::size_t size_{};
    std::size_t max_size_{};

    void insert(header_field field);
};

} // namespace liquid::http2
