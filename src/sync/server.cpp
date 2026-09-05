#include "liquid/sync.hpp"

#include <algorithm>
#include <charconv>
#include <optional>
#include <utility>

#include "encoding/cbor.hpp"
#include "liquid/repository.hpp"
#include "liquid/store.hpp"
#include "repository/store_access.hpp"
#include "repository/sync_access.hpp"
#include "sync/codec.hpp"

namespace liquid {
namespace {

auto server_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto same_digest(const digest& left, const digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

} // namespace

struct sync_server::impl {
    repository* subject{};
    store* objects{};
    sync_server_options options;
    bool matching_store{};
};

sync_server::sync_server(repository& subject, sync_server_options options)
    : sync_server(subject, repository_detail::sync_access::objects(subject), std::move(options)) {}

sync_server::sync_server(repository& subject, store& objects, sync_server_options options)
    : impl_(std::make_unique<impl>(
          impl{&subject,
               &objects,
               std::move(options),
               &repository_detail::sync_access::objects(subject) == &objects})) {}

sync_server::~sync_server() = default;
sync_server::sync_server(sync_server&&) noexcept = default;
sync_server& sync_server::operator=(sync_server&&) noexcept = default;

runtime::task<std::vector<std::byte>> sync_server::handle_push(std::span<const std::byte> request) {
    if (!impl_ || !impl_->subject || !impl_->objects || !impl_->matching_store) {
        co_return std::unexpected(
            server_error(errc::invalid_argument, "sync server repository and store do not match"));
    }
    auto incoming = sync_detail::decode_metadata(request);
    if (!incoming) {
        co_return std::unexpected(incoming.error());
    }
    if (impl_->options.authorize) {
        for (const auto& item : incoming->items) {
            if (item.kind != sync_item_kind::change) {
                continue;
            }
            auto authorized = co_await impl_->options.authorize(item);
            if (!authorized) {
                co_return std::unexpected(authorized.error());
            }
        }
    }
    auto imported = co_await impl_->subject->import_drips(incoming->items);
    if (!imported) {
        co_return std::unexpected(imported.error());
    }
    co_return sync_detail::encode_metadata(
        sync_detail::metadata_document{.cursor = incoming->cursor});
}

runtime::task<std::vector<std::byte>> sync_server::handle_pull(std::span<const std::byte> request) {
    if (!impl_ || !impl_->subject || !impl_->objects || !impl_->matching_store) {
        co_return std::unexpected(
            server_error(errc::invalid_argument, "sync server repository and store do not match"));
    }
    if (impl_->options.batch_items == 0) {
        co_return std::unexpected(
            server_error(errc::invalid_argument, "sync server batch size must be positive"));
    }
    auto incoming = sync_detail::decode_metadata(request);
    if (!incoming) {
        co_return std::unexpected(incoming.error());
    }
    if (!incoming->items.empty() || !incoming->objects.empty()) {
        co_return std::unexpected(
            server_error(errc::invalid_argument, "pull request contains metadata or objects"));
    }
    auto outgoing = sync_detail::metadata_document{};
    auto cursor = incoming->cursor;
    auto page = std::vector<sync_item>(impl_->options.batch_items);
    for (;;) {
        auto exported = co_await impl_->subject->export_drips(cursor, page);
        if (!exported) {
            co_return std::unexpected(exported.error());
        }
        outgoing.items.insert(outgoing.items.end(),
                              std::make_move_iterator(page.begin()),
                              std::make_move_iterator(page.begin() + exported->count));
        outgoing.objects.insert(
            outgoing.objects.end(), exported->objects.begin(), exported->objects.end());
        cursor = exported->cursor;
        if (exported->done) {
            break;
        }
    }
    outgoing.cursor = std::move(cursor);
    co_return sync_detail::encode_metadata(outgoing);
}

runtime::task<std::vector<std::byte>>
sync_server::handle_find_missing(std::span<const std::byte> request) {
    if (!impl_ || !impl_->objects || !impl_->matching_store) {
        co_return std::unexpected(
            server_error(errc::invalid_argument, "sync server repository and store do not match"));
    }
    auto requested = sync_detail::decode_object_list(request);
    if (!requested) {
        co_return std::unexpected(requested.error());
    }
    auto missing = std::vector<digest>{};
    for (const auto& id : *requested) {
        auto present = co_await impl_->objects->contains(id);
        if (!present) {
            co_return std::unexpected(present.error());
        }
        if (!*present) {
            missing.push_back(id);
        }
    }
    co_return sync_detail::encode_object_list(missing);
}

runtime::task<void> sync_server::handle_chunks(std::span<const std::byte> request) {
    if (!impl_ || !impl_->objects || !impl_->matching_store) {
        co_return std::unexpected(
            server_error(errc::invalid_argument, "sync server repository and store do not match"));
    }
    auto frames = sync_detail::decode_pack_frames(request);
    if (!frames) {
        co_return std::unexpected(frames.error());
    }
    for (auto& [id, contents] : *frames) {
        auto stored = co_await repository_detail::store_access::put_verified(
            *impl_->objects, id, std::move(contents));
        if (!stored) {
            co_return std::unexpected(stored.error());
        }
        if (!same_digest(*stored, id)) {
            co_return std::unexpected(
                server_error(errc::corrupt, "stored object identity changed", to_string(id)));
        }
    }
    co_return result<void>{};
}

runtime::task<std::uint64_t>
sync_server::handle_chunks(digest id, std::uint64_t offset, std::span<std::byte> output) {
    if (!impl_ || !impl_->objects || !impl_->matching_store) {
        co_return std::unexpected(
            server_error(errc::invalid_argument, "sync server repository and store do not match"));
    }
    co_return co_await impl_->objects->read(id, offset, output);
}

result<std::vector<std::byte>> sync_server::encode_error(const error& value) {
    auto context = value.context;
    auto item = std::optional<std::uint64_t>{};
    if (context.starts_with("item=")) {
        const auto separator = context.find(':');
        const auto number = std::string_view{context}.substr(5, separator - 5);
        auto parsed_item = std::uint64_t{};
        const auto parsed =
            std::from_chars(number.data(), number.data() + number.size(), parsed_item);
        if (parsed.ec == std::errc{} && parsed.ptr == number.data() + number.size()) {
            item = parsed_item;
            context =
                separator == std::string::npos ? std::string{} : context.substr(separator + 1);
            if (context.starts_with(' ')) {
                context.erase(0, 1);
            }
        }
    }
    auto fields = encoding::cbor::value::map{
        {"v", encoding::cbor::value{std::uint64_t{1}}},
        {"code", std::string{to_string(value.code)}},
        {"message", value.message},
        {"context", std::move(context)},
    };
    if (item) {
        fields.emplace_back("item", *item);
    }
    return encoding::cbor::encode(encoding::cbor::value{std::move(fields)});
}

} // namespace liquid
