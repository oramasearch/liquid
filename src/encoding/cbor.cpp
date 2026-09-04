#include "encoding/cbor.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <span>
#include <string_view>

namespace liquid::encoding::cbor {
namespace {

auto failure(std::string message) -> error {
    return error{errc::corrupt, std::move(message), "CBOR"};
}

auto valid_utf8(std::string_view text) -> bool {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i]);
        std::size_t continuation = 0;
        std::uint32_t codepoint = 0;
        if (first <= 0x7f) {
            ++i;
            continue;
        }
        if ((first & 0xe0U) == 0xc0U) {
            continuation = 1;
            codepoint = first & 0x1fU;
        } else if ((first & 0xf0U) == 0xe0U) {
            continuation = 2;
            codepoint = first & 0x0fU;
        } else if ((first & 0xf8U) == 0xf0U) {
            continuation = 3;
            codepoint = first & 0x07U;
        } else {
            return false;
        }
        if (i + continuation >= text.size())
            return false;
        for (std::size_t j = 1; j <= continuation; ++j) {
            const auto next = static_cast<unsigned char>(text[i + j]);
            if ((next & 0xc0U) != 0x80U)
                return false;
            codepoint = (codepoint << 6U) | (next & 0x3fU);
        }
        const auto minimum = std::array<std::uint32_t, 4>{0, 0x80, 0x800, 0x10000};
        if (codepoint < minimum[continuation] || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff))
            return false;
        i += continuation + 1;
    }
    return true;
}

void append_uint(std::vector<std::byte> &output, std::uint8_t major, std::uint64_t value) {
    const auto initial = static_cast<std::uint8_t>(major << 5U);
    if (value < 24) {
        output.push_back(std::byte{static_cast<std::uint8_t>(initial | value)});
    } else if (value <= std::numeric_limits<std::uint8_t>::max()) {
        output.push_back(std::byte{static_cast<std::uint8_t>(initial | 24U)});
        output.push_back(std::byte{static_cast<std::uint8_t>(value)});
    } else if (value <= std::numeric_limits<std::uint16_t>::max()) {
        output.push_back(std::byte{static_cast<std::uint8_t>(initial | 25U)});
        output.push_back(std::byte{static_cast<std::uint8_t>(value >> 8U)});
        output.push_back(std::byte{static_cast<std::uint8_t>(value)});
    } else if (value <= std::numeric_limits<std::uint32_t>::max()) {
        output.push_back(std::byte{static_cast<std::uint8_t>(initial | 26U)});
        for (auto shift : {24U, 16U, 8U, 0U}) {
            output.push_back(std::byte{static_cast<std::uint8_t>(value >> shift)});
        }
    } else {
        output.push_back(std::byte{static_cast<std::uint8_t>(initial | 27U)});
        for (auto shift : {56U, 48U, 40U, 32U, 24U, 16U, 8U, 0U}) {
            output.push_back(std::byte{static_cast<std::uint8_t>(value >> shift)});
        }
    }
}

auto encode_into(const value &input, std::vector<std::byte> &output) -> result<void>;

auto encoded_key(const value &key) -> result<std::vector<std::byte>> {
    auto bytes = std::vector<std::byte>{};
    if (auto encoded = encode_into(key, bytes); !encoded) {
        return std::unexpected(encoded.error());
    }
    return bytes;
}

auto encode_into(const value &input, std::vector<std::byte> &output) -> result<void> {
    return std::visit(
        [&](const auto &item) -> result<void> {
            using type = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<type, null>) {
                output.push_back(std::byte{0xf6});
            } else if constexpr (std::is_same_v<type, bool>) {
                output.push_back(item ? std::byte{0xf5} : std::byte{0xf4});
            } else if constexpr (std::is_same_v<type, std::uint64_t>) {
                append_uint(output, 0, item);
            } else if constexpr (std::is_same_v<type, std::int64_t>) {
                if (item >= 0) {
                    append_uint(output, 0, static_cast<std::uint64_t>(item));
                } else {
                    append_uint(output, 1, static_cast<std::uint64_t>(-(item + 1)));
                }
            } else if constexpr (std::is_same_v<type, value::bytes>) {
                append_uint(output, 2, item.size());
                output.insert(output.end(), item.begin(), item.end());
            } else if constexpr (std::is_same_v<type, std::string>) {
                if (!valid_utf8(item))
                    return std::unexpected(failure("invalid UTF-8 text string"));
                append_uint(output, 3, item.size());
                output.insert(output.end(), reinterpret_cast<const std::byte *>(item.data()),
                              reinterpret_cast<const std::byte *>(item.data() + item.size()));
            } else if constexpr (std::is_same_v<type, value::array>) {
                append_uint(output, 4, item.size());
                for (const auto &child : item) {
                    if (auto encoded = encode_into(child, output); !encoded) {
                        return encoded;
                    }
                }
            } else if constexpr (std::is_same_v<type, value::map>) {
                struct entry {
                    std::vector<std::byte> key;
                    const value *val;
                };
                auto entries = std::vector<entry>{};
                entries.reserve(item.size());
                for (const auto &[key, val] : item) {
                    auto bytes = encoded_key(key);
                    if (!bytes)
                        return std::unexpected(bytes.error());
                    entries.push_back(entry{std::move(*bytes), &val});
                }
                std::ranges::sort(entries, {}, &entry::key);
                if (std::ranges::adjacent_find(entries, {}, &entry::key) != entries.end()) {
                    return std::unexpected(failure("duplicate map key"));
                }
                append_uint(output, 5, entries.size());
                for (const auto &current : entries) {
                    output.insert(output.end(), current.key.begin(), current.key.end());
                    if (auto encoded = encode_into(*current.val, output); !encoded) {
                        return encoded;
                    }
                }
            }
            return {};
        },
        input.data);
}

class decoder {
  public:
    explicit decoder(std::span<const std::byte> input) : input_(input) {}

    auto read() -> result<value> {
        if (position_ == input_.size()) {
            return std::unexpected(failure("unexpected end of input"));
        }
        const auto initial = byte();
        const auto major = static_cast<std::uint8_t>(initial >> 5U);
        const auto additional = static_cast<std::uint8_t>(initial & 0x1fU);

        if (major == 7) {
            if (additional == 20)
                return value{false};
            if (additional == 21)
                return value{true};
            if (additional == 22)
                return value{null{}};
            return std::unexpected(failure("unsupported simple value"));
        }

        auto argument = read_argument(additional);
        if (!argument)
            return std::unexpected(argument.error());
        switch (major) {
        case 0:
            return value{*argument};
        case 1:
            if (*argument > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                return std::unexpected(failure("negative integer is out of range"));
            }
            return value{-1 - static_cast<std::int64_t>(*argument)};
        case 2:
            return read_bytes(*argument);
        case 3:
            return read_text(*argument);
        case 4:
            return read_array(*argument);
        case 5:
            return read_map(*argument);
        default:
            return std::unexpected(failure("unsupported major type"));
        }
    }

    auto finished() const -> bool { return position_ == input_.size(); }

  private:
    auto byte() -> std::uint8_t { return std::to_integer<std::uint8_t>(input_[position_++]); }

    auto read_argument(std::uint8_t additional) -> result<std::uint64_t> {
        if (additional < 24)
            return additional;
        std::size_t width = 0;
        switch (additional) {
        case 24:
            width = 1;
            break;
        case 25:
            width = 2;
            break;
        case 26:
            width = 4;
            break;
        case 27:
            width = 8;
            break;
        default:
            return std::unexpected(failure("indefinite lengths are unsupported"));
        }
        if (input_.size() - position_ < width) {
            return std::unexpected(failure("truncated integer"));
        }
        auto result = std::uint64_t{0};
        for (std::size_t i = 0; i < width; ++i)
            result = (result << 8U) | byte();
        const auto minimum =
            std::array<std::uint64_t, 9>{0, 24, 256, 0, 65536, 0, 0, 0, 4294967296ULL};
        if (result < minimum[width]) {
            return std::unexpected(failure("integer is not shortest form"));
        }
        return result;
    }

    auto bounded_size(std::uint64_t size) -> result<std::size_t> {
        if (size > input_.size() - position_) {
            return std::unexpected(failure("declared length exceeds input"));
        }
        return static_cast<std::size_t>(size);
    }

    auto read_bytes(std::uint64_t length) -> result<value> {
        auto size = bounded_size(length);
        if (!size)
            return std::unexpected(size.error());
        auto result = value::bytes(input_.begin() + static_cast<std::ptrdiff_t>(position_),
                                   input_.begin() + static_cast<std::ptrdiff_t>(position_ + *size));
        position_ += *size;
        return value{std::move(result)};
    }

    auto read_text(std::uint64_t length) -> result<value> {
        auto size = bounded_size(length);
        if (!size)
            return std::unexpected(size.error());
        auto result = std::string(reinterpret_cast<const char *>(input_.data() + position_), *size);
        position_ += *size;
        if (!valid_utf8(result))
            return std::unexpected(failure("invalid UTF-8 text string"));
        return value{std::move(result)};
    }

    auto read_array(std::uint64_t length) -> result<value> {
        if (length > input_.size() - position_) {
            return std::unexpected(failure("array length exceeds input"));
        }
        auto result = value::array{};
        result.reserve(static_cast<std::size_t>(length));
        for (std::uint64_t i = 0; i < length; ++i) {
            auto item = read();
            if (!item)
                return std::unexpected(item.error());
            result.push_back(std::move(*item));
        }
        return value{std::move(result)};
    }

    auto read_map(std::uint64_t length) -> result<value> {
        if (length > (input_.size() - position_) / 2) {
            return std::unexpected(failure("map length exceeds input"));
        }
        auto result = value::map{};
        result.reserve(static_cast<std::size_t>(length));
        auto previous = std::vector<std::byte>{};
        for (std::uint64_t i = 0; i < length; ++i) {
            const auto start = position_;
            auto key = read();
            if (!key)
                return std::unexpected(key.error());
            auto encoded =
                std::vector<std::byte>(input_.begin() + static_cast<std::ptrdiff_t>(start),
                                       input_.begin() + static_cast<std::ptrdiff_t>(position_));
            if (i != 0 && !(previous < encoded)) {
                return std::unexpected(failure("map keys are not in canonical order"));
            }
            previous = std::move(encoded);
            auto val = read();
            if (!val)
                return std::unexpected(val.error());
            result.emplace_back(std::move(*key), std::move(*val));
        }
        return value{std::move(result)};
    }

    std::span<const std::byte> input_;
    std::size_t position_{0};
};

} // namespace

auto encode(const value &input) -> result<std::vector<std::byte>> {
    try {
        auto output = std::vector<std::byte>{};
        if (auto encoded = encode_into(input, output); !encoded) {
            return std::unexpected(encoded.error());
        }
        return output;
    } catch (const std::exception &exception) {
        return std::unexpected(error{errc::internal, exception.what(), "CBOR encode"});
    }
}

auto decode(const std::vector<std::byte> &input) -> result<value> {
    return decode(std::span<const std::byte>{input});
}

auto decode(std::span<const std::byte> input) -> result<value> {
    try {
        auto source = decoder{input};
        auto output = source.read();
        if (!output)
            return output;
        if (!source.finished()) {
            return std::unexpected(failure("trailing bytes"));
        }
        return output;
    } catch (const std::exception &exception) {
        return std::unexpected(error{errc::internal, exception.what(), "CBOR decode"});
    }
}

} // namespace liquid::encoding::cbor
