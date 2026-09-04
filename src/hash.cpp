#include "liquid/hash.hpp"

#include <exception>
#include <utility>

#include "blake3.h"
#include "hash/sha256.h"

namespace liquid {
namespace {

constexpr char hex_digits[] = "0123456789abcdef";

int hex_value(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

result<digest> parse_error(std::string message) {
    return std::unexpected(error{errc::invalid_argument, std::move(message), "parse_digest"});
}

}  // namespace

result<digest> hash(algorithm alg, std::span<const std::byte> input) {
    digest output{alg, {}};
    switch (alg) {
        case algorithm::blake3: {
            blake3_hasher hasher;
            blake3_hasher_init(&hasher);
            blake3_hasher_update(&hasher, input.data(), input.size());
            blake3_hasher_finalize(
                &hasher, reinterpret_cast<std::uint8_t*>(output.bytes.data()),
                output.bytes.size());
            return output;
        }
        case algorithm::sha256: {
            SHA256_CTX context;
            sha256_init(&context);
            sha256_update(
                &context, reinterpret_cast<const BYTE*>(input.data()), input.size());
            sha256_final(&context, reinterpret_cast<BYTE*>(output.bytes.data()));
            return output;
        }
    }
    return std::unexpected(
        error{errc::invalid_argument, "unknown digest algorithm", "hash"});
}

std::string to_string(const digest& value) {
    std::string output;
    switch (value.alg) {
        case algorithm::blake3:
            output = "blake3:";
            break;
        case algorithm::sha256:
            output = "sha256:";
            break;
        default:
            output = "unknown:";
            break;
    }
    output.reserve(output.size() + value.bytes.size() * 2);
    for (const auto byte : value.bytes) {
        const auto raw = std::to_integer<unsigned char>(byte);
        output.push_back(hex_digits[raw >> 4]);
        output.push_back(hex_digits[raw & 0x0f]);
    }
    return output;
}

result<digest> parse_digest(std::string_view text) {
    try {
        algorithm alg;
        std::string_view hex;
        if (text.starts_with("blake3:")) {
            alg = algorithm::blake3;
            hex = text.substr(7);
        } else if (text.starts_with("sha256:")) {
            alg = algorithm::sha256;
            hex = text.substr(7);
        } else {
            return parse_error("unknown digest algorithm");
        }

        if (hex.size() != 64) return parse_error("digest must contain 64 hexadecimal digits");

        digest output{alg, {}};
        for (std::size_t i = 0; i < output.bytes.size(); ++i) {
            const int high = hex_value(hex[i * 2]);
            const int low = hex_value(hex[i * 2 + 1]);
            if (high < 0 || low < 0) return parse_error("digest contains a non-hexadecimal digit");
            output.bytes[i] = std::byte{static_cast<unsigned char>((high << 4) | low)};
        }
        return output;
    } catch (const std::exception& exception) {
        return std::unexpected(error{errc::internal, exception.what(), "parse_digest"});
    } catch (...) {
        return std::unexpected(error{errc::internal, "unknown exception", "parse_digest"});
    }
}

}  // namespace liquid
