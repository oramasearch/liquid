#include "liquid/sync.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>
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

#include "encoding/cbor.hpp"
#include "repository/sync_access.hpp"
#include "sync/codec.hpp"

namespace liquid {
namespace {

constexpr auto content_type = "application/vnd.liquid.sync-v1";
namespace cbor = encoding::cbor;

struct parsed_url {
    bool tls{};
    std::string host;
    std::string port;
    std::string path;
};

struct http_response {
    unsigned status{};
    std::vector<std::byte> body;
};

auto sync_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
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
        return std::unexpected(
            sync_error(errc::invalid_argument, "sync URL must use http or https", "sync URL"));
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
            sync_error(errc::invalid_argument, "invalid sync URL authority", "sync URL"));
    }
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) {
            return std::unexpected(
                sync_error(errc::invalid_argument, "invalid IPv6 sync URL", "sync URL"));
        }
        output.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') {
                return std::unexpected(
                    sync_error(errc::invalid_argument, "invalid sync URL port", "sync URL"));
            }
            output.port = authority.substr(close + 2);
        }
    } else {
        const auto colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            output.host = authority.substr(0, colon);
            output.port = authority.substr(colon + 1);
        } else {
            output.host = authority;
        }
    }
    if (output.host.empty() || output.port.empty()) {
        return std::unexpected(
            sync_error(errc::invalid_argument, "sync URL host or port is empty", "sync URL"));
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
            auto field = header.substr(offset, colon - offset);
            auto matches = field.size() == name.size();
            for (auto index = std::size_t{}; matches && index < field.size(); ++index) {
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

auto decode_chunked(std::string_view input) -> result<std::vector<std::byte>> {
    auto output = std::vector<std::byte>{};
    auto offset = std::size_t{};
    for (;;) {
        const auto line_end = input.find("\r\n", offset);
        if (line_end == std::string_view::npos) {
            return std::unexpected(sync_error(errc::corrupt, "truncated HTTP chunk size"));
        }
        const auto extension = input.find(';', offset);
        const auto size_end =
            extension != std::string_view::npos && extension < line_end ? extension : line_end;
        auto size = std::uint64_t{};
        const auto parsed =
            std::from_chars(input.data() + offset, input.data() + size_end, size, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != input.data() + size_end ||
            size > std::numeric_limits<std::size_t>::max()) {
            return std::unexpected(sync_error(errc::corrupt, "invalid HTTP chunk size"));
        }
        offset = line_end + 2;
        if (size == 0) {
            return output;
        }
        if (size > input.size() - offset || input.size() - offset - size < 2 ||
            input.substr(offset + size, 2) != "\r\n") {
            return std::unexpected(sync_error(errc::corrupt, "truncated HTTP chunk"));
        }
        const auto* first = reinterpret_cast<const std::byte*>(input.data() + offset);
        output.insert(output.end(), first, first + size);
        offset += static_cast<std::size_t>(size) + 2;
    }
}

auto parse_response(std::string wire) -> result<http_response> {
    const auto header_end = wire.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return std::unexpected(sync_error(errc::corrupt, "HTTP response header is truncated"));
    }
    const auto header_text = std::string_view{wire}.substr(0, header_end + 4);
    auto header = boost::http::response{header_text};
    const auto body = std::string_view{wire}.substr(header_end + 4);
    auto output = http_response{.status = static_cast<unsigned>(header.status())};
    const auto transfer_encoding = header_value(header_text, "transfer-encoding");
    if (transfer_encoding == "chunked") {
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
            return std::unexpected(sync_error(errc::corrupt, "invalid HTTP content length"));
        }
    }
    const auto* first = reinterpret_cast<const std::byte*>(body.data());
    output.body.assign(first, first + body.size());
    return output;
}

auto cbor_field(const cbor::value& value, std::string_view key) -> const cbor::value* {
    const auto* fields = std::get_if<cbor::value::map>(&value.data);
    if (!fields) {
        return nullptr;
    }
    for (const auto& [name, child] : *fields) {
        const auto* text = std::get_if<std::string>(&name.data);
        if (text && *text == key) {
            return &child;
        }
    }
    return nullptr;
}

auto parse_error_code(std::string_view value) -> std::optional<errc> {
    for (const auto code : {errc::not_found,
                            errc::already_exists,
                            errc::conflict,
                            errc::invalid_argument,
                            errc::unsupported,
                            errc::corrupt,
                            errc::io,
                            errc::locked,
                            errc::unauthorized,
                            errc::unavailable,
                            errc::busy,
                            errc::internal}) {
        if (to_string(code) == value) {
            return code;
        }
    }
    return std::nullopt;
}

auto status_error(unsigned status, std::string context, std::span<const std::byte> body) -> error {
    auto code = errc::internal;
    if (status == 400) {
        code = errc::invalid_argument;
    } else if (status == 401 || status == 403) {
        code = errc::unauthorized;
    } else if (status == 404) {
        code = errc::not_found;
    } else if (status == 409) {
        code = errc::conflict;
    } else if (status == 423) {
        code = errc::locked;
    } else if (status == 429) {
        code = errc::busy;
    } else if (status == 503) {
        code = errc::unavailable;
    }
    auto decoded = cbor::decode(body);
    const auto* version_field = decoded ? cbor_field(*decoded, "v") : nullptr;
    const auto* code_field = decoded ? cbor_field(*decoded, "code") : nullptr;
    const auto* message_field = decoded ? cbor_field(*decoded, "message") : nullptr;
    const auto* context_field = decoded ? cbor_field(*decoded, "context") : nullptr;
    const auto* item_field = decoded ? cbor_field(*decoded, "item") : nullptr;
    const auto* version =
        version_field ? std::get_if<std::uint64_t>(&version_field->data) : nullptr;
    const auto* code_text = code_field ? std::get_if<std::string>(&code_field->data) : nullptr;
    const auto* message = message_field ? std::get_if<std::string>(&message_field->data) : nullptr;
    const auto* error_context =
        context_field ? std::get_if<std::string>(&context_field->data) : nullptr;
    const auto* item = item_field ? std::get_if<std::uint64_t>(&item_field->data) : nullptr;
    if (version && *version == 1 && code_text && message) {
        if (auto parsed = parse_error_code(*code_text)) {
            code = *parsed;
        }
        context = error_context ? *error_context : std::move(context);
        if (item) {
            context = "item=" + std::to_string(*item) +
                      (context.empty() ? std::string{} : ": " + context);
        }
        return sync_error(code, *message, std::move(context));
    }
    return sync_error(code, "sync endpoint returned HTTP " + std::to_string(status), context);
}

auto make_request(boost::http::method method,
                  const parsed_url& url,
                  std::string_view target,
                  std::string_view token,
                  std::span<const std::byte> body) -> std::string {
    auto request = boost::http::request{method, target};
    auto authority = url.host;
    if ((!url.tls && url.port != "80") || (url.tls && url.port != "443")) {
        authority += ':' + url.port;
    }
    request.set(boost::http::field::host, authority);
    request.set(boost::http::field::connection, "close");
    request.set(boost::http::field::content_type, content_type);
    request.set_payload_size(body.size());
    if (!token.empty()) {
        request.set(boost::http::field::authorization, "Bearer " + std::string{token});
    }
    auto wire = std::string{request.buffer()};
    wire.append(reinterpret_cast<const char*>(body.data()), body.size());
    return wire;
}

template <boost::capy::Stream Stream>
auto exchange_http(Stream& stream, std::string wire) -> runtime::task<http_response> {
    auto [write_error, written] =
        co_await boost::capy::write(stream, boost::capy::const_buffer(wire.data(), wire.size()));
    if (write_error || written != wire.size()) {
        co_return std::unexpected(sync_error(
            errc::io, write_error ? write_error.message() : "short HTTP write", "sync endpoint"));
    }
    auto response = std::string{};
    auto [read_error, read] =
        co_await boost::capy::read(stream, boost::capy::string_dynamic_buffer(&response));
    static_cast<void>(read);
    if (read_error && read_error != boost::capy::error::eof) {
        co_return std::unexpected(sync_error(errc::io, read_error.message(), "sync endpoint"));
    }
    try {
        co_return parse_response(std::move(response));
    } catch (const std::exception& exception) {
        co_return std::unexpected(sync_error(errc::corrupt, exception.what(), "sync endpoint"));
    }
}

auto perform_request(const parsed_url& url,
                     std::string_view token,
                     boost::http::method method,
                     std::string target,
                     std::span<const std::byte> body) -> runtime::task<http_response> {
    try {
        const auto executor = co_await boost::capy::this_coro::executor;
        auto resolver = boost::corosio::resolver{executor.context()};
        auto [resolve_error, results] = co_await resolver.resolve(url.host, url.port);
        if (resolve_error) {
            co_return std::unexpected(
                sync_error(errc::unavailable, resolve_error.message(), url.host));
        }
        auto socket = boost::corosio::tcp_socket{executor.context()};
        auto [connect_error, endpoint] = co_await boost::corosio::connect(socket, results);
        static_cast<void>(endpoint);
        if (connect_error) {
            co_return std::unexpected(
                sync_error(errc::unavailable, connect_error.message(), url.host));
        }
        auto wire = make_request(method, url, target, token, body);
        if (!url.tls) {
            co_return co_await exchange_http(socket, std::move(wire));
        }
        auto context = boost::corosio::tls_context{};
        context.set_hostname(url.host);
        if (auto code = context.set_default_verify_paths(); code) {
            co_return std::unexpected(sync_error(errc::io, code.message(), url.host));
        }
        if (auto code = context.set_verify_mode(boost::corosio::tls_verify_mode::peer); code) {
            co_return std::unexpected(sync_error(errc::io, code.message(), url.host));
        }
        auto secure = boost::corosio::openssl_stream{&socket, context};
        auto [handshake_error] = co_await secure.handshake(boost::corosio::openssl_stream::client);
        if (handshake_error) {
            co_return std::unexpected(
                sync_error(errc::unavailable, handshake_error.message(), url.host));
        }
        auto response = co_await exchange_http(secure, std::move(wire));
        auto [shutdown_error] = co_await secure.shutdown();
        if (shutdown_error && shutdown_error != boost::capy::error::eof && response) {
            co_return std::unexpected(sync_error(errc::io, shutdown_error.message(), url.host));
        }
        co_return response;
    } catch (const std::exception& exception) {
        co_return std::unexpected(sync_error(errc::internal, exception.what(), url.host));
    }
}

auto endpoint_target(const parsed_url& url, std::string_view suffix) -> std::string {
    auto output = url.path == "/" ? std::string{} : url.path;
    output += suffix;
    return output;
}

auto reference_outcomes(std::span<const sync_item> sent, std::span<const sync_ref_outcome> received)
    -> result<std::vector<sync_ref_outcome>> {
    auto expected = std::vector<std::string>{};
    for (const auto& item : sent) {
        if (item.kind == sync_item_kind::reference) {
            expected.push_back(item.name);
        }
    }
    if (received.empty()) {
        auto legacy = std::vector<sync_ref_outcome>{};
        legacy.reserve(expected.size());
        for (auto& name : expected) {
            legacy.push_back(sync_ref_outcome{std::move(name)});
        }
        return legacy;
    }
    if (received.size() != expected.size()) {
        return std::unexpected(
            sync_error(errc::corrupt, "push reply has the wrong number of reference outcomes"));
    }
    auto output = std::vector<sync_ref_outcome>{received.begin(), received.end()};
    for (const auto& name : expected) {
        const auto count = std::ranges::count(output, name, &sync_ref_outcome::name);
        if (count != 1) {
            return std::unexpected(
                sync_error(errc::corrupt, "push reply has an invalid reference outcome", name));
        }
    }
    return output;
}

} // namespace

struct sync_endpoint::impl {
    parsed_url url;
    sync_endpoint_options options;
};

sync_endpoint::sync_endpoint(std::string repository_url, sync_endpoint_options options) {
    auto parsed = parse_url(repository_url);
    if (!parsed) {
        throw std::invalid_argument(parsed.error().message);
    }
    impl_ = std::make_unique<impl>(impl{std::move(*parsed), std::move(options)});
}

sync_endpoint::~sync_endpoint() = default;
sync_endpoint::sync_endpoint(sync_endpoint&&) noexcept = default;
sync_endpoint& sync_endpoint::operator=(sync_endpoint&&) noexcept = default;

runtime::task<std::vector<digest>> sync_endpoint::find_missing(std::string instance,
                                                               std::vector<digest> blobs) {
    static_cast<void>(instance);
    auto encoded = sync_detail::encode_object_list(blobs);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    auto response = co_await perform_request(impl_->url,
                                             impl_->options.bearer_token,
                                             boost::http::method::post,
                                             endpoint_target(impl_->url, "/sync/find-missing"),
                                             *encoded);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status < 200 || response->status >= 300) {
        co_return std::unexpected(status_error(response->status, "find-missing", response->body));
    }
    co_return sync_detail::decode_object_list(response->body);
}

runtime::task<std::uint64_t> sync_endpoint::read_range(std::string instance,
                                                       digest blob,
                                                       std::uint64_t offset,
                                                       std::span<std::byte> output) {
    static_cast<void>(instance);
    auto target = endpoint_target(impl_->url, "/sync/chunks/") + to_string(blob) +
                  "?offset=" + std::to_string(offset) + "&length=" + std::to_string(output.size());
    auto response = co_await perform_request(
        impl_->url, impl_->options.bearer_token, boost::http::method::get, std::move(target), {});
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status < 200 || response->status >= 300) {
        co_return std::unexpected(status_error(response->status, "chunk range", response->body));
    }
    if (response->body.size() > output.size()) {
        co_return std::unexpected(
            sync_error(errc::corrupt, "chunk range response exceeds output span"));
    }
    std::ranges::copy(response->body, output.begin());
    co_return response->body.size();
}

runtime::task<void>
sync_endpoint::write(std::string instance, digest blob, std::vector<std::byte> contents) {
    static_cast<void>(instance);
    auto frame = sync_detail::encode_pack_frame(blob, contents);
    if (!frame) {
        co_return std::unexpected(frame.error());
    }
    auto response = co_await perform_request(impl_->url,
                                             impl_->options.bearer_token,
                                             boost::http::method::post,
                                             endpoint_target(impl_->url, "/sync/chunks"),
                                             *frame);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status < 200 || response->status >= 300) {
        co_return std::unexpected(status_error(response->status, "chunk upload", response->body));
    }
    co_return result<void>{};
}

runtime::task<sync_report> sync_endpoint::push(repository& source) {
    auto document = sync_detail::metadata_document{};
    auto cursor = impl_->options.cursor;
    auto page = std::vector<sync_item>(impl_->options.batch_items);
    if (page.empty()) {
        co_return std::unexpected(
            sync_error(errc::invalid_argument, "sync batch size must be positive"));
    }
    for (;;) {
        auto exported = co_await source.export_drips(cursor, page);
        if (!exported) {
            co_return std::unexpected(exported.error());
        }
        document.items.insert(document.items.end(),
                              std::make_move_iterator(page.begin()),
                              std::make_move_iterator(page.begin() + exported->count));
        document.objects.insert(
            document.objects.end(), exported->objects.begin(), exported->objects.end());
        cursor = exported->cursor;
        if (exported->done) {
            break;
        }
    }
    document.cursor = cursor;
    document.atomic = impl_->options.atomic;
    auto session = sync_session{repository_detail::sync_access::objects(source), *this, {}};
    auto sent = co_await session.send(document.objects);
    if (!sent) {
        co_return std::unexpected(sent.error());
    }
    auto encoded = sync_detail::encode_metadata(document);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    auto response = co_await perform_request(impl_->url,
                                             impl_->options.bearer_token,
                                             boost::http::method::post,
                                             endpoint_target(impl_->url, "/sync/push"),
                                             *encoded);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status < 200 || response->status >= 300) {
        co_return std::unexpected(status_error(response->status, "push", response->body));
    }
    auto incoming = sync_detail::decode_metadata(response->body);
    if (!incoming) {
        co_return std::unexpected(incoming.error());
    }
    auto references = reference_outcomes(document.items, incoming->outcomes);
    if (!references) {
        co_return std::unexpected(references.error());
    }
    auto received = co_await session.receive(incoming->objects);
    if (!received) {
        co_return std::unexpected(received.error());
    }
    auto imported = co_await source.import_drips(incoming->items);
    if (!imported) {
        co_return std::unexpected(imported.error());
    }
    co_return sync_report{.cursor = incoming->cursor,
                          .metadata_items = document.items.size(),
                          .sent = *sent,
                          .received = *received,
                          .references = std::move(*references)};
}

runtime::task<sync_report> sync_endpoint::pull(repository& destination) {
    auto request_document = sync_detail::metadata_document{};
    request_document.cursor = impl_->options.cursor;
    auto request = sync_detail::encode_metadata(request_document);
    if (!request) {
        co_return std::unexpected(request.error());
    }
    auto response = co_await perform_request(impl_->url,
                                             impl_->options.bearer_token,
                                             boost::http::method::post,
                                             endpoint_target(impl_->url, "/sync/pull"),
                                             *request);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status < 200 || response->status >= 300) {
        co_return std::unexpected(status_error(response->status, "pull", response->body));
    }
    auto incoming = sync_detail::decode_metadata(response->body);
    if (!incoming) {
        co_return std::unexpected(incoming.error());
    }
    auto session = sync_session{repository_detail::sync_access::objects(destination), *this, {}};
    auto received = co_await session.receive(incoming->objects);
    if (!received) {
        co_return std::unexpected(received.error());
    }
    auto imported = co_await destination.import_drips(incoming->items);
    if (!imported) {
        co_return std::unexpected(imported.error());
    }
    co_return sync_report{.cursor = incoming->cursor,
                          .metadata_items = incoming->items.size(),
                          .received = *received};
}

} // namespace liquid
