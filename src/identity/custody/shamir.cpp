#include "identity/custody/providers.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string_view>

#include "crypto/crypto.hpp"
#include "identity/custody/common.hpp"

namespace liquid::custody_detail {
namespace {

struct decoded_share {
    std::uint8_t index{};
    std::uint8_t threshold{};
    key value{};
};

auto multiply(std::uint8_t left, std::uint8_t right) -> std::uint8_t {
    auto output = std::uint8_t{};
    while (right != 0) {
        if ((right & 1U) != 0) {
            output ^= left;
        }
        const auto high = (left & 0x80U) != 0;
        left <<= 1U;
        if (high) {
            left ^= 0x1bU;
        }
        right >>= 1U;
    }
    return output;
}

auto power(std::uint8_t value, std::uint8_t exponent) -> std::uint8_t {
    auto output = std::uint8_t{1};
    while (exponent != 0) {
        if ((exponent & 1U) != 0) {
            output = multiply(output, value);
        }
        value = multiply(value, value);
        exponent >>= 1U;
    }
    return output;
}

auto divide(std::uint8_t numerator, std::uint8_t denominator) -> std::uint8_t {
    return multiply(numerator, power(denominator, 254));
}

auto parse_byte(std::string_view value) -> result<std::uint8_t> {
    auto output = unsigned{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), output);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        output == 0 || output > 255) {
        return std::unexpected(custody_error(
            errc::invalid_argument, "invalid Shamir share number", std::string{value}));
    }
    return static_cast<std::uint8_t>(output);
}

auto hex_value(char value) -> int {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

auto decode_share(std::string_view encoded) -> result<decoded_share> {
    constexpr auto prefix = std::string_view{"liquid-share-1-"};
    if (!encoded.starts_with(prefix)) {
        return std::unexpected(
            custody_error(errc::invalid_argument, "invalid Shamir share prefix", "Shamir custody"));
    }
    auto remainder = encoded.substr(prefix.size());
    const auto first = remainder.find('-');
    const auto second = first == std::string_view::npos ? first : remainder.find('-', first + 1);
    const auto third = second == std::string_view::npos ? second : remainder.find('-', second + 1);
    if (first == std::string_view::npos || second == std::string_view::npos ||
        third == std::string_view::npos ||
        remainder.find('-', third + 1) != std::string_view::npos) {
        return std::unexpected(
            custody_error(errc::invalid_argument, "invalid Shamir share fields", "Shamir custody"));
    }
    auto index = parse_byte(remainder.substr(0, first));
    auto threshold = parse_byte(remainder.substr(first + 1, second - first - 1));
    const auto material = remainder.substr(second + 1, third - second - 1);
    const auto checksum = remainder.substr(third + 1);
    const auto base = std::string{encoded.substr(0, prefix.size() + third)};
    const auto digest = sha256(as_bytes(base));
    const auto expected = hex(std::span{digest}.first<4>());
    if (!index || !threshold || material.size() != 64 || checksum != expected) {
        return std::unexpected(
            custody_error(errc::unauthorized, "invalid Shamir share", "Shamir custody"));
    }
    auto output = decoded_share{*index, *threshold, {}};
    for (auto offset = std::size_t{}; offset != output.value.size(); ++offset) {
        const auto high = hex_value(material[offset * 2]);
        const auto low = hex_value(material[offset * 2 + 1]);
        if (high < 0 || low < 0) {
            return std::unexpected(custody_error(
                errc::unauthorized, "invalid Shamir share material", "Shamir custody"));
        }
        output.value[offset] = std::byte{static_cast<unsigned char>((high << 4U) | low)};
    }
    return output;
}

auto encode_share(std::uint8_t index, std::uint8_t threshold, const key& value) -> std::string {
    auto output = "liquid-share-1-" + std::to_string(index) + "-" + std::to_string(threshold) +
                  "-" + hex(value);
    const auto digest = sha256(as_bytes(output));
    output += "-" + hex(std::span{digest}.first<4>());
    return output;
}

void wipe_shares(std::map<std::uint8_t, key>& shares) {
    for (auto& [index, value] : shares) {
        static_cast<void>(index);
        crypto_detail::wipe(value);
    }
}

auto share_key(const key& secret, std::uint8_t threshold, std::uint8_t count)
    -> result<std::vector<std::string>> {
    auto coefficients = std::vector<std::byte>(secret.size() * (threshold - 1));
    auto random = crypto_detail::random_bytes(coefficients);
    if (!random) {
        return std::unexpected(random.error());
    }
    auto output = std::vector<std::string>{};
    output.reserve(count);
    for (auto share_index = std::uint16_t{1}; share_index <= count; ++share_index) {
        auto value = key{};
        for (auto octet = std::size_t{}; octet != secret.size(); ++octet) {
            auto evaluated = std::to_integer<std::uint8_t>(secret[octet]);
            auto x_power = std::uint8_t{1};
            for (auto coefficient = std::uint8_t{1}; coefficient < threshold; ++coefficient) {
                x_power = multiply(x_power, static_cast<std::uint8_t>(share_index));
                const auto term = std::to_integer<std::uint8_t>(
                    coefficients[octet * (threshold - 1) + coefficient - 1]);
                evaluated ^= multiply(term, x_power);
            }
            value[octet] = std::byte{evaluated};
        }
        output.push_back(encode_share(static_cast<std::uint8_t>(share_index), threshold, value));
        crypto_detail::wipe(value);
    }
    crypto_detail::wipe(coefficients);
    return output;
}

auto reconstruct(std::span<const std::string> encoded) -> result<std::pair<key, std::uint8_t>> {
    auto shares = std::map<std::uint8_t, key>{};
    auto threshold = std::uint8_t{};
    for (const auto& value : encoded) {
        auto decoded = decode_share(value);
        if (!decoded) {
            wipe_shares(shares);
            return std::unexpected(decoded.error());
        }
        if (threshold == 0) {
            threshold = decoded->threshold;
        }
        const auto inserted = shares.emplace(decoded->index, decoded->value).second;
        crypto_detail::wipe(decoded->value);
        if (decoded->threshold != threshold || !inserted) {
            wipe_shares(shares);
            return std::unexpected(
                custody_error(errc::unauthorized, "incompatible Shamir shares", "Shamir custody"));
        }
    }
    if (threshold < 2 || shares.size() < threshold) {
        wipe_shares(shares);
        return std::unexpected(
            custody_error(errc::unauthorized, "not enough Shamir shares", "Shamir custody"));
    }
    while (shares.size() > threshold) {
        auto extra = std::prev(shares.end());
        crypto_detail::wipe(extra->second);
        shares.erase(extra);
    }
    auto output = key{};
    for (auto octet = std::size_t{}; octet != output.size(); ++octet) {
        auto value = std::uint8_t{};
        for (const auto& [index, share] : shares) {
            auto basis = std::uint8_t{1};
            for (const auto& [other, ignored] : shares) {
                static_cast<void>(ignored);
                if (other != index) {
                    basis = multiply(basis, divide(other, other ^ index));
                }
            }
            value ^= multiply(std::to_integer<std::uint8_t>(share[octet]), basis);
        }
        output[octet] = std::byte{value};
    }
    wipe_shares(shares);
    return std::pair{output, threshold};
}

auto split_values(std::string_view input) -> std::vector<std::string> {
    auto output = std::vector<std::string>{};
    while (!input.empty()) {
        const auto separator = input.find_first_of(",\n");
        if (const auto value = input.substr(0, separator); !value.empty()) {
            output.emplace_back(value);
        }
        if (separator == std::string_view::npos) {
            break;
        }
        input.remove_prefix(separator + 1);
    }
    return output;
}

auto share_inputs(std::string_view input) -> result<std::vector<std::string>> {
    auto output = std::vector<std::string>{};
    for (auto value : split_values(input)) {
        if (value.starts_with("liquid-share-1-")) {
            output.push_back(std::move(value));
            continue;
        }
        if (value.starts_with('@'))
            value.erase(value.begin());
        auto file = std::ifstream{value};
        if (!file) {
            return std::unexpected(custody_error(
                errc::unauthorized, "cannot read Shamir share file", std::move(value)));
        }
        const auto contents = std::string{std::istreambuf_iterator<char>{file}, {}};
        auto loaded = split_values(contents);
        if (loaded.empty() || !std::ranges::all_of(loaded, [](const std::string& share) {
                return share.starts_with("liquid-share-1-");
            })) {
            return std::unexpected(custody_error(
                errc::unauthorized, "Shamir share file is invalid", std::move(value)));
        }
        output.insert(output.end(),
                      std::make_move_iterator(loaded.begin()),
                      std::make_move_iterator(loaded.end()));
    }
    return output;
}

auto parse_parameters(std::string_view parameters)
    -> result<std::pair<std::uint8_t, std::uint8_t>> {
    const auto separator = parameters.find('/');
    if (separator == std::string_view::npos) {
        return std::unexpected(custody_error(errc::invalid_argument,
                                             "invalid Shamir threshold parameters",
                                             std::string{parameters}));
    }
    auto threshold = parse_byte(parameters.substr(0, separator));
    auto shares = parse_byte(parameters.substr(separator + 1));
    if (!threshold || !shares || *threshold < 2 || *threshold > *shares) {
        return std::unexpected(custody_error(errc::invalid_argument,
                                             "invalid Shamir threshold parameters",
                                             std::string{parameters}));
    }
    return std::pair{*threshold, *shares};
}

class shamir_provider final : public key_provider {
public:
    shamir_provider(const key& kek, std::uint8_t threshold, std::uint8_t shares)
        : kek_(kek), threshold_(threshold), shares_(shares), available_(true) {}

    shamir_provider(std::uint8_t threshold, std::uint8_t shares)
        : threshold_(threshold), shares_(shares) {}

    ~shamir_provider() override {
        crypto_detail::wipe(kek_);
    }

    auto wrap(std::span<const std::byte, 32> data_key, wrap_context context)
        -> runtime::task<std::vector<std::byte>> override {
        if (!available_) {
            co_return std::unexpected(custody_error(
                errc::unauthorized, "Shamir share quorum is unavailable", reference(context)));
        }
        co_return crypto_detail::seal(kek_, as_bytes(reference(context)), data_key);
    }

    auto unwrap(std::span<const std::byte> wrapped, wrap_context context)
        -> runtime::task<key> override {
        if (!available_) {
            co_return std::unexpected(custody_error(
                errc::unauthorized, "Shamir share quorum is unavailable", reference(context)));
        }
        auto opened = crypto_detail::open(kek_, as_bytes(reference(context)), wrapped);
        if (!opened || opened->size() != key{}.size()) {
            if (opened) {
                crypto_detail::wipe(*opened);
            }
            co_return std::unexpected(custody_error(errc::unauthorized,
                                                    "cannot unwrap data key with Shamir shares",
                                                    reference(context)));
        }
        auto output = key{};
        std::ranges::copy(*opened, output.begin());
        crypto_detail::wipe(*opened);
        co_return output;
    }

    auto custody_kind() const noexcept -> std::string_view override {
        return "shamir";
    }

protected:
    auto parameters() const -> std::vector<std::byte> override {
        return bytes(std::to_string(threshold_) + "/" + std::to_string(shares_));
    }

private:
    key kek_;
    std::uint8_t threshold_{};
    std::uint8_t shares_{};
    bool available_{};
};

} // namespace

auto make_shamir_split(std::uint8_t threshold, std::uint8_t shares) -> result<custody_split> {
    if (threshold < 2 || threshold > shares) {
        return std::unexpected(custody_error(
            errc::invalid_argument, "invalid Shamir threshold or share count", "Shamir custody"));
    }
    auto kek = crypto_detail::random_key();
    if (!kek) {
        return std::unexpected(kek.error());
    }
    auto encoded = share_key(*kek, threshold, shares);
    if (!encoded) {
        crypto_detail::wipe(*kek);
        return std::unexpected(encoded.error());
    }
    auto provider = std::make_shared<shamir_provider>(*kek, threshold, shares);
    crypto_detail::wipe(*kek);
    return custody_split{std::move(provider), std::move(*encoded)};
}

auto make_shamir_provider(std::string parameters) -> result<std::shared_ptr<key_provider>> {
    auto parsed = parse_parameters(parameters);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    auto values = share_inputs(optional_environment("LIQUID_SHAMIR_SHARES"));
    if (!values)
        return std::make_shared<shamir_provider>(parsed->first, parsed->second);
    auto reconstructed = reconstruct(*values);
    if (!reconstructed || reconstructed->second != parsed->first) {
        if (reconstructed)
            crypto_detail::wipe(reconstructed->first);
        return std::make_shared<shamir_provider>(parsed->first, parsed->second);
    }
    auto provider =
        std::make_shared<shamir_provider>(reconstructed->first, parsed->first, parsed->second);
    crypto_detail::wipe(reconstructed->first);
    return provider;
}

} // namespace liquid::custody_detail
