#include "liquid/transparency.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include <boost/capy/buffers.hpp>
#include <boost/capy/buffers/string_dynamic_buffer.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/this_coro.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/write.hpp>
#include <boost/corosio/connect.hpp>
#include <boost/corosio/openssl_stream.hpp>
#include <boost/corosio/resolver.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/tls_context.hpp>
#include <boost/http.hpp>
#include <nlohmann/json.hpp>

#include "encoding/cbor.hpp"
#include "monocypher-ed25519.h"

namespace liquid::transparency {
namespace {

using json = nlohmann::json;
using bytes = std::vector<std::byte>;

constexpr std::string_view entry_header = "LAKE-TILE-1\n";
constexpr std::string_view hash_header = "LAKE-HASH-TILE-1\n";
constexpr std::string_view checkpoint_domain = "TAAP/1 lake_checkpoint/1";

struct parsed_url {
    bool tls{};
    std::string host;
    std::string port;
    std::string path;
};

struct http_response {
    unsigned status{};
    bytes body;
};

auto failure(errc code, std::string message, std::string context = "transparency") -> error {
    return {code, std::move(message), std::move(context)};
}

auto hex(const hash& value) -> std::string {
    constexpr std::string_view digits = "0123456789abcdef";
    auto output = std::string{};
    output.reserve(value.size() * 2);
    for (const auto byte : value) {
        const auto value = std::to_integer<unsigned char>(byte);
        output.push_back(digits[value >> 4U]);
        output.push_back(digits[value & 0x0fU]);
    }
    return output;
}

auto decode_hex(std::string_view value) -> result<hash> {
    if (value.size() != hash{}.size() * 2) {
        return std::unexpected(failure(errc::corrupt, "transparency hash has invalid length"));
    }
    const auto nibble = [](char character) -> int {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        return -1;
    };
    auto output = hash{};
    for (auto index = std::size_t{}; index < output.size(); ++index) {
        const auto high = nibble(value[index * 2]);
        const auto low = nibble(value[index * 2 + 1]);
        if (high < 0 || low < 0) {
            return std::unexpected(
                failure(errc::corrupt, "transparency hash is not lowercase hex"));
        }
        output[index] = std::byte{static_cast<unsigned char>((high << 4U) | low)};
    }
    return output;
}

auto decode_hex_bytes(std::string_view value) -> result<bytes> {
    if (value.empty() || value.size() % 2 != 0) {
        return std::unexpected(failure(errc::corrupt, "hex value has invalid length"));
    }
    const auto nibble = [](char character) -> int {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        return -1;
    };
    auto output = bytes{};
    output.reserve(value.size() / 2);
    for (auto index = std::size_t{}; index < value.size(); index += 2) {
        const auto high = nibble(value[index]);
        const auto low = nibble(value[index + 1]);
        if (high < 0 || low < 0) {
            return std::unexpected(failure(errc::corrupt, "hex value is not lowercase hex"));
        }
        output.push_back(std::byte{static_cast<unsigned char>((high << 4U) | low)});
    }
    return output;
}

auto sha256(std::span<const std::byte> value) -> result<hash> {
    auto calculated = liquid::hash(algorithm::sha256, value);
    if (!calculated) {
        return std::unexpected(calculated.error());
    }
    return calculated->bytes;
}

auto leaf_hash(std::span<const std::byte> value) -> result<hash> {
    auto input = bytes{};
    input.reserve(value.size() + 1);
    input.push_back(std::byte{0});
    input.insert(input.end(), value.begin(), value.end());
    return sha256(input);
}

auto node_hash(const hash& left, const hash& right) -> result<hash> {
    auto input = bytes{};
    input.reserve(1 + left.size() + right.size());
    input.push_back(std::byte{1});
    input.insert(input.end(), left.begin(), left.end());
    input.insert(input.end(), right.begin(), right.end());
    return sha256(input);
}

auto verify_tile_hash(const tile& value) -> result<void> {
    auto digest = sha256(value.bytes);
    if (!digest || *digest != value.sha256) {
        return std::unexpected(failure(errc::corrupt, "tile hash does not match its bytes"));
    }
    return {};
}

auto base64url_decode(std::string value) -> result<bytes> {
    if (value.empty() || value.size() % 4 == 1) {
        return std::unexpected(failure(errc::corrupt, "invalid base64url value"));
    }
    for (auto& character : value) {
        if (character == '-') {
            character = '+';
        } else if (character == '_') {
            character = '/';
        }
    }
    if (const auto remainder = value.size() % 4; remainder != 0) {
        value.append(4 - remainder, '=');
    }
    const auto decode = [](char character) -> int {
        if (character >= 'A' && character <= 'Z') {
            return character - 'A';
        }
        if (character >= 'a' && character <= 'z') {
            return character - 'a' + 26;
        }
        if (character >= '0' && character <= '9') {
            return character - '0' + 52;
        }
        if (character == '+') {
            return 62;
        }
        if (character == '/') {
            return 63;
        }
        return -1;
    };
    auto output = bytes{};
    output.reserve((value.size() / 4) * 3);
    for (auto index = std::size_t{}; index < value.size(); index += 4) {
        const auto first = decode(value[index]);
        const auto second = decode(value[index + 1]);
        const auto third = value[index + 2] == '=' ? -2 : decode(value[index + 2]);
        const auto fourth = value[index + 3] == '=' ? -2 : decode(value[index + 3]);
        if (first < 0 || second < 0 || third == -1 || fourth == -1 ||
            (third == -2 && fourth != -2) ||
            ((third == -2 || fourth == -2) && index + 4 != value.size())) {
            return std::unexpected(failure(errc::corrupt, "invalid base64url value"));
        }
        const auto group = (static_cast<std::uint32_t>(first) << 18U) |
                           (static_cast<std::uint32_t>(second) << 12U) |
                           (third < 0 ? 0U : static_cast<std::uint32_t>(third) << 6U) |
                           (fourth < 0 ? 0U : static_cast<std::uint32_t>(fourth));
        output.push_back(std::byte{static_cast<unsigned char>(group >> 16U)});
        if (third >= 0) {
            output.push_back(std::byte{static_cast<unsigned char>(group >> 8U)});
        }
        if (fourth >= 0) {
            output.push_back(std::byte{static_cast<unsigned char>(group)});
        }
    }
    return output;
}

auto to_bytes(std::string_view value) -> bytes {
    return {reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

auto field(const json& object, std::string_view name) -> result<const json*> {
    if (!object.is_object()) {
        return std::unexpected(failure(errc::corrupt, "transparency response is not an object"));
    }
    const auto found = object.find(name);
    if (found == object.end()) {
        return std::unexpected(
            failure(errc::corrupt, "transparency response is missing " + std::string{name}));
    }
    return &*found;
}

auto text_field(const json& object, std::string_view name) -> result<std::string> {
    auto value = field(object, name);
    if (!value) {
        return std::unexpected(value.error());
    }
    if (!(*value)->is_string()) {
        return std::unexpected(
            failure(errc::corrupt, "transparency field is not text", std::string{name}));
    }
    return (*value)->get<std::string>();
}

auto unsigned_field(const json& object, std::string_view name) -> result<std::uint64_t> {
    auto value = field(object, name);
    if (!value) {
        return std::unexpected(value.error());
    }
    if (!(*value)->is_number_unsigned()) {
        return std::unexpected(failure(
            errc::corrupt, "transparency field is not an unsigned integer", std::string{name}));
    }
    return (*value)->get<std::uint64_t>();
}

auto hash_field(const json& object, std::string_view name) -> result<hash> {
    auto value = text_field(object, name);
    if (!value) {
        return std::unexpected(value.error());
    }
    return decode_hex(*value);
}

auto parse_witness(const json& value) -> result<witness_signature> {
    const auto endpoint = text_field(value, "endpoint");
    const auto key_text = text_field(value, "public_key");
    const auto signature_text = text_field(value, "signature");
    if (!endpoint || !key_text || !signature_text) {
        return std::unexpected(!endpoint   ? endpoint.error()
                               : !key_text ? key_text.error()
                                           : signature_text.error());
    }
    auto key = decode_hex(*key_text);
    auto signature = decode_hex_bytes(*signature_text);
    if (!signature) {
        signature = base64url_decode(*signature_text);
    }
    if (!key || !signature || signature->size() != 64) {
        return std::unexpected(failure(errc::corrupt, "invalid witness signature"));
    }
    return witness_signature{*endpoint, *key, std::move(*signature)};
}

auto parse_checkpoint(const json& record) -> result<checkpoint> {
    const auto checkpoint_value = field(record, "checkpoint");
    const auto sequence = unsigned_field(record, "sequence");
    const auto checkpoint_hash = hash_field(record, "checkpoint_hash_sha256");
    const auto signature_value = field(record, "signature");
    const auto witnesses = field(record, "witness_signatures");
    if (!checkpoint_value || !sequence || !checkpoint_hash || !signature_value || !witnesses) {
        return std::unexpected(!checkpoint_value  ? checkpoint_value.error()
                               : !sequence        ? sequence.error()
                               : !checkpoint_hash ? checkpoint_hash.error()
                               : !signature_value ? signature_value.error()
                                                  : witnesses.error());
    }
    if (!(*checkpoint_value)->is_object() || !(*signature_value)->is_object() ||
        !(*witnesses)->is_array()) {
        return std::unexpected(failure(errc::corrupt, "invalid checkpoint record"));
    }
    const auto version = text_field(**checkpoint_value, "version");
    const auto algorithm = text_field(**checkpoint_value, "hash_algorithm");
    const auto tree_size = unsigned_field(**checkpoint_value, "tree_size");
    const auto entry_width = unsigned_field(**checkpoint_value, "tile_width");
    const auto hash_width = unsigned_field(**checkpoint_value, "hash_tile_width");
    const auto root_hash = hash_field(**checkpoint_value, "root_hash");
    const auto signature_algorithm = text_field(**signature_value, "algorithm");
    const auto signer = text_field(**signature_value, "signer");
    const auto signature = text_field(**signature_value, "signature");
    if (!version || !algorithm || !tree_size || !entry_width || !hash_width || !root_hash ||
        !signature_algorithm || !signer || !signature) {
        return std::unexpected(!version               ? version.error()
                               : !algorithm           ? algorithm.error()
                               : !tree_size           ? tree_size.error()
                               : !entry_width         ? entry_width.error()
                               : !hash_width          ? hash_width.error()
                               : !root_hash           ? root_hash.error()
                               : !signature_algorithm ? signature_algorithm.error()
                               : !signer              ? signer.error()
                                                      : signature.error());
    }
    if (*version != "lake_checkpoint/1" || *algorithm != "sha256" ||
        *signature_algorithm != "ed25519") {
        return std::unexpected(failure(errc::corrupt, "unsupported checkpoint format"));
    }
    auto decoded_signature = base64url_decode(*signature);
    if (!decoded_signature || decoded_signature->size() != 64) {
        return std::unexpected(failure(errc::corrupt, "invalid checkpoint signature"));
    }
    auto parsed_witnesses = std::vector<witness_signature>{};
    parsed_witnesses.reserve((*witnesses)->size());
    for (const auto& value : **witnesses) {
        auto parsed = parse_witness(value);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        parsed_witnesses.push_back(std::move(*parsed));
    }
    const auto body = (*checkpoint_value)->dump();
    return checkpoint{*sequence,
                      *tree_size,
                      *entry_width,
                      *hash_width,
                      *root_hash,
                      *checkpoint_hash,
                      {*signer, std::move(*decoded_signature)},
                      std::move(parsed_witnesses),
                      to_bytes(body)};
}

auto parse_tile(const json& value) -> result<tile> {
    const auto version = text_field(value, "version");
    const auto kind = text_field(value, "kind");
    const auto encoding = text_field(value, "encoding");
    const auto level = unsigned_field(value, "level");
    const auto index = unsigned_field(value, "index");
    const auto encoded = text_field(value, "tile");
    const auto digest = hash_field(value, "sha256");
    if (!version || !kind || !encoding || !level || !index || !encoded || !digest) {
        return std::unexpected(!version    ? version.error()
                               : !kind     ? kind.error()
                               : !encoding ? encoding.error()
                               : !level    ? level.error()
                               : !index    ? index.error()
                               : !encoded  ? encoded.error()
                                           : digest.error());
    }
    if (*level > std::numeric_limits<std::uint32_t>::max() || *encoding != "base64url" ||
        (*level == 0 && (*version != "lake_tile/1" || *kind != "entries")) ||
        (*level != 0 && (*version != "lake_hash_tile/1" || *kind != "hashes"))) {
        return std::unexpected(failure(errc::corrupt, "invalid transparency tile metadata"));
    }
    const auto width = unsigned_field(value, *level == 0 ? "entries" : "hashes");
    auto decoded = base64url_decode(*encoded);
    if (!width || !decoded || *width == 0 || *width > tile_width) {
        return std::unexpected(!width ? width.error()
                               : !decoded
                                   ? decoded.error()
                                   : failure(errc::corrupt, "invalid transparency tile width"));
    }
    auto actual = sha256(*decoded);
    if (!actual || *actual != *digest) {
        return std::unexpected(failure(errc::corrupt, "transparency tile digest does not match"));
    }
    return tile{static_cast<std::uint32_t>(*level), *index, *width, *digest, std::move(*decoded)};
}

auto parse_hash_path(const json& value) -> result<std::vector<hash>> {
    const auto path = field(value, "audit_path");
    if (!path) {
        return std::unexpected(path.error());
    }
    if (!(*path)->is_array()) {
        return std::unexpected(failure(errc::corrupt, "proof audit path is not an array"));
    }
    auto output = std::vector<hash>{};
    output.reserve((*path)->size());
    for (const auto& item : **path) {
        if (!item.is_string()) {
            return std::unexpected(failure(errc::corrupt, "proof audit path entry is not text"));
        }
        auto decoded = decode_hex(item.get<std::string>());
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        output.push_back(*decoded);
    }
    return output;
}

auto parse_inclusion(const json& value) -> result<inclusion_proof> {
    const auto version = text_field(value, "version");
    const auto algorithm = text_field(value, "hash_algorithm");
    const auto leaf_index = unsigned_field(value, "leaf_index");
    const auto tree_size = unsigned_field(value, "tree_size");
    const auto root_hash = hash_field(value, "root_hash");
    const auto checkpoint_value = field(value, "checkpoint");
    const auto path = parse_hash_path(value);
    if (!version || !algorithm || !leaf_index || !tree_size || !root_hash || !checkpoint_value ||
        !path) {
        return std::unexpected(!version            ? version.error()
                               : !algorithm        ? algorithm.error()
                               : !leaf_index       ? leaf_index.error()
                               : !tree_size        ? tree_size.error()
                               : !root_hash        ? root_hash.error()
                               : !checkpoint_value ? checkpoint_value.error()
                                                   : path.error());
    }
    if (*version != "lake_inclusion_proof/1" || *algorithm != "sha256") {
        return std::unexpected(failure(errc::corrupt, "unsupported inclusion proof format"));
    }
    auto checkpoint = parse_checkpoint(**checkpoint_value);
    if (!checkpoint) {
        return std::unexpected(checkpoint.error());
    }
    return inclusion_proof{
        *leaf_index, *tree_size, *root_hash, std::move(*path), std::move(*checkpoint)};
}

auto parse_consistency(const json& value) -> result<consistency_proof> {
    const auto version = text_field(value, "version");
    const auto algorithm = text_field(value, "hash_algorithm");
    const auto old_size = unsigned_field(value, "old_tree_size");
    const auto new_size = unsigned_field(value, "new_tree_size");
    const auto old_root = hash_field(value, "old_root_hash");
    const auto new_root = hash_field(value, "new_root_hash");
    const auto checkpoint_value = field(value, "checkpoint");
    const auto path = parse_hash_path(value);
    if (!version || !algorithm || !old_size || !new_size || !old_root || !new_root ||
        !checkpoint_value || !path) {
        return std::unexpected(!version            ? version.error()
                               : !algorithm        ? algorithm.error()
                               : !old_size         ? old_size.error()
                               : !new_size         ? new_size.error()
                               : !old_root         ? old_root.error()
                               : !new_root         ? new_root.error()
                               : !checkpoint_value ? checkpoint_value.error()
                                                   : path.error());
    }
    if (*version != "lake_consistency_proof/1" || *algorithm != "sha256") {
        return std::unexpected(failure(errc::corrupt, "unsupported consistency proof format"));
    }
    auto checkpoint = parse_checkpoint(**checkpoint_value);
    if (!checkpoint) {
        return std::unexpected(checkpoint.error());
    }
    return consistency_proof{
        *old_size, *new_size, *old_root, *new_root, std::move(*path), std::move(*checkpoint)};
}

auto parse_url(std::string_view input) -> result<parsed_url> {
    auto output = parsed_url{};
    if (input.starts_with("https://")) {
        output.tls = true;
        output.port = "443";
        input.remove_prefix(8);
    } else if (input.starts_with("http://")) {
        output.port = "80";
        input.remove_prefix(7);
    } else {
        return std::unexpected(failure(errc::invalid_argument, "log URL must use http or https"));
    }
    const auto slash = input.find('/');
    auto authority = input.substr(0, slash);
    output.path =
        slash == std::string_view::npos ? std::string{} : std::string{input.substr(slash)};
    while (output.path.size() > 1 && output.path.ends_with('/')) {
        output.path.pop_back();
    }
    if (authority.empty() || authority.contains('@')) {
        return std::unexpected(failure(errc::invalid_argument, "invalid log URL authority"));
    }
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos ||
            (close + 1 < authority.size() && authority[close + 1] != ':')) {
            return std::unexpected(failure(errc::invalid_argument, "invalid IPv6 log URL"));
        }
        output.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            output.port = authority.substr(close + 2);
        }
    } else if (const auto colon = authority.rfind(':'); colon != std::string_view::npos) {
        output.host = authority.substr(0, colon);
        output.port = authority.substr(colon + 1);
    } else {
        output.host = authority;
    }
    if (output.host.empty() || output.port.empty()) {
        return std::unexpected(failure(errc::invalid_argument, "log URL host or port is empty"));
    }
    return output;
}

auto header_value(std::string_view header, std::string_view name) -> std::string_view {
    auto offset = header.find("\r\n") + 2;
    while (offset < header.size()) {
        const auto end = header.find("\r\n", offset);
        if (end == offset || end == std::string_view::npos) {
            break;
        }
        const auto colon = header.find(':', offset);
        if (colon != std::string_view::npos && colon < end) {
            const auto field = header.substr(offset, colon - offset);
            auto matches = field.size() == name.size();
            for (auto index = std::size_t{}; matches && index < field.size(); ++index) {
                matches = std::tolower(static_cast<unsigned char>(field[index])) ==
                          std::tolower(static_cast<unsigned char>(name[index]));
            }
            if (matches) {
                auto output = header.substr(colon + 1, end - colon - 1);
                while (!output.empty() && (output.front() == ' ' || output.front() == '\t')) {
                    output.remove_prefix(1);
                }
                return output;
            }
        }
        offset = end + 2;
    }
    return {};
}

auto decode_chunked(std::string_view input) -> result<bytes> {
    auto output = bytes{};
    auto offset = std::size_t{};
    for (;;) {
        const auto line_end = input.find("\r\n", offset);
        if (line_end == std::string_view::npos) {
            return std::unexpected(failure(errc::corrupt, "truncated HTTP chunk size"));
        }
        const auto extension = input.find(';', offset);
        const auto size_end =
            extension != std::string_view::npos && extension < line_end ? extension : line_end;
        auto size = std::uint64_t{};
        const auto parsed =
            std::from_chars(input.data() + offset, input.data() + size_end, size, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != input.data() + size_end ||
            size > std::numeric_limits<std::size_t>::max()) {
            return std::unexpected(failure(errc::corrupt, "invalid HTTP chunk size"));
        }
        offset = line_end + 2;
        if (size == 0) {
            return output;
        }
        if (size > input.size() - offset || input.size() - offset - size < 2 ||
            input.substr(offset + size, 2) != "\r\n") {
            return std::unexpected(failure(errc::corrupt, "truncated HTTP chunk"));
        }
        const auto* first = reinterpret_cast<const std::byte*>(input.data() + offset);
        output.insert(output.end(), first, first + size);
        offset += static_cast<std::size_t>(size) + 2;
    }
}

auto parse_response(std::string wire) -> result<http_response> {
    const auto header_end = wire.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return std::unexpected(failure(errc::corrupt, "truncated HTTP response header"));
    }
    const auto header_text = std::string_view{wire}.substr(0, header_end + 4);
    const auto header = boost::http::response{header_text};
    const auto body = std::string_view{wire}.substr(header_end + 4);
    auto output = http_response{.status = static_cast<unsigned>(header.status())};
    if (header_value(header_text, "transfer-encoding") == "chunked") {
        auto decoded = decode_chunked(body);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        output.body = std::move(*decoded);
        return output;
    }
    const auto length_text = header_value(header_text, "content-length");
    if (!length_text.empty()) {
        auto length = std::uint64_t{};
        const auto parsed =
            std::from_chars(length_text.data(), length_text.data() + length_text.size(), length);
        if (parsed.ec != std::errc{} || parsed.ptr != length_text.data() + length_text.size() ||
            length != body.size()) {
            return std::unexpected(failure(errc::corrupt, "invalid HTTP content length"));
        }
    }
    output.body = to_bytes(body);
    return output;
}

template <boost::capy::Stream Stream>
auto exchange_http(Stream& stream, std::string wire) -> runtime::task<http_response> {
    auto [write_error, written] =
        co_await boost::capy::write(stream, boost::capy::const_buffer(wire.data(), wire.size()));
    if (write_error || written != wire.size()) {
        co_return std::unexpected(failure(
            errc::io, write_error ? write_error.message() : "short HTTP write", "log HTTP"));
    }
    auto response = std::string{};
    auto [read_error, read] =
        co_await boost::capy::read(stream, boost::capy::string_dynamic_buffer(&response));
    static_cast<void>(read);
    if (read_error && read_error != boost::capy::error::eof) {
        co_return std::unexpected(failure(errc::io, read_error.message(), "log HTTP"));
    }
    co_return parse_response(std::move(response));
}

auto perform_get(parsed_url url, std::string target) -> runtime::task<http_response> {
    try {
        const auto executor = co_await boost::capy::this_coro::executor;
        auto resolver = boost::corosio::resolver{executor.context()};
        auto [resolve_error, endpoints] = co_await resolver.resolve(url.host, url.port);
        if (resolve_error) {
            co_return std::unexpected(
                failure(errc::unavailable, resolve_error.message(), url.host));
        }
        auto socket = boost::corosio::tcp_socket{executor.context()};
        auto [connect_error, endpoint] = co_await boost::corosio::connect(socket, endpoints);
        static_cast<void>(endpoint);
        if (connect_error) {
            co_return std::unexpected(
                failure(errc::unavailable, connect_error.message(), url.host));
        }
        auto authority = url.host;
        if ((!url.tls && url.port != "80") || (url.tls && url.port != "443")) {
            authority += ':' + url.port;
        }
        auto request = boost::http::request{boost::http::method::get, target};
        request.set(boost::http::field::host, authority);
        request.set(boost::http::field::connection, "close");
        const auto wire = std::string{request.buffer()};
        if (!url.tls) {
            co_return co_await exchange_http(socket, wire);
        }
        auto context = boost::corosio::tls_context{};
        context.set_hostname(url.host);
        if (const auto code = context.set_default_verify_paths(); code) {
            co_return std::unexpected(failure(errc::io, code.message(), url.host));
        }
        if (const auto code = context.set_verify_mode(boost::corosio::tls_verify_mode::peer);
            code) {
            co_return std::unexpected(failure(errc::io, code.message(), url.host));
        }
        auto secure = boost::corosio::openssl_stream{&socket, context};
        auto [handshake_error] = co_await secure.handshake(boost::corosio::openssl_stream::client);
        if (handshake_error) {
            co_return std::unexpected(
                failure(errc::unavailable, handshake_error.message(), url.host));
        }
        auto response = co_await exchange_http(secure, wire);
        auto [shutdown_error] = co_await secure.shutdown();
        if (shutdown_error && shutdown_error != boost::capy::error::eof && response) {
            co_return std::unexpected(failure(errc::io, shutdown_error.message(), url.host));
        }
        co_return response;
    } catch (const std::exception& exception) {
        co_return std::unexpected(failure(errc::internal, exception.what(), "log HTTP"));
    }
}

auto parse_value(std::span<const std::byte> body) -> result<json> {
    const auto text = std::string{reinterpret_cast<const char*>(body.data()), body.size()};
    const auto parsed = json::parse(text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return std::unexpected(failure(errc::corrupt, "log endpoint returned invalid JSON"));
    }
    if (const auto ok = parsed.find("ok"); ok != parsed.end()) {
        if (!ok->is_boolean() || !ok->get<bool>()) {
            return std::unexpected(failure(errc::unavailable, "log endpoint refused request"));
        }
        const auto value = parsed.find("value");
        if (value == parsed.end()) {
            return std::unexpected(failure(errc::corrupt, "log endpoint response has no value"));
        }
        return *value;
    }
    return parsed;
}

auto query_target(const parsed_url& url, std::string_view suffix, std::string_view query = {})
    -> std::string {
    auto output = url.path == "/" ? std::string{} : url.path;
    output += suffix;
    if (!query.empty()) {
        output += '?';
        output += query;
    }
    return output;
}

auto merkle_root(std::span<const std::vector<std::byte>> entries,
                 std::size_t first,
                 std::size_t count) -> result<hash> {
    if (count == 0) {
        return sha256({});
    }
    if (count == 1) {
        return leaf_hash(entries[first]);
    }
    auto split = std::size_t{1};
    while (split * 2 < count) {
        split *= 2;
    }
    const auto left = merkle_root(entries, first, split);
    const auto right = merkle_root(entries, first + split, count - split);
    if (!left || !right) {
        return std::unexpected(!left ? left.error() : right.error());
    }
    return node_hash(*left, *right);
}

auto inclusion_root(const hash& leaf,
                    std::uint64_t leaf_index,
                    std::uint64_t tree_size,
                    std::span<const hash> path,
                    std::size_t& position) -> result<hash> {
    if (tree_size == 1) {
        return leaf;
    }
    auto split = std::uint64_t{1};
    while (split * 2 < tree_size) {
        split *= 2;
    }
    if (leaf_index < split) {
        auto child = inclusion_root(leaf, leaf_index, split, path, position);
        if (!child || position == path.size()) {
            return std::unexpected(!child ? child.error()
                                          : failure(errc::corrupt, "inclusion path is too short"));
        }
        return node_hash(*child, path[position++]);
    }
    auto child = inclusion_root(leaf, leaf_index - split, tree_size - split, path, position);
    if (!child || position == path.size()) {
        return std::unexpected(!child ? child.error()
                                      : failure(errc::corrupt, "inclusion path is too short"));
    }
    return node_hash(path[position++], *child);
}

auto same_checkpoint(const checkpoint& left, const checkpoint& right) -> bool {
    return left.sequence == right.sequence && left.tree_size == right.tree_size &&
           left.root_hash == right.root_hash && left.checkpoint_hash == right.checkpoint_hash;
}

auto metadata_key(std::string_view log_name) -> result<std::string> {
    auto name_bytes = to_bytes(log_name);
    auto digest = sha256(name_bytes);
    if (!digest) {
        return std::unexpected(digest.error());
    }
    return "transparency/checkpoint/" + hex(*digest);
}

auto encode_trusted(const checkpoint& value) -> result<bytes> {
    namespace cbor = encoding::cbor;
    auto record = cbor::value::map{};
    record.emplace_back("version", "liquid_trusted_checkpoint/1");
    record.emplace_back("sequence", value.sequence);
    record.emplace_back("tree_size", value.tree_size);
    record.emplace_back("root_hash",
                        cbor::value::bytes{value.root_hash.begin(), value.root_hash.end()});
    record.emplace_back(
        "checkpoint_hash",
        cbor::value::bytes{value.checkpoint_hash.begin(), value.checkpoint_hash.end()});
    return cbor::encode(cbor::value{std::move(record)});
}

struct trusted_state {
    std::uint64_t sequence{};
    std::uint64_t tree_size{};
    hash root_hash{};
    hash checkpoint_hash{};
};

auto cbor_field(const encoding::cbor::value& value, std::string_view name)
    -> const encoding::cbor::value* {
    const auto* map = std::get_if<encoding::cbor::value::map>(&value.data);
    if (!map) {
        return nullptr;
    }
    for (const auto& [key, child] : *map) {
        const auto* text = std::get_if<std::string>(&key.data);
        if (text && *text == name) {
            return &child;
        }
    }
    return nullptr;
}

auto decode_trusted(std::span<const std::byte> input) -> result<trusted_state> {
    auto decoded = encoding::cbor::decode(input);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    const auto* version = cbor_field(*decoded, "version");
    const auto* sequence = cbor_field(*decoded, "sequence");
    const auto* tree_size = cbor_field(*decoded, "tree_size");
    const auto* root = cbor_field(*decoded, "root_hash");
    const auto* checkpoint_hash = cbor_field(*decoded, "checkpoint_hash");
    const auto* version_text = version ? std::get_if<std::string>(&version->data) : nullptr;
    const auto* sequence_value = sequence ? std::get_if<std::uint64_t>(&sequence->data) : nullptr;
    const auto* tree_size_value =
        tree_size ? std::get_if<std::uint64_t>(&tree_size->data) : nullptr;
    const auto* root_value =
        root ? std::get_if<encoding::cbor::value::bytes>(&root->data) : nullptr;
    const auto* hash_value = checkpoint_hash
                                 ? std::get_if<encoding::cbor::value::bytes>(&checkpoint_hash->data)
                                 : nullptr;
    if (!version_text || *version_text != "liquid_trusted_checkpoint/1" || !sequence_value ||
        !tree_size_value || !root_value || root_value->size() != hash{}.size() || !hash_value ||
        hash_value->size() != hash{}.size()) {
        return std::unexpected(failure(errc::corrupt, "invalid trusted checkpoint record"));
    }
    auto root_hash = hash{};
    auto record_hash = hash{};
    std::ranges::copy(*root_value, root_hash.begin());
    std::ranges::copy(*hash_value, record_hash.begin());
    return trusted_state{*sequence_value, *tree_size_value, root_hash, record_hash};
}

auto trusted_checkpoint(repository& source, std::string log_name)
    -> runtime::task<std::optional<trusted_state>> {
    auto key = metadata_key(log_name);
    if (!key) {
        co_return std::unexpected(key.error());
    }
    auto stored = co_await source.metadata(std::move(*key));
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    if (!*stored) {
        co_return std::optional<trusted_state>{};
    }
    auto decoded = decode_trusted(**stored);
    if (!decoded) {
        co_return std::unexpected(decoded.error());
    }
    co_return std::optional<trusted_state>{*decoded};
}

auto persist_checkpoint(repository& destination, std::string log_name, const checkpoint& value)
    -> runtime::task<void> {
    auto key = metadata_key(log_name);
    auto record = encode_trusted(value);
    if (!key || !record) {
        co_return std::unexpected(!key ? key.error() : record.error());
    }
    co_return co_await destination.set_metadata(std::move(*key), std::move(*record));
}

auto verify_canonical_attestation(std::span<const std::byte> encoded) -> result<attestation> {
    auto decoded = decode_attestation(encoded);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    auto canonical = encode_attestation(*decoded);
    if (!canonical || !std::ranges::equal(*canonical, encoded)) {
        return std::unexpected(failure(errc::corrupt, "attestation is not canonical TAAP"));
    }
    return *decoded;
}

auto names_identity(const attestation& value, std::string_view identity) -> bool {
    if (value.issuer.taap_id == identity) {
        return true;
    }
    for (const auto& attribute : value.attributes) {
        if (const auto* text = std::get_if<std::string>(&attribute.value);
            text && *text == identity) {
            return true;
        }
        if (const auto* list = std::get_if<std::vector<std::string>>(&attribute.value)) {
            if (std::ranges::find(*list, identity) != list->end()) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

result<std::vector<std::vector<std::byte>>> tile::entries() const {
    auto verified = verify_tile_hash(*this);
    if (!verified || level != 0 || width == 0 || width > tile_width ||
        bytes.size() < entry_header.size() ||
        !std::ranges::equal(
            entry_header,
            std::string_view{reinterpret_cast<const char*>(bytes.data()), entry_header.size()})) {
        return std::unexpected(failure(errc::corrupt, "invalid entry tile header"));
    }
    auto output = std::vector<std::vector<std::byte>>{};
    output.reserve(static_cast<std::size_t>(width));
    auto offset = entry_header.size();
    while (offset < bytes.size()) {
        if (bytes.size() - offset < 4) {
            return std::unexpected(failure(errc::corrupt, "truncated entry tile record"));
        }
        const auto length = (std::to_integer<std::uint32_t>(bytes[offset]) << 24U) |
                            (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 16U) |
                            (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 8U) |
                            std::to_integer<std::uint32_t>(bytes[offset + 3]);
        offset += 4;
        if (length == 0 || length > bytes.size() - offset) {
            return std::unexpected(failure(errc::corrupt, "invalid entry tile record"));
        }
        output.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                            bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
        offset += length;
    }
    if (output.size() != width) {
        return std::unexpected(
            failure(errc::corrupt, "entry tile width does not match record count"));
    }
    return output;
}

result<std::vector<hash>> tile::hashes() const {
    auto verified = verify_tile_hash(*this);
    if (!verified || level == 0 || width == 0 || width > tile_width ||
        bytes.size() != hash_header.size() + width * hash{}.size() ||
        !std::ranges::equal(
            hash_header,
            std::string_view{reinterpret_cast<const char*>(bytes.data()), hash_header.size()})) {
        return std::unexpected(failure(errc::corrupt, "invalid hash tile"));
    }
    auto output = std::vector<hash>{};
    output.reserve(static_cast<std::size_t>(width));
    auto offset = hash_header.size();
    for (auto index = std::uint64_t{}; index < width; ++index) {
        auto value = hash{};
        std::ranges::copy_n(
            bytes.begin() + static_cast<std::ptrdiff_t>(offset), value.size(), value.begin());
        output.push_back(value);
        offset += value.size();
    }
    return output;
}

result<void> checkpoint::verify(const log_trust& trust) const {
    if (entry_tile_width != tile_width || hash_tile_width != tile_width || canonical_body.empty() ||
        signature.bytes.size() != 64 || signature.signer != trust.identity.taap_id ||
        trust.identity.algorithm != "ed25519" || trust.identity.public_key.size() != 32) {
        return std::unexpected(failure(errc::unauthorized, "invalid checkpoint trust binding"));
    }
    auto calculated_hash = sha256(canonical_body);
    if (!calculated_hash || *calculated_hash != checkpoint_hash) {
        return std::unexpected(failure(errc::corrupt, "checkpoint hash does not match its body"));
    }
    const auto body_text =
        std::string{reinterpret_cast<const char*>(canonical_body.data()), canonical_body.size()};
    const auto body = json::parse(body_text, nullptr, false);
    if (body.is_discarded() || body.dump() != body_text) {
        return std::unexpected(failure(errc::corrupt, "checkpoint body is not canonical JSON"));
    }
    const auto cbor = json::to_cbor(body);
    auto signing_input = bytes{};
    signing_input.reserve(checkpoint_domain.size() + 1 + cbor.size());
    signing_input.insert(
        signing_input.end(),
        reinterpret_cast<const std::byte*>(checkpoint_domain.data()),
        reinterpret_cast<const std::byte*>(checkpoint_domain.data() + checkpoint_domain.size()));
    signing_input.push_back(std::byte{0});
    signing_input.insert(signing_input.end(),
                         reinterpret_cast<const std::byte*>(cbor.data()),
                         reinterpret_cast<const std::byte*>(cbor.data() + cbor.size()));
    if (crypto_ed25519_check(
            reinterpret_cast<const std::uint8_t*>(signature.bytes.data()),
            reinterpret_cast<const std::uint8_t*>(trust.identity.public_key.data()),
            reinterpret_cast<const std::uint8_t*>(signing_input.data()),
            signing_input.size()) != 0) {
        return std::unexpected(failure(errc::unauthorized, "checkpoint log signature is invalid"));
    }
    auto distinct = std::vector<std::array<std::byte, 32>>{};
    for (const auto& signed_witness : witness_signatures) {
        const auto configured = std::ranges::find_if(
            trust.configuration.witnesses, [&signed_witness](const witness& expected) {
                return expected.endpoint == signed_witness.endpoint &&
                       expected.public_key == signed_witness.public_key;
            });
        if (configured == trust.configuration.witnesses.end() ||
            signed_witness.bytes.size() != 64 ||
            std::ranges::find(distinct, signed_witness.public_key) != distinct.end()) {
            continue;
        }
        if (crypto_ed25519_check(
                reinterpret_cast<const std::uint8_t*>(signed_witness.bytes.data()),
                reinterpret_cast<const std::uint8_t*>(signed_witness.public_key.data()),
                reinterpret_cast<const std::uint8_t*>(canonical_body.data()),
                canonical_body.size()) == 0) {
            distinct.push_back(signed_witness.public_key);
        }
    }
    if (distinct.size() < trust.configuration.witness_threshold) {
        return std::unexpected(failure(errc::unauthorized,
                                       "checkpoint witness threshold not met",
                                       std::to_string(distinct.size()) + "/" +
                                           std::to_string(trust.configuration.witness_threshold)));
    }
    return {};
}

result<void> inclusion_proof::verify(std::span<const std::byte> attestation,
                                     const checkpoint& selected) const {
    if (tree_size == 0 || leaf_index >= tree_size || root_hash != selected.root_hash ||
        tree_size != selected.tree_size || !same_checkpoint(bound_checkpoint, selected)) {
        return std::unexpected(
            failure(errc::corrupt, "inclusion proof is not bound to checkpoint"));
    }
    auto leaf = leaf_hash(attestation);
    if (!leaf) {
        return std::unexpected(leaf.error());
    }
    auto position = std::size_t{};
    auto root = inclusion_root(*leaf, leaf_index, tree_size, audit_path, position);
    if (!root || position != audit_path.size() || *root != root_hash) {
        return std::unexpected(failure(errc::corrupt, "inclusion proof does not verify"));
    }
    return {};
}

result<void> consistency_proof::verify(const checkpoint& old_checkpoint,
                                       const checkpoint& new_checkpoint) const {
    if (old_tree_size != old_checkpoint.tree_size || new_tree_size != new_checkpoint.tree_size ||
        old_root_hash != old_checkpoint.root_hash || new_root_hash != new_checkpoint.root_hash ||
        !same_checkpoint(bound_checkpoint, new_checkpoint) || old_tree_size > new_tree_size) {
        return std::unexpected(
            failure(errc::corrupt, "consistency proof is not bound to checkpoints"));
    }
    if (old_tree_size == 0) {
        return audit_path.empty() ? result<void>{}
                                  : result<void>{std::unexpected(failure(
                                        errc::corrupt, "zero-size consistency proof has a path"))};
    }
    if (old_tree_size == new_tree_size) {
        return audit_path.empty() && old_root_hash == new_root_hash
                   ? result<void>{}
                   : result<void>{std::unexpected(
                         failure(errc::corrupt, "equal-size checkpoints disagree"))};
    }
    if (audit_path.empty()) {
        return std::unexpected(failure(errc::corrupt, "consistency proof path is empty"));
    }
    auto old_node = old_tree_size - 1;
    auto new_node = new_tree_size - 1;
    while ((old_node & 1U) != 0U) {
        old_node >>= 1U;
        new_node >>= 1U;
    }
    auto position = std::size_t{};
    auto old_candidate = hash{};
    auto new_candidate = hash{};
    if (old_node == 0) {
        old_candidate = old_root_hash;
        new_candidate = old_root_hash;
    } else {
        old_candidate = audit_path[position];
        new_candidate = audit_path[position++];
    }
    while (position < audit_path.size()) {
        if (new_node == 0) {
            return std::unexpected(failure(errc::corrupt, "consistency proof has trailing hashes"));
        }
        if ((old_node & 1U) != 0U || old_node == new_node) {
            auto old_parent = node_hash(audit_path[position], old_candidate);
            auto new_parent = node_hash(audit_path[position], new_candidate);
            if (!old_parent || !new_parent) {
                return std::unexpected(!old_parent ? old_parent.error() : new_parent.error());
            }
            old_candidate = *old_parent;
            new_candidate = *new_parent;
            while (old_node != 0 && (old_node & 1U) == 0U) {
                old_node >>= 1U;
                new_node >>= 1U;
            }
        } else {
            auto parent = node_hash(new_candidate, audit_path[position]);
            if (!parent) {
                return std::unexpected(parent.error());
            }
            new_candidate = *parent;
        }
        old_node >>= 1U;
        new_node >>= 1U;
        ++position;
    }
    if (new_node != 0 || old_candidate != old_root_hash || new_candidate != new_root_hash) {
        return std::unexpected(failure(errc::corrupt, "consistency proof does not verify"));
    }
    return {};
}

struct http_log_source::impl {
    parsed_url url;
    std::string name;
};

http_log_source::http_log_source(std::string url) : impl_(std::make_unique<impl>()) {
    auto parsed = parse_url(url);
    if (!parsed) {
        throw std::invalid_argument(parsed.error().message);
    }
    impl_->url = std::move(*parsed);
    impl_->name = std::move(url);
}

http_log_source::~http_log_source() = default;
http_log_source::http_log_source(http_log_source&&) noexcept = default;
http_log_source& http_log_source::operator=(http_log_source&&) noexcept = default;

std::string http_log_source::name() const {
    return impl_->name;
}

runtime::task<actor> http_log_source::identity() {
    auto response = co_await perform_get(impl_->url, query_target(impl_->url, "/v1/identity"));
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status < 200 || response->status >= 300) {
        co_return std::unexpected(
            failure(errc::unavailable, "log identity request failed", impl_->name));
    }
    auto value = parse_value(response->body);
    if (!value) {
        co_return std::unexpected(value.error());
    }
    const auto identifier = text_field(*value, "taap_id");
    const auto public_key = text_field(*value, "public_key");
    if (!identifier || !public_key) {
        co_return std::unexpected(!identifier ? identifier.error() : public_key.error());
    }
    auto decoded = base64url_decode(*public_key);
    if (!decoded || decoded->size() != 32 || !identifier->starts_with("taap:1:ed25519:")) {
        co_return std::unexpected(failure(errc::corrupt, "invalid log identity"));
    }
    co_return actor{*identifier, "ed25519", {}, std::move(*decoded), {}};
}

runtime::task<log_configuration> http_log_source::configuration() {
    auto response = co_await perform_get(
        impl_->url, query_target(impl_->url, "/v1/transparency_log_configuration"));
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status < 200 || response->status >= 300) {
        co_return std::unexpected(
            failure(errc::unavailable, "log configuration request failed", impl_->name));
    }
    auto value = parse_value(response->body);
    if (!value) {
        co_return std::unexpected(value.error());
    }
    const auto threshold = unsigned_field(*value, "witness_threshold");
    const auto witnesses = field(*value, "witnesses");
    if (!threshold || !witnesses || !(*witnesses)->is_array()) {
        co_return std::unexpected(!threshold ? threshold.error()
                                  : !witnesses
                                      ? witnesses.error()
                                      : failure(errc::corrupt, "witnesses are not an array"));
    }
    auto output = log_configuration{};
    output.witness_threshold = *threshold;
    for (const auto& item : **witnesses) {
        const auto endpoint = text_field(item, "endpoint");
        const auto public_key = text_field(item, "public_key");
        if (!endpoint || !public_key) {
            co_return std::unexpected(!endpoint ? endpoint.error() : public_key.error());
        }
        auto decoded = decode_hex(*public_key);
        if (!decoded) {
            co_return std::unexpected(decoded.error());
        }
        output.witnesses.push_back({*endpoint, *decoded});
    }
    co_return output;
}

runtime::task<checkpoint> http_log_source::current_checkpoint() {
    auto response =
        co_await perform_get(impl_->url, query_target(impl_->url, "/v1/transparency_checkpoint"));
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status < 200 || response->status >= 300) {
        co_return std::unexpected(
            failure(errc::unavailable, "checkpoint request failed", impl_->name));
    }
    auto value = parse_value(response->body);
    if (!value) {
        co_return std::unexpected(value.error());
    }
    co_return parse_checkpoint(*value);
}

runtime::task<tile>
http_log_source::get_tile(std::uint32_t level, std::uint64_t index, std::uint64_t width) {
    const auto query = "level=" + std::to_string(level) + "&index=" + std::to_string(index) +
                       "&width=" + std::to_string(width);
    auto response =
        co_await perform_get(impl_->url, query_target(impl_->url, "/v1/transparency_tile", query));
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status < 200 || response->status >= 300) {
        co_return std::unexpected(failure(errc::unavailable, "tile request failed", impl_->name));
    }
    auto value = parse_value(response->body);
    if (!value) {
        co_return std::unexpected(value.error());
    }
    co_return parse_tile(*value);
}

runtime::task<inclusion_proof> http_log_source::get_inclusion_proof(std::uint64_t leaf_index,
                                                                    std::uint64_t tree_size) {
    const auto query =
        "leaf_index=" + std::to_string(leaf_index) + "&tree_size=" + std::to_string(tree_size);
    auto response = co_await perform_get(
        impl_->url, query_target(impl_->url, "/v1/transparency_inclusion_proof", query));
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status < 200 || response->status >= 300) {
        co_return std::unexpected(
            failure(errc::unavailable, "inclusion proof request failed", impl_->name));
    }
    auto value = parse_value(response->body);
    if (!value) {
        co_return std::unexpected(value.error());
    }
    co_return parse_inclusion(*value);
}

runtime::task<consistency_proof>
http_log_source::get_consistency_proof(std::uint64_t old_tree_size, std::uint64_t new_tree_size) {
    const auto query = "old_tree_size=" + std::to_string(old_tree_size) +
                       "&new_tree_size=" + std::to_string(new_tree_size);
    auto response = co_await perform_get(
        impl_->url, query_target(impl_->url, "/v1/transparency_consistency_proof", query));
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status < 200 || response->status >= 300) {
        co_return std::unexpected(
            failure(errc::unavailable, "consistency proof request failed", impl_->name));
    }
    auto value = parse_value(response->body);
    if (!value) {
        co_return std::unexpected(value.error());
    }
    co_return parse_consistency(*value);
}

runtime::task<checkpoint_trust> trust_checkpoint(repository& destination,
                                                 std::string log_name,
                                                 checkpoint value,
                                                 bootstrap_policy policy) {
    auto existing = co_await trusted_checkpoint(destination, log_name);
    if (!existing) {
        co_return std::unexpected(existing.error());
    }
    if (*existing) {
        if ((*existing)->tree_size != value.tree_size ||
            (*existing)->root_hash != value.root_hash ||
            (*existing)->checkpoint_hash != value.checkpoint_hash) {
            co_return std::unexpected(failure(
                errc::conflict, "trust update requires a verified consistency proof", log_name));
        }
        co_return checkpoint_trust{std::move(value), false};
    }
    auto saved = co_await persist_checkpoint(destination, std::move(log_name), value);
    if (!saved) {
        co_return std::unexpected(saved.error());
    }
    co_return checkpoint_trust{std::move(value), policy == bootstrap_policy::accept_with_warning};
}

struct log_client::impl {
    log_source* source{};
};

log_client::log_client(log_source& source) : impl_(std::make_unique<impl>(impl{&source})) {}
log_client::~log_client() = default;
log_client::log_client(log_client&&) noexcept = default;
log_client& log_client::operator=(log_client&&) noexcept = default;

runtime::task<checkpoint> log_client::current_checkpoint() {
    auto identity = co_await impl_->source->identity();
    auto configuration = co_await impl_->source->configuration();
    auto value = co_await impl_->source->current_checkpoint();
    if (!identity || !configuration || !value) {
        co_return std::unexpected(!identity        ? identity.error()
                                  : !configuration ? configuration.error()
                                                   : value.error());
    }
    auto verified = value->verify({*identity, *configuration});
    if (!verified) {
        co_return std::unexpected(verified.error());
    }
    co_return std::move(*value);
}

namespace {

auto checked_entries(log_source& source, const checkpoint& value)
    -> runtime::task<std::vector<std::vector<std::byte>>> {
    if (value.tree_size == 0) {
        auto root = sha256({});
        if (!root || *root != value.root_hash) {
            co_return std::unexpected(
                failure(errc::corrupt, "empty checkpoint root does not verify"));
        }
        co_return std::vector<std::vector<std::byte>>{};
    }
    auto entries = std::vector<std::vector<std::byte>>{};
    entries.reserve(static_cast<std::size_t>(value.tree_size));
    for (auto start = std::uint64_t{}; start < value.tree_size; start += tile_width) {
        const auto width = std::min<std::uint64_t>(tile_width, value.tree_size - start);
        auto received = co_await source.get_tile(0, start / tile_width, width);
        if (!received) {
            co_return std::unexpected(received.error());
        }
        if (received->level != 0 || received->index != start / tile_width ||
            received->width != width) {
            co_return std::unexpected(
                failure(errc::corrupt, "entry tile metadata does not match request"));
        }
        auto decoded = received->entries();
        if (!decoded) {
            co_return std::unexpected(decoded.error());
        }
        entries.insert(entries.end(),
                       std::make_move_iterator(decoded->begin()),
                       std::make_move_iterator(decoded->end()));
    }
    auto level_values = std::vector<hash>{};
    level_values.reserve(entries.size());
    for (const auto& entry : entries) {
        auto calculated = leaf_hash(entry);
        if (!calculated) {
            co_return std::unexpected(calculated.error());
        }
        level_values.push_back(*calculated);
    }
    for (auto level = std::uint32_t{1}; level_values.size() >= 2; ++level) {
        auto expected = std::vector<hash>{};
        expected.reserve(level_values.size() / 2);
        for (auto index = std::size_t{}; index + 1 < level_values.size(); index += 2) {
            auto parent = node_hash(level_values[index], level_values[index + 1]);
            if (!parent) {
                co_return std::unexpected(parent.error());
            }
            expected.push_back(*parent);
        }
        for (auto start = std::uint64_t{}; start < expected.size(); start += tile_width) {
            const auto width = std::min<std::uint64_t>(tile_width, expected.size() - start);
            auto received = co_await source.get_tile(level, start / tile_width, width);
            if (!received) {
                co_return std::unexpected(received.error());
            }
            if (received->level != level || received->index != start / tile_width ||
                received->width != width) {
                co_return std::unexpected(
                    failure(errc::corrupt, "hash tile metadata does not match request"));
            }
            auto decoded = received->hashes();
            if (!decoded) {
                co_return std::unexpected(decoded.error());
            }
            if (!std::ranges::equal(*decoded, std::span{expected}.subspan(start, width))) {
                co_return std::unexpected(
                    failure(errc::corrupt, "parent hash tile does not match children"));
            }
        }
        level_values = std::move(expected);
    }
    auto root = merkle_root(entries, 0, entries.size());
    if (!root || *root != value.root_hash) {
        co_return std::unexpected(
            failure(errc::corrupt, "entry and parent tiles do not reach checkpoint root"));
    }
    co_return entries;
}

} // namespace

runtime::task<void> log_client::verify_tiles(checkpoint value) {
    auto entries = co_await checked_entries(*impl_->source, value);
    if (!entries) {
        co_return std::unexpected(entries.error());
    }
    co_return result<void>{};
}

runtime::task<checkpoint_trust> log_client::verify_inclusion(repository& destination,
                                                             std::vector<std::byte> attestation,
                                                             bootstrap_policy policy) {
    auto canonical = verify_canonical_attestation(attestation);
    if (!canonical) {
        co_return std::unexpected(canonical.error());
    }
    auto selected = co_await current_checkpoint();
    if (!selected) {
        co_return std::unexpected(selected.error());
    }
    auto entries = co_await checked_entries(*impl_->source, *selected);
    if (!entries) {
        co_return std::unexpected(entries.error());
    }
    const auto found = std::ranges::find(*entries, attestation);
    if (found == entries->end()) {
        co_return std::unexpected(
            failure(errc::not_found, "attestation is absent from verified log tiles"));
    }
    const auto index = static_cast<std::uint64_t>(std::distance(entries->begin(), found));
    auto proof = co_await impl_->source->get_inclusion_proof(index, selected->tree_size);
    if (!proof) {
        co_return std::unexpected(proof.error());
    }
    auto verified = proof->verify(attestation, *selected);
    if (!verified) {
        co_return std::unexpected(verified.error());
    }
    co_return co_await trust_checkpoint(
        destination, impl_->source->name(), std::move(*selected), policy);
}

runtime::task<checkpoint_trust> log_client::verify_consistency(repository& destination,
                                                               bootstrap_policy policy) {
    auto selected = co_await current_checkpoint();
    if (!selected) {
        co_return std::unexpected(selected.error());
    }
    auto previous = co_await trusted_checkpoint(destination, impl_->source->name());
    if (!previous) {
        co_return std::unexpected(previous.error());
    }
    if (!*previous) {
        co_return co_await trust_checkpoint(
            destination, impl_->source->name(), std::move(*selected), policy);
    }
    auto old = checkpoint{};
    old.sequence = (*previous)->sequence;
    old.tree_size = (*previous)->tree_size;
    old.root_hash = (*previous)->root_hash;
    old.checkpoint_hash = (*previous)->checkpoint_hash;
    auto proof = co_await impl_->source->get_consistency_proof(old.tree_size, selected->tree_size);
    if (!proof) {
        co_return std::unexpected(proof.error());
    }
    auto verified = proof->verify(old, *selected);
    if (!verified) {
        co_return std::unexpected(verified.error());
    }
    auto saved = co_await persist_checkpoint(destination, impl_->source->name(), *selected);
    if (!saved) {
        co_return std::unexpected(saved.error());
    }
    co_return checkpoint_trust{std::move(*selected), false};
}

runtime::task<identity_scan> log_client::scan(std::string identity) {
    auto selected = co_await current_checkpoint();
    if (!selected) {
        co_return std::unexpected(selected.error());
    }
    auto entries = co_await checked_entries(*impl_->source, *selected);
    if (!entries) {
        co_return std::unexpected(entries.error());
    }
    auto output = identity_scan{std::move(*selected), {}};
    for (auto index = std::size_t{}; index < entries->size(); ++index) {
        auto decoded = verify_canonical_attestation((*entries)[index]);
        if (!decoded) {
            co_return std::unexpected(decoded.error());
        }
        if (!names_identity(*decoded, identity)) {
            continue;
        }
        output.matches.push_back(identity_match{static_cast<std::uint64_t>(index),
                                                decoded->profile,
                                                decoded->issuer.taap_id,
                                                to_string(decoded->subject),
                                                std::move((*entries)[index])});
    }
    co_return output;
}

} // namespace liquid::transparency
