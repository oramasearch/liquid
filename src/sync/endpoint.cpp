// SPDX-License-Identifier: MIT

#include "liquid/sync.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include <boost/http.hpp>

#include "encoding/cbor.hpp"
#include "net/http_client.hpp"
#include "repository/sync_access.hpp"
#include "sync/codec.hpp"

namespace liquid {
namespace {

constexpr auto content_type = "application/vnd.liquid.sync-v1";
namespace cbor = encoding::cbor;

auto sync_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
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
                            errc::internal,
                            errc::not_remote,
                            errc::not_configured}) {
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

auto flow_rejection(std::span<const std::byte> body)
    -> std::optional<std::pair<std::uint64_t, std::string>> {
    auto decoded = cbor::decode(body);
    if (!decoded) {
        return std::nullopt;
    }
    const auto* retry_value = cbor_field(*decoded, "retry_after_ms");
    const auto* scope_value = cbor_field(*decoded, "scope");
    const auto* retry = retry_value ? std::get_if<std::uint64_t>(&retry_value->data) : nullptr;
    const auto* scope = scope_value ? std::get_if<std::string>(&scope_value->data) : nullptr;
    if (!retry || !scope) {
        return std::nullopt;
    }
    return std::pair{*retry, *scope};
}

auto endpoint_target(const net_detail::parsed_url& url, std::string_view suffix) -> std::string {
    auto output = url.path == "/" ? std::string{} : url.path;
    output += suffix;
    return output;
}

auto endpoint_request(const net_detail::parsed_url& url,
                      std::string_view bearer,
                      boost::http::method method,
                      std::string target,
                      std::string_view content_type,
                      std::span<const std::byte> body) -> runtime::task<net_detail::http_response> {
    auto response = co_await net_detail::perform_request(net_detail::http_request{
        .method = method == boost::http::method::get ? net_detail::http_method::get
                                                     : net_detail::http_method::post,
        .url = url,
        .target = std::move(target),
        .bearer = std::string{bearer},
        .content_type = std::string{content_type},
        .body = {body.begin(), body.end()},
    });
    if (!response) {
        co_return std::unexpected(
            sync_error(response.error().code, response.error().message, "sync endpoint"));
    }
    co_return std::move(*response);
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
    net_detail::parsed_url url;
    std::string source;
    sync_endpoint_options options;
};

sync_endpoint::sync_endpoint(std::string repository_url, sync_endpoint_options options) {
    auto parsed = net_detail::parse_url(repository_url, "sync URL");
    if (!parsed) {
        throw std::invalid_argument(parsed.error().message);
    }
    impl_ = std::make_unique<impl>(
        impl{std::move(*parsed), std::move(repository_url), std::move(options)});
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
    auto response = co_await endpoint_request(impl_->url,
                                              impl_->options.bearer_token,
                                              boost::http::method::post,
                                              endpoint_target(impl_->url, "/sync/find-missing"),
                                              content_type,
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
    auto response = co_await endpoint_request(impl_->url,
                                              impl_->options.bearer_token,
                                              boost::http::method::get,
                                              std::move(target),
                                              content_type,
                                              {});
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
    auto response = co_await endpoint_request(impl_->url,
                                              impl_->options.bearer_token,
                                              boost::http::method::post,
                                              endpoint_target(impl_->url, "/sync/chunks"),
                                              content_type,
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
    auto response = co_await endpoint_request(impl_->url,
                                              impl_->options.bearer_token,
                                              boost::http::method::post,
                                              endpoint_target(impl_->url, "/sync/push"),
                                              content_type,
                                              *encoded);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status < 200 || response->status >= 300) {
        if (response->status == 429) {
            if (auto rejected = flow_rejection(response->body)) {
                co_return sync_report{.cursor = impl_->options.cursor,
                                      .metadata_items = document.items.size(),
                                      .sent = *sent,
                                      .retry_after_ms = rejected->first,
                                      .exhausted_scope = std::move(rejected->second)};
            }
        }
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
    auto response = co_await endpoint_request(impl_->url,
                                              impl_->options.bearer_token,
                                              boost::http::method::post,
                                              endpoint_target(impl_->url, "/sync/pull"),
                                              content_type,
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

runtime::task<sync_report> sync_endpoint::pull_metadata(repository& destination) {
    auto request_document = sync_detail::metadata_document{};
    request_document.cursor = impl_->options.cursor;
    auto request = sync_detail::encode_metadata(request_document);
    if (!request) {
        co_return std::unexpected(request.error());
    }
    auto response = co_await endpoint_request(impl_->url,
                                              impl_->options.bearer_token,
                                              boost::http::method::post,
                                              endpoint_target(impl_->url, "/sync/pull"),
                                              content_type,
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
    auto session = sync_session{
        repository_detail::sync_access::objects(destination), *this, impl_->source, {}};
    auto received = co_await session.receive_metadata(incoming->objects);
    if (!received) {
        co_return std::unexpected(received.error());
    }
    auto imported = co_await destination.import_drips(incoming->items);
    if (!imported) {
        co_return std::unexpected(imported.error());
    }
    co_return sync_report{.cursor = incoming->cursor,
                          .metadata_items = incoming->items.size(),
                          .received = {.objects = received->trees + received->blobs},
                          .bytes_remote = received->bytes_remote};
}

} // namespace liquid
