// SPDX-License-Identifier: MIT

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
#include <boost/json.hpp>

#include "liquid/error.hpp"
#include "liquid/runtime.hpp"
#include "liquid/transparency.hpp"

namespace liquid_cli {
namespace {

constexpr std::size_t maximum_response_size = 32U * 1024U * 1024U;

struct parsed_url {
    bool tls{};
    std::string host;
    std::string port;
    std::string path;
};

struct http_response {
    unsigned status{};
    std::string body;
};

struct timestamp_material {
    boost::json::object evidence;
    std::vector<std::byte> checkpoint_signature;
    std::vector<std::byte> evidence_record;
};

auto cli_error(liquid::errc code, std::string message, std::string context = {}) -> liquid::error {
    return {code, std::move(message), std::move(context)};
}

auto fail(const liquid::error& error) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(error.code), error.message, error.context);
    return 1;
}

auto usage(std::string_view message) -> int {
    std::println(stderr, "error: usage: {}", message);
    return 2;
}

auto parse_url(std::string_view input) -> liquid::result<parsed_url> {
    auto output = parsed_url{};
    if (input.starts_with("https://")) {
        output.tls = true;
        output.port = "443";
        input.remove_prefix(8);
    } else if (input.starts_with("http://")) {
        output.port = "80";
        input.remove_prefix(7);
    } else {
        return std::unexpected(
            cli_error(liquid::errc::invalid_argument, "log URL must use http or https", "log URL"));
    }
    const auto slash = input.find('/');
    auto authority = input.substr(0, slash);
    output.path =
        slash == std::string_view::npos ? std::string{} : std::string{input.substr(slash)};
    while (output.path.size() > 1 && output.path.ends_with('/')) {
        output.path.pop_back();
    }
    if (authority.empty() || authority.find('@') != std::string_view::npos) {
        return std::unexpected(
            cli_error(liquid::errc::invalid_argument, "invalid log URL authority", "log URL"));
    }
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) {
            return std::unexpected(
                cli_error(liquid::errc::invalid_argument, "invalid IPv6 log URL", "log URL"));
        }
        output.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') {
                return std::unexpected(
                    cli_error(liquid::errc::invalid_argument, "invalid log URL port", "log URL"));
            }
            output.port = authority.substr(close + 2);
        }
    } else {
        const auto colon = authority.rfind(':');
        if (colon == std::string_view::npos) {
            output.host = authority;
        } else {
            output.host = authority.substr(0, colon);
            output.port = authority.substr(colon + 1);
        }
    }
    if (output.host.empty() || output.port.empty()) {
        return std::unexpected(
            cli_error(liquid::errc::invalid_argument, "log URL host or port is empty", "log URL"));
    }
    if (output.path.empty()) {
        output.path = "/";
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
            for (std::size_t index = 0; matches && index < field.size(); ++index) {
                const auto left = static_cast<unsigned char>(field[index]);
                const auto right = static_cast<unsigned char>(name[index]);
                matches = std::tolower(left) == std::tolower(right);
            }
            if (matches) {
                auto value = header.substr(colon + 1, end - colon - 1);
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                    value.remove_prefix(1);
                }
                return value;
            }
        }
        offset = end + 2;
    }
    return {};
}

auto decode_chunked(std::string_view input) -> liquid::result<std::string> {
    auto output = std::string{};
    auto offset = std::size_t{};
    for (;;) {
        const auto line_end = input.find("\r\n", offset);
        if (line_end == std::string_view::npos) {
            return std::unexpected(
                cli_error(liquid::errc::corrupt, "truncated HTTP chunk size", "log endpoint"));
        }
        const auto extension = input.find(';', offset);
        const auto size_end =
            extension != std::string_view::npos && extension < line_end ? extension : line_end;
        auto size = std::uint64_t{};
        const auto parsed =
            std::from_chars(input.data() + offset, input.data() + size_end, size, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != input.data() + size_end ||
            size > maximum_response_size || size > input.size() - line_end - 2) {
            return std::unexpected(
                cli_error(liquid::errc::corrupt, "invalid HTTP chunk size", "log endpoint"));
        }
        offset = line_end + 2;
        if (size == 0) {
            return output;
        }
        if (size > input.size() - offset || input.size() - offset - size < 2 ||
            input.substr(offset + size, 2) != "\r\n" ||
            size > maximum_response_size - output.size()) {
            return std::unexpected(
                cli_error(liquid::errc::corrupt, "truncated HTTP chunk", "log endpoint"));
        }
        output.append(input.substr(offset, size));
        offset += static_cast<std::size_t>(size) + 2;
    }
}

auto parse_response(std::string wire) -> liquid::result<http_response> {
    const auto header_end = wire.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return std::unexpected(
            cli_error(liquid::errc::corrupt, "HTTP response header is truncated", "log endpoint"));
    }
    const auto header_text = std::string_view{wire}.substr(0, header_end + 4);
    auto header = boost::http::response{header_text};
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
            return std::unexpected(
                cli_error(liquid::errc::corrupt, "invalid HTTP content length", "log endpoint"));
        }
    }
    if (body.size() > maximum_response_size) {
        return std::unexpected(
            cli_error(liquid::errc::corrupt, "HTTP response exceeds size limit", "log endpoint"));
    }
    output.body = body;
    return output;
}

auto make_request(const parsed_url& url, std::string_view target, std::string_view token)
    -> std::string {
    auto request = boost::http::request{boost::http::method::get, target};
    auto authority = url.host;
    if ((!url.tls && url.port != "80") || (url.tls && url.port != "443")) {
        authority += ':' + url.port;
    }
    request.set(boost::http::field::host, authority);
    request.set(boost::http::field::connection, "close");
    request.set(boost::http::field::accept, "application/json");
    if (!token.empty()) {
        request.set(boost::http::field::authorization, "Bearer " + std::string{token});
    }
    return std::string{request.buffer()};
}

template <boost::capy::Stream Stream>
auto exchange_http(Stream& stream, std::string request) -> liquid::runtime::task<http_response> {
    auto [write_error, written] = co_await boost::capy::write(
        stream, boost::capy::const_buffer(request.data(), request.size()));
    if (write_error || written != request.size()) {
        co_return std::unexpected(
            cli_error(liquid::errc::io,
                      write_error ? write_error.message() : "short HTTP write",
                      "log endpoint"));
    }
    auto response = std::string{};
    auto [read_error, read] = co_await boost::capy::read(
        stream, boost::capy::string_dynamic_buffer(&response, maximum_response_size + 64U * 1024U));
    static_cast<void>(read);
    if (read_error && read_error != boost::capy::error::eof) {
        co_return std::unexpected(
            cli_error(liquid::errc::io, read_error.message(), "log endpoint"));
    }
    try {
        co_return parse_response(std::move(response));
    } catch (const std::exception& exception) {
        co_return std::unexpected(
            cli_error(liquid::errc::corrupt, exception.what(), "log endpoint"));
    }
}

auto request(const parsed_url& url, std::string target, std::string token)
    -> liquid::runtime::task<http_response> {
    try {
        const auto executor = co_await boost::capy::this_coro::executor;
        auto resolver = boost::corosio::resolver{executor.context()};
        auto [resolve_error, results] = co_await resolver.resolve(url.host, url.port);
        if (resolve_error) {
            co_return std::unexpected(
                cli_error(liquid::errc::unavailable, resolve_error.message(), url.host));
        }
        auto socket = boost::corosio::tcp_socket{executor.context()};
        auto [connect_error, endpoint] = co_await boost::corosio::connect(socket, results);
        static_cast<void>(endpoint);
        if (connect_error) {
            co_return std::unexpected(
                cli_error(liquid::errc::unavailable, connect_error.message(), url.host));
        }
        auto wire = make_request(url, target, token);
        if (!url.tls) {
            co_return co_await exchange_http(socket, std::move(wire));
        }
        auto context = boost::corosio::tls_context{};
        context.set_hostname(url.host);
        if (auto code = context.set_default_verify_paths(); code) {
            co_return std::unexpected(cli_error(liquid::errc::io, code.message(), url.host));
        }
        if (auto code = context.set_verify_mode(boost::corosio::tls_verify_mode::peer); code) {
            co_return std::unexpected(cli_error(liquid::errc::io, code.message(), url.host));
        }
        auto secure = boost::corosio::openssl_stream{&socket, context};
        auto [handshake_error] = co_await secure.handshake(boost::corosio::openssl_stream::client);
        if (handshake_error) {
            co_return std::unexpected(
                cli_error(liquid::errc::unavailable, handshake_error.message(), url.host));
        }
        auto response = co_await exchange_http(secure, std::move(wire));
        auto [shutdown_error] = co_await secure.shutdown();
        if (shutdown_error && shutdown_error != boost::capy::error::eof && response) {
            co_return std::unexpected(
                cli_error(liquid::errc::io, shutdown_error.message(), url.host));
        }
        co_return response;
    } catch (const std::exception& exception) {
        co_return std::unexpected(
            cli_error(liquid::errc::internal, exception.what(), "log endpoint"));
    }
}

auto endpoint_target(const parsed_url& url, std::string_view suffix) -> std::string {
    auto target = url.path == "/" ? std::string{} : url.path;
    target += suffix;
    return target;
}

auto response_value(const http_response& response, std::string_view capability)
    -> liquid::result<boost::json::object> {
    boost::system::error_code code;
    auto document = boost::json::parse(response.body, code);
    if (code || !document.is_object()) {
        return std::unexpected(cli_error(
            liquid::errc::corrupt, "log endpoint returned invalid JSON", std::string{capability}));
    }
    const auto& envelope = document.as_object();
    const auto* ok = envelope.if_contains("ok");
    if (!ok || !ok->is_bool()) {
        return std::unexpected(cli_error(liquid::errc::corrupt,
                                         "log endpoint returned an invalid response envelope",
                                         std::string{capability}));
    }
    if (response.status < 200 || response.status >= 300 || !ok->as_bool()) {
        auto message = "log endpoint returned HTTP " + std::to_string(response.status);
        if (const auto* error_value = envelope.if_contains("error");
            error_value && error_value->is_object()) {
            const auto* detail = error_value->as_object().if_contains("message");
            if (detail && detail->is_string()) {
                message = std::string{detail->as_string()};
            }
        }
        const auto error_code = response.status == 401 || response.status == 403
                                    ? liquid::errc::unauthorized
                                    : liquid::errc::unavailable;
        return std::unexpected(cli_error(error_code, std::move(message), std::string{capability}));
    }
    const auto* value = envelope.if_contains("value");
    if (!value || !value->is_object()) {
        return std::unexpected(cli_error(liquid::errc::corrupt,
                                         "log endpoint returned a non-object value",
                                         std::string{capability}));
    }
    return value->as_object();
}

auto get_capability(const parsed_url& url,
                    std::string_view capability,
                    std::string_view query,
                    std::string token) -> liquid::result<boost::json::object> {
    auto target = endpoint_target(url, "/v1/" + std::string{capability});
    target += query;
    auto response = liquid::runtime::block_on(request(url, std::move(target), std::move(token)));
    if (!response) {
        return std::unexpected(response.error());
    }
    return response_value(*response, capability);
}

auto string_field(const boost::json::object& object,
                  std::string_view field,
                  std::string_view context) -> liquid::result<std::string> {
    const auto* value = object.if_contains(field);
    if (!value || !value->is_string()) {
        return std::unexpected(cli_error(liquid::errc::corrupt,
                                         "log endpoint omitted string field " + std::string{field},
                                         std::string{context}));
    }
    return std::string{value->as_string()};
}

auto decode_base64url(std::string_view input, std::string_view context)
    -> liquid::result<std::vector<std::byte>> {
    const auto last_value = input.find_last_not_of('=');
    const auto padding =
        last_value == std::string_view::npos ? input.size() : input.size() - last_value - 1;
    if (padding > 2 || (padding != 0 && input.size() % 4 != 0) ||
        input.substr(0, input.size() - padding).find('=') != std::string_view::npos) {
        return std::unexpected(
            cli_error(liquid::errc::corrupt, "invalid base64url padding", std::string{context}));
    }
    input.remove_suffix(padding);
    const auto remainder = input.size() % 4;
    const auto expected_padding = remainder == 0 ? 0 : 4 - remainder;
    if (remainder == 1 || (padding != 0 && padding != expected_padding)) {
        return std::unexpected(
            cli_error(liquid::errc::corrupt, "invalid base64url length", std::string{context}));
    }
    auto value = [](char character) -> int {
        if (character >= 'A' && character <= 'Z') {
            return character - 'A';
        }
        if (character >= 'a' && character <= 'z') {
            return character - 'a' + 26;
        }
        if (character >= '0' && character <= '9') {
            return character - '0' + 52;
        }
        if (character == '-') {
            return 62;
        }
        if (character == '_') {
            return 63;
        }
        return -1;
    };
    auto output = std::vector<std::byte>{};
    output.reserve((input.size() * 3) / 4);
    auto offset = std::size_t{};
    while (input.size() - offset >= 4) {
        const auto first = value(input[offset]);
        const auto second = value(input[offset + 1]);
        const auto third = value(input[offset + 2]);
        const auto fourth = value(input[offset + 3]);
        if (first < 0 || second < 0 || third < 0 || fourth < 0) {
            return std::unexpected(cli_error(
                liquid::errc::corrupt, "invalid base64url character", std::string{context}));
        }
        output.push_back(static_cast<std::byte>((first << 2) | (second >> 4)));
        output.push_back(static_cast<std::byte>((second << 4) | (third >> 2)));
        output.push_back(static_cast<std::byte>((third << 6) | fourth));
        offset += 4;
    }
    if (remainder == 2) {
        const auto first = value(input[offset]);
        const auto second = value(input[offset + 1]);
        if (first < 0 || second < 0 || (second & 0x0f) != 0) {
            return std::unexpected(
                cli_error(liquid::errc::corrupt, "invalid base64url tail", std::string{context}));
        }
        output.push_back(static_cast<std::byte>((first << 2) | (second >> 4)));
    } else if (remainder == 3) {
        const auto first = value(input[offset]);
        const auto second = value(input[offset + 1]);
        const auto third = value(input[offset + 2]);
        if (first < 0 || second < 0 || third < 0 || (third & 0x03) != 0) {
            return std::unexpected(
                cli_error(liquid::errc::corrupt, "invalid base64url tail", std::string{context}));
        }
        output.push_back(static_cast<std::byte>((first << 2) | (second >> 4)));
        output.push_back(static_cast<std::byte>((second << 4) | (third >> 2)));
    }
    return output;
}

auto load_material(std::string_view log_url) -> liquid::result<timestamp_material> {
    auto url = parse_url(log_url);
    if (!url) {
        return std::unexpected(url.error());
    }
    auto token = std::string{};
    if (const auto* configured = std::getenv("LIQUID_TOKEN")) {
        token = configured;
    }
    auto checkpoint = get_capability(*url, "transparency_checkpoint", "", token);
    if (!checkpoint) {
        return std::unexpected(checkpoint.error());
    }
    auto digest = string_field(*checkpoint, "checkpoint_hash_sha256", "transparency_checkpoint");
    if (!digest) {
        return std::unexpected(digest.error());
    }
    const auto* signature_value = checkpoint->if_contains("signature");
    if (!signature_value || !signature_value->is_object()) {
        return std::unexpected(cli_error(liquid::errc::corrupt,
                                         "checkpoint omitted its signature object",
                                         "transparency_checkpoint"));
    }
    auto signature =
        string_field(signature_value->as_object(), "signature", "transparency_checkpoint");
    if (!signature) {
        return std::unexpected(signature.error());
    }
    auto evidence = get_capability(
        *url, "timestamp_evidence", "?checkpoint_digest=" + *digest, std::move(token));
    if (!evidence) {
        return std::unexpected(evidence.error());
    }
    auto encoded_record = string_field(*evidence, "evidence_record", "timestamp_evidence");
    if (!encoded_record) {
        return std::unexpected(encoded_record.error());
    }
    auto signature_bytes = decode_base64url(*signature, "checkpoint signature");
    if (!signature_bytes) {
        return std::unexpected(signature_bytes.error());
    }
    auto record_bytes = decode_base64url(*encoded_record, "timestamp evidence record");
    if (!record_bytes) {
        return std::unexpected(record_bytes.error());
    }
    return timestamp_material{
        std::move(*evidence), std::move(*signature_bytes), std::move(*record_bytes)};
}

auto read_file(const std::filesystem::path& path) -> liquid::result<std::string> {
    auto input = std::ifstream{path, std::ios::binary};
    if (!input) {
        return std::unexpected(
            cli_error(liquid::errc::io, "could not open TSA trust anchor file", path.string()));
    }
    return std::string{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

auto display_evidence(const boost::json::object& evidence) -> liquid::result<void> {
    for (const auto field : {"checkpoint_digest", "signature_digest", "digest_algorithm"}) {
        auto value = string_field(evidence, field, "timestamp_evidence");
        if (!value) {
            return std::unexpected(value.error());
        }
        std::println("{}={}", field, *value);
    }
    const auto* timestamps = evidence.if_contains("archive_timestamps");
    if (!timestamps || !timestamps->is_array()) {
        return std::unexpected(cli_error(liquid::errc::corrupt,
                                         "timestamp evidence omitted archive_timestamps",
                                         "timestamp_evidence"));
    }
    std::println("archive_timestamps={}", timestamps->as_array().size());
    for (const auto& timestamp_value : timestamps->as_array()) {
        if (!timestamp_value.is_object()) {
            return std::unexpected(cli_error(
                liquid::errc::corrupt, "timestamp summary is not an object", "timestamp_evidence"));
        }
        const auto& timestamp = timestamp_value.as_object();
        const auto* sequence = timestamp.if_contains("sequence");
        auto generated_at = string_field(timestamp, "generated_at", "timestamp_evidence");
        auto policy = string_field(timestamp, "policy", "timestamp_evidence");
        auto digest = string_field(timestamp, "token_sha256", "timestamp_evidence");
        if (!sequence || (!sequence->is_int64() && !sequence->is_uint64()) || !generated_at ||
            !policy || !digest) {
            return std::unexpected(cli_error(
                liquid::errc::corrupt, "timestamp summary is incomplete", "timestamp_evidence"));
        }
        const auto sequence_number = sequence->is_uint64()
                                         ? sequence->as_uint64()
                                         : static_cast<std::uint64_t>(sequence->as_int64());
        std::println("timestamp={} generated_at={} policy={} token_sha256={}",
                     sequence_number,
                     *generated_at,
                     *policy,
                     *digest);
    }
    return {};
}

} // namespace

auto log_timestamps(int argc, char** argv) -> int {
    if (argc != 1) {
        return usage("log timestamps <log-url>");
    }
    auto material = load_material(argv[0]);
    if (!material) {
        return fail(material.error());
    }
    auto displayed = display_evidence(material->evidence);
    return displayed ? 0 : fail(displayed.error());
}

auto verify_log_timestamps(int argc, char** argv) -> int {
    if (argc != 1 || std::string_view{argv[0]} != "--timestamps") {
        return usage("log verify --timestamps");
    }
    const auto* log_url = std::getenv("LIQUID_LOG_URL");
    if (!log_url || std::string_view{log_url}.empty()) {
        return fail(cli_error(liquid::errc::not_configured,
                              "LIQUID_LOG_URL is required for timestamp verification",
                              "timestamp trust configuration"));
    }
    const auto* trust_path = std::getenv("LIQUID_TSA_TRUST_CERT");
    if (!trust_path || std::string_view{trust_path}.empty()) {
        return fail(cli_error(liquid::errc::unauthorized,
                              "no TSA trust anchors are configured; set LIQUID_TSA_TRUST_CERT",
                              "timestamp trust configuration"));
    }
    auto trust_pem = read_file(trust_path);
    if (!trust_pem) {
        return fail(trust_pem.error());
    }
    auto trust = liquid::x509::trust_anchor_set{};
    auto added = trust.add_pem(*trust_pem);
    if (!added) {
        return fail(added.error());
    }
    auto material = load_material(log_url);
    if (!material) {
        return fail(material.error());
    }
    auto verified = liquid::transparency::evidence_record::verify(
        material->evidence_record, material->checkpoint_signature, trust);
    if (!verified) {
        return fail(verified.error());
    }
    auto checkpoint_digest =
        string_field(material->evidence, "checkpoint_digest", "timestamp_evidence");
    if (!checkpoint_digest) {
        return fail(checkpoint_digest.error());
    }
    std::println("checkpoint_digest={}", *checkpoint_digest);
    std::println("timestamp_evidence=verified");
    std::println("archive_timestamps={}", verified->archive_timestamps.size());
    for (const auto& timestamp : verified->archive_timestamps) {
        std::println("timestamp={} generated_at={} policy={} digest={} signature={}",
                     timestamp.sequence,
                     timestamp.timestamp.generated_at,
                     timestamp.timestamp.policy,
                     timestamp.timestamp.digest_algorithm,
                     timestamp.timestamp.signature_algorithm);
    }
    return 0;
}

} // namespace liquid_cli
