// SPDX-License-Identifier: MIT

#include "crypto/signature.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <string_view>

#include <boost/multiprecision/cpp_int.hpp>

#include "liquid/asn1.hpp"
#include "liquid/hash.hpp"

namespace liquid::crypto {
namespace {

using boost::multiprecision::cpp_int;

struct sha512_context {
    std::array<std::uint64_t, 8> state{
        0xcbbb9d5dc1059ed8ULL,
        0x629a292a367cd507ULL,
        0x9159015a3070dd17ULL,
        0x152fecd8f70e5939ULL,
        0x67332667ffc00b31ULL,
        0x8eb44a8768581511ULL,
        0xdb0c2e0d64f98fa7ULL,
        0x47b5481dbefa4fa4ULL,
    };
    std::array<std::byte, 128> block{};
    std::uint64_t high_bits = 0;
    std::uint64_t low_bits = 0;
    std::size_t used = 0;
};

constexpr std::array<std::uint64_t, 80> sha512_constants{
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

constexpr auto load64(const std::byte* input) -> std::uint64_t {
    std::uint64_t output = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        output = (output << 8) | std::to_integer<std::uint8_t>(input[index]);
    }
    return output;
}

auto store64(std::uint64_t value, std::byte* output) -> void {
    for (std::size_t index = 0; index < 8; ++index) {
        output[7 - index] = static_cast<std::byte>(value & 0xff);
        value >>= 8;
    }
}

auto sha384_transform(sha512_context& context) -> void {
    std::array<std::uint64_t, 80> words{};
    for (std::size_t index = 0; index < 16; ++index) {
        words[index] = load64(context.block.data() + index * 8);
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
        const std::uint64_t s0 = std::rotr(words[index - 15], 1) ^ std::rotr(words[index - 15], 8) ^
                                 (words[index - 15] >> 7);
        const std::uint64_t s1 = std::rotr(words[index - 2], 19) ^ std::rotr(words[index - 2], 61) ^
                                 (words[index - 2] >> 6);
        words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }

    auto [a, b, c, d, e, f, g, h] = context.state;
    for (std::size_t index = 0; index < words.size(); ++index) {
        const std::uint64_t sum1 = std::rotr(e, 14) ^ std::rotr(e, 18) ^ std::rotr(e, 41);
        const std::uint64_t choose = (e & f) ^ (~e & g);
        const std::uint64_t first = h + sum1 + choose + sha512_constants[index] + words[index];
        const std::uint64_t sum0 = std::rotr(a, 28) ^ std::rotr(a, 34) ^ std::rotr(a, 39);
        const std::uint64_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint64_t second = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + first;
        d = c;
        c = b;
        b = a;
        a = first + second;
    }
    context.state[0] += a;
    context.state[1] += b;
    context.state[2] += c;
    context.state[3] += d;
    context.state[4] += e;
    context.state[5] += f;
    context.state[6] += g;
    context.state[7] += h;
}

auto sha384_update(sha512_context& context, std::span<const std::byte> input) -> void {
    const std::uint64_t added_low = static_cast<std::uint64_t>(input.size()) << 3;
    const std::uint64_t previous_low = context.low_bits;
    context.low_bits += added_low;
    context.high_bits += static_cast<std::uint64_t>(input.size()) >> 61;
    if (context.low_bits < previous_low) {
        ++context.high_bits;
    }
    for (const std::byte value : input) {
        context.block[context.used++] = value;
        if (context.used == context.block.size()) {
            sha384_transform(context);
            context.used = 0;
        }
    }
}

auto sha384(std::span<const std::byte> input) -> std::vector<std::byte> {
    sha512_context context;
    sha384_update(context, input);
    context.block[context.used++] = std::byte{0x80};
    if (context.used > 112) {
        std::fill(context.block.begin() + static_cast<std::ptrdiff_t>(context.used),
                  context.block.end(),
                  std::byte{0});
        sha384_transform(context);
        context.used = 0;
    }
    std::fill(context.block.begin() + static_cast<std::ptrdiff_t>(context.used),
              context.block.begin() + 112,
              std::byte{0});
    store64(context.high_bits, context.block.data() + 112);
    store64(context.low_bits, context.block.data() + 120);
    sha384_transform(context);

    std::vector<std::byte> output(48);
    for (std::size_t index = 0; index < 6; ++index) {
        store64(context.state[index], output.data() + index * 8);
    }
    return output;
}

auto invalid(std::string message) -> error {
    return error{errc::corrupt, std::move(message), "signature verification"};
}

auto unsupported(std::string message) -> error {
    return error{errc::unsupported, std::move(message), "signature verification"};
}

auto integer(std::span<const std::byte> input) -> cpp_int {
    cpp_int output = 0;
    for (const std::byte value : input) {
        output <<= 8;
        output += std::to_integer<std::uint8_t>(value);
    }
    return output;
}

auto integer(std::string_view hexadecimal) -> cpp_int {
    cpp_int output = 0;
    for (const char value : hexadecimal) {
        output <<= 4;
        if (value >= '0' && value <= '9') {
            output += value - '0';
        } else {
            output += value - 'A' + 10;
        }
    }
    return output;
}

auto octets(cpp_int value, std::size_t size) -> std::vector<std::byte> {
    std::vector<std::byte> output(size);
    for (std::size_t index = 0; index < size; ++index) {
        output[size - 1 - index] =
            static_cast<std::byte>((value & 0xff).convert_to<unsigned int>());
        value >>= 8;
    }
    return output;
}

auto bit_length(const cpp_int& value) -> std::size_t {
    if (value == 0) {
        return 0;
    }
    return static_cast<std::size_t>(boost::multiprecision::msb(value)) + 1;
}

auto equal(std::span<const std::byte> left, std::span<const std::byte> right) -> bool {
    if (left.size() != right.size()) {
        return false;
    }
    std::uint8_t difference = 0;
    for (std::size_t index = 0; index < left.size(); ++index) {
        difference |= std::to_integer<std::uint8_t>(left[index] ^ right[index]);
    }
    return difference == 0;
}

auto mgf1(digest_kind kind, std::span<const std::byte> seed, std::size_t size)
    -> result<std::vector<std::byte>> {
    std::vector<std::byte> output;
    output.reserve(size);
    for (std::uint32_t counter = 0; output.size() < size; ++counter) {
        std::vector<std::byte> input(seed.begin(), seed.end());
        input.push_back(static_cast<std::byte>(counter >> 24));
        input.push_back(static_cast<std::byte>(counter >> 16));
        input.push_back(static_cast<std::byte>(counter >> 8));
        input.push_back(static_cast<std::byte>(counter));
        auto block = digest(kind, input);
        if (!block) {
            return std::unexpected(block.error());
        }
        const std::size_t count = std::min(block->size(), size - output.size());
        output.insert(output.end(), block->begin(), block->begin() + count);
    }
    return output;
}

auto verify_pkcs1(std::span<const std::byte> encoded,
                  digest_kind kind,
                  std::span<const std::byte> message_digest) -> result<void> {
    constexpr std::array<std::byte, 19> sha256_prefix{
        std::byte{0x30}, std::byte{0x31}, std::byte{0x30}, std::byte{0x0d}, std::byte{0x06},
        std::byte{0x09}, std::byte{0x60}, std::byte{0x86}, std::byte{0x48}, std::byte{0x01},
        std::byte{0x65}, std::byte{0x03}, std::byte{0x04}, std::byte{0x02}, std::byte{0x01},
        std::byte{0x05}, std::byte{0x00}, std::byte{0x04}, std::byte{0x20},
    };
    constexpr std::array<std::byte, 19> sha384_prefix{
        std::byte{0x30}, std::byte{0x41}, std::byte{0x30}, std::byte{0x0d}, std::byte{0x06},
        std::byte{0x09}, std::byte{0x60}, std::byte{0x86}, std::byte{0x48}, std::byte{0x01},
        std::byte{0x65}, std::byte{0x03}, std::byte{0x04}, std::byte{0x02}, std::byte{0x02},
        std::byte{0x05}, std::byte{0x00}, std::byte{0x04}, std::byte{0x30},
    };
    const std::span prefix =
        kind == digest_kind::sha256 ? std::span(sha256_prefix) : std::span(sha384_prefix);
    const std::size_t payload_size = prefix.size() + message_digest.size();
    if (encoded.size() < payload_size + 11 || encoded[0] != std::byte{0} ||
        encoded[1] != std::byte{1}) {
        return std::unexpected(invalid("RSA PKCS#1 v1.5 padding is invalid"));
    }
    const std::size_t separator = encoded.size() - payload_size - 1;
    if (separator < 10 || encoded[separator] != std::byte{0}) {
        return std::unexpected(invalid("RSA PKCS#1 v1.5 padding is invalid"));
    }
    for (std::size_t index = 2; index < separator; ++index) {
        if (encoded[index] != std::byte{0xff}) {
            return std::unexpected(invalid("RSA PKCS#1 v1.5 padding is invalid"));
        }
    }
    if (!equal(encoded.subspan(separator + 1, prefix.size()), prefix) ||
        !equal(encoded.last(message_digest.size()), message_digest)) {
        return std::unexpected(invalid("RSA PKCS#1 v1.5 digest does not match"));
    }
    return {};
}

auto verify_pss(std::span<const std::byte> encoded,
                std::size_t modulus_bits,
                digest_kind kind,
                std::size_t salt_length,
                std::span<const std::byte> message_digest) -> result<void> {
    const std::size_t digest_size = message_digest.size();
    if (encoded.size() < digest_size + salt_length + 2 || encoded.back() != std::byte{0xbc}) {
        return std::unexpected(invalid("RSA-PSS encoding is invalid"));
    }
    const std::size_t data_size = encoded.size() - digest_size - 1;
    const auto masked_data = encoded.first(data_size);
    const auto encoded_digest = encoded.subspan(data_size, digest_size);
    const std::size_t unused_bits = encoded.size() * 8 - (modulus_bits - 1);
    if (unused_bits > 8 ||
        (std::to_integer<std::uint8_t>(masked_data.front()) >> (8 - unused_bits)) != 0) {
        return std::unexpected(invalid("RSA-PSS high bits are invalid"));
    }
    auto mask = mgf1(kind, encoded_digest, data_size);
    if (!mask) {
        return std::unexpected(mask.error());
    }
    std::vector<std::byte> data(data_size);
    for (std::size_t index = 0; index < data_size; ++index) {
        data[index] = masked_data[index] ^ (*mask)[index];
    }
    if (unused_bits != 0) {
        data.front() &= static_cast<std::byte>(0xff >> unused_bits);
    }
    const std::size_t marker = data_size - salt_length - 1;
    if (data[marker] != std::byte{1} ||
        std::ranges::any_of(std::span(data).first(marker),
                            [](std::byte value) { return value != std::byte{0}; })) {
        return std::unexpected(invalid("RSA-PSS data block is invalid"));
    }
    std::vector<std::byte> hash_input(8, std::byte{0});
    hash_input.insert(hash_input.end(), message_digest.begin(), message_digest.end());
    hash_input.insert(
        hash_input.end(), data.begin() + static_cast<std::ptrdiff_t>(marker + 1), data.end());
    auto expected = digest(kind, hash_input);
    if (!expected) {
        return std::unexpected(expected.error());
    }
    if (!equal(*expected, encoded_digest)) {
        return std::unexpected(invalid("RSA-PSS digest does not match"));
    }
    return {};
}

struct curve {
    cpp_int prime;
    cpp_int coefficient;
    cpp_int order;
    cpp_int generator_x;
    cpp_int generator_y;
    std::size_t size = 0;
};

auto selected_curve(curve_kind kind) -> result<curve> {
    if (kind == curve_kind::p256) {
        const cpp_int prime =
            integer("FFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF");
        return curve{
            .prime = prime,
            .coefficient =
                integer("5AC635D8AA3A93E7B3EBBD55769886BC651D06B0CC53B0F63BCE3C3E27D2604B"),
            .order = integer("FFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551"),
            .generator_x =
                integer("6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296"),
            .generator_y =
                integer("4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5"),
            .size = 32,
        };
    }
    if (kind == curve_kind::p384) {
        const cpp_int prime = integer("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"
                                      "FFFEFFFFFFFF0000000000000000FFFFFFFF");
        return curve{
            .prime = prime,
            .coefficient = integer("B3312FA7E23EE7E4988E056BE3F82D19181D9C6EFE8141120314088F5013875"
                                   "AC656398D8A2ED19D2A85C8EDD3EC2AEF"),
            .order = integer("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFC7634D81F4372DDF581A0"
                             "DB248B0A77AECEC196ACCC52973"),
            .generator_x = integer("AA87CA22BE8B05378EB1C71EF320AD746E1D3B628BA79B9859F741E082542A3"
                                   "85502F25DBF55296C3A545E3872760AB7"),
            .generator_y = integer("3617DE4A96262C6F5D9E98BF9292DC29F8F41DBD289A147CE9DA3113B5F0B8C"
                                   "00A60B1CE1D7E819D7A431D7C90EA0E5F"),
            .size = 48,
        };
    }
    return std::unexpected(unsupported("unsupported ECDSA curve"));
}

auto modulo(cpp_int value, const cpp_int& modulus) -> cpp_int {
    value %= modulus;
    if (value < 0) {
        value += modulus;
    }
    return value;
}

struct point {
    cpp_int x = 0;
    cpp_int y = 1;
    cpp_int z = 0;
};

auto double_point(const point& input, const curve& parameters) -> point {
    if (input.z == 0 || input.y == 0) {
        return {};
    }
    const cpp_int delta = modulo(input.z * input.z, parameters.prime);
    const cpp_int gamma = modulo(input.y * input.y, parameters.prime);
    const cpp_int beta = modulo(input.x * gamma, parameters.prime);
    const cpp_int alpha = modulo(3 * (input.x - delta) * (input.x + delta), parameters.prime);
    point output;
    output.x = modulo(alpha * alpha - 8 * beta, parameters.prime);
    output.z = modulo((input.y + input.z) * (input.y + input.z) - gamma - delta, parameters.prime);
    output.y = modulo(alpha * (4 * beta - output.x) - 8 * gamma * gamma, parameters.prime);
    return output;
}

auto add_mixed(const point& left, const cpp_int& x, const cpp_int& y, const curve& parameters)
    -> point {
    if (left.z == 0) {
        return point{x, y, 1};
    }
    const cpp_int z_squared = modulo(left.z * left.z, parameters.prime);
    const cpp_int u = modulo(x * z_squared, parameters.prime);
    const cpp_int s = modulo(y * left.z * z_squared, parameters.prime);
    if (u == left.x) {
        if (s == left.y) {
            return double_point(left, parameters);
        }
        return {};
    }
    const cpp_int h = modulo(u - left.x, parameters.prime);
    const cpp_int hh = modulo(h * h, parameters.prime);
    const cpp_int i = modulo(4 * hh, parameters.prime);
    const cpp_int j = modulo(h * i, parameters.prime);
    const cpp_int r = modulo(2 * (s - left.y), parameters.prime);
    const cpp_int v = modulo(left.x * i, parameters.prime);
    point output;
    output.x = modulo(r * r - j - 2 * v, parameters.prime);
    output.y = modulo(r * (v - output.x) - 2 * left.y * j, parameters.prime);
    output.z = modulo((left.z + h) * (left.z + h) - z_squared - hh, parameters.prime);
    return output;
}

auto multiply(cpp_int scalar, const cpp_int& x, const cpp_int& y, const curve& parameters)
    -> point {
    point output;
    const std::size_t bits = bit_length(scalar);
    for (std::size_t offset = 0; offset < bits; ++offset) {
        const std::size_t bit = bits - 1 - offset;
        output = double_point(output, parameters);
        if (boost::multiprecision::bit_test(scalar, static_cast<unsigned int>(bit))) {
            output = add_mixed(output, x, y, parameters);
        }
    }
    return output;
}

auto affine(const point& input, const curve& parameters) -> result<std::pair<cpp_int, cpp_int>> {
    if (input.z == 0) {
        return std::unexpected(invalid("ECDSA point is at infinity"));
    }
    const cpp_int inverse =
        boost::multiprecision::powm(input.z, parameters.prime - 2, parameters.prime);
    const cpp_int inverse_squared = modulo(inverse * inverse, parameters.prime);
    return std::pair{
        modulo(input.x * inverse_squared, parameters.prime),
        modulo(input.y * inverse_squared * inverse, parameters.prime),
    };
}

auto verify_ecdsa(const public_key& key,
                  digest_kind kind,
                  std::span<const std::byte> message_digest,
                  std::span<const std::byte> signature) -> result<void> {
    auto parameters = selected_curve(key.curve);
    if (!parameters) {
        return std::unexpected(parameters.error());
    }
    if (key.first.size() != parameters->size || key.second.size() != parameters->size) {
        return std::unexpected(invalid("ECDSA public key has the wrong size"));
    }
    const cpp_int public_x = integer(key.first);
    const cpp_int public_y = integer(key.second);
    if (public_x >= parameters->prime || public_y >= parameters->prime ||
        modulo(public_y * public_y - public_x * public_x * public_x + 3 * public_x -
                   parameters->coefficient,
               parameters->prime) != 0) {
        return std::unexpected(invalid("ECDSA public key is not on the selected curve"));
    }

    asn1::reader signature_reader(signature);
    auto sequence = signature_reader.single();
    if (!sequence || sequence->identifier != asn1::tag{asn1::tag_class::universal, 16, true}) {
        return std::unexpected(invalid("ECDSA signature is not a DER sequence"));
    }
    asn1::reader values(sequence->content);
    auto r_element = values.next();
    auto s_element = values.next();
    if (!r_element || !s_element || !*r_element || !*s_element || !values.empty()) {
        return std::unexpected(invalid("ECDSA signature must contain two integers"));
    }
    auto r_bytes = asn1::unsigned_integer(**r_element);
    auto s_bytes = asn1::unsigned_integer(**s_element);
    if (!r_bytes || !s_bytes) {
        return std::unexpected(invalid("ECDSA signature integers are invalid"));
    }
    const cpp_int r = integer(*r_bytes);
    const cpp_int s = integer(*s_bytes);
    if (r <= 0 || r >= parameters->order || s <= 0 || s >= parameters->order) {
        return std::unexpected(invalid("ECDSA signature integers are out of range"));
    }

    cpp_int z = integer(message_digest);
    const std::size_t excess = message_digest.size() * 8 - bit_length(parameters->order);
    if (excess > 0) {
        z >>= excess;
    }
    const cpp_int inverse =
        boost::multiprecision::powm(s, parameters->order - 2, parameters->order);
    const cpp_int first_scalar = modulo(z * inverse, parameters->order);
    const cpp_int second_scalar = modulo(r * inverse, parameters->order);
    const point first =
        multiply(first_scalar, parameters->generator_x, parameters->generator_y, *parameters);
    const point second = multiply(second_scalar, public_x, public_y, *parameters);
    auto second_affine = affine(second, *parameters);
    if (!second_affine) {
        return std::unexpected(second_affine.error());
    }
    const point combined =
        add_mixed(first, second_affine->first, second_affine->second, *parameters);
    auto result = affine(combined, *parameters);
    if (!result || modulo(result->first, parameters->order) != r) {
        return std::unexpected(invalid("ECDSA signature is invalid"));
    }
    return {};
}

auto verify_rsa(const public_key& key,
                const signature_parameters& parameters,
                std::span<const std::byte> message_digest,
                std::span<const std::byte> signature) -> result<void> {
    const cpp_int modulus = integer(key.first);
    const cpp_int exponent = integer(key.second);
    const std::size_t modulus_size = (bit_length(modulus) + 7) / 8;
    if (modulus_size < 256 || modulus_size > 512 || exponent < 3 || (exponent & 1) == 0) {
        return std::unexpected(unsupported("RSA key must be an odd-exponent 2048-4096 bit key"));
    }
    if (signature.size() != modulus_size) {
        return std::unexpected(invalid("RSA signature has the wrong size"));
    }
    const cpp_int signature_value = integer(signature);
    if (signature_value >= modulus) {
        return std::unexpected(invalid("RSA signature is outside the modulus"));
    }
    const cpp_int encoded_value = boost::multiprecision::powm(signature_value, exponent, modulus);
    const auto encoded = octets(encoded_value, modulus_size);
    if (parameters.kind == signature_kind::rsa_pkcs1) {
        return verify_pkcs1(encoded, parameters.digest, message_digest);
    }
    return verify_pss(encoded,
                      bit_length(modulus),
                      parameters.digest,
                      parameters.pss_salt_length,
                      message_digest);
}

} // namespace

auto digest(digest_kind kind, std::span<const std::byte> input) -> result<std::vector<std::byte>> {
    if (kind == digest_kind::sha384) {
        return sha384(input);
    }
    auto value = liquid::hash(algorithm::sha256, input);
    if (!value) {
        return std::unexpected(value.error());
    }
    return std::vector<std::byte>(value->bytes.begin(), value->bytes.end());
}

auto verify_signature(const public_key& key,
                      const signature_parameters& parameters,
                      std::span<const std::byte> message,
                      std::span<const std::byte> signature) -> result<void> {
    auto message_digest = digest(parameters.digest, message);
    if (!message_digest) {
        return std::unexpected(message_digest.error());
    }
    if (parameters.kind == signature_kind::ecdsa) {
        if (key.kind != signature_kind::ecdsa) {
            return std::unexpected(invalid("ECDSA signature used with a non-ECDSA key"));
        }
        return verify_ecdsa(key, parameters.digest, *message_digest, signature);
    }
    if (key.kind == signature_kind::ecdsa) {
        return std::unexpected(invalid("RSA signature used with a non-RSA key"));
    }
    return verify_rsa(key, parameters, *message_digest, signature);
}

} // namespace liquid::crypto
