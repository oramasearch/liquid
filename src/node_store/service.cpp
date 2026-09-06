#include "node_store/service.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "formats/json.hpp"
#include "liquid/repository.hpp"
#include "liquid/store.hpp"
#include "liquid/sync.hpp"
#include "liquid/vfs.hpp"
#include "objects/records.hpp"
#include "serve/http.hpp"
#include "vfs/backend.hpp"

namespace liquid::node_store_detail {
namespace {

using json = formats::json::value;

auto failure(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto same_digest(const digest& left, const digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

auto json_text(std::string value) -> json {
    return json{std::move(value)};
}

auto json_number(std::uint64_t value) -> json {
    return json{formats::json::number{std::to_string(value)}};
}

auto encode_json(json value) -> result<std::string> {
    return formats::json::write(value);
}

auto object_field(const json& value, std::string_view name) -> const json* {
    const auto* object = std::get_if<json::object>(&value.data);
    if (!object) {
        return nullptr;
    }
    const auto found = std::ranges::find(*object, name, &std::pair<std::string, json>::first);
    return found == object->end() ? nullptr : &found->second;
}

auto text_field(const json& value, std::string_view name, bool required = true)
    -> result<std::string> {
    const auto* field = object_field(value, name);
    if (!field && !required) {
        return std::string{};
    }
    const auto* text = field ? std::get_if<std::string>(&field->data) : nullptr;
    if (!text) {
        return std::unexpected(failure(
            errc::invalid_argument, "node-store JSON text field is missing", std::string{name}));
    }
    return *text;
}

auto number_field(const json& value, std::string_view name) -> result<std::uint64_t> {
    const auto* field = object_field(value, name);
    const auto* number = field ? std::get_if<formats::json::number>(&field->data) : nullptr;
    if (!number) {
        return std::unexpected(failure(
            errc::invalid_argument, "node-store JSON number field is missing", std::string{name}));
    }
    auto output = std::uint64_t{};
    const auto parsed =
        std::from_chars(number->text.data(), number->text.data() + number->text.size(), output);
    if (parsed.ec != std::errc{} || parsed.ptr != number->text.data() + number->text.size()) {
        return std::unexpected(failure(
            errc::invalid_argument, "node-store JSON number is invalid", std::string{name}));
    }
    return output;
}

auto parse_object(std::string_view body) -> result<json> {
    auto parsed = formats::json::parse(body);
    if (!parsed || !std::holds_alternative<json::object>(parsed->data)) {
        return std::unexpected(
            parsed ? failure(errc::invalid_argument, "node-store request must be a JSON object")
                   : parsed.error());
    }
    return parsed;
}

auto parse_digest_field(const json& value, std::string_view name) -> result<digest> {
    auto text = text_field(value, name);
    if (!text) {
        return std::unexpected(text.error());
    }
    auto parsed = parse_digest(*text);
    if (!parsed) {
        return std::unexpected(
            failure(errc::invalid_argument, "node-store digest is invalid", std::move(*text)));
    }
    return parsed;
}

auto base64(std::span<const std::byte> input) -> std::string {
    static constexpr auto alphabet =
        std::string_view{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
    auto output = std::string{};
    output.reserve(((input.size() + 2U) / 3U) * 4U);
    for (auto offset = std::size_t{}; offset < input.size(); offset += 3U) {
        const auto first = std::to_integer<unsigned>(input[offset]);
        const auto second =
            offset + 1U < input.size() ? std::to_integer<unsigned>(input[offset + 1U]) : 0U;
        const auto third =
            offset + 2U < input.size() ? std::to_integer<unsigned>(input[offset + 2U]) : 0U;
        const auto value = (first << 16U) | (second << 8U) | third;
        output.push_back(alphabet[(value >> 18U) & 0x3fU]);
        output.push_back(alphabet[(value >> 12U) & 0x3fU]);
        output.push_back(offset + 1U < input.size() ? alphabet[(value >> 6U) & 0x3fU] : '=');
        output.push_back(offset + 2U < input.size() ? alphabet[value & 0x3fU] : '=');
    }
    return output;
}

class reporting_source final : public remote_blob_source {
public:
    reporting_source(std::shared_ptr<remote_blob_source> source,
                     std::function<void(const error&)> report)
        : source_(std::move(source)), report_(std::move(report)) {}

    runtime::task<std::vector<digest>> find_missing(std::string instance,
                                                    std::vector<digest> blobs) override {
        auto found = co_await source_->find_missing(std::move(instance), std::move(blobs));
        if (!found) {
            report_(found.error());
        }
        co_return found;
    }

    runtime::task<std::uint64_t> read_range(std::string instance,
                                            digest content,
                                            std::uint64_t offset,
                                            std::span<std::byte> output) override {
        auto read = co_await source_->read_range(std::move(instance), content, offset, output);
        if (!read) {
            report_(read.error());
        }
        co_return read;
    }

    runtime::task<void>
    write(std::string instance, digest content, std::vector<std::byte> bytes) override {
        auto written = co_await source_->write(std::move(instance), content, std::move(bytes));
        if (!written) {
            report_(written.error());
        }
        co_return written;
    }

private:
    std::shared_ptr<remote_blob_source> source_;
    std::function<void(const error&)> report_;
};

struct catalog_blob {
    digest content;
    std::vector<digest> chunks;
    std::vector<std::uint64_t> sizes;
};

struct mounted_image {
    repository image;
    liquid::mount mounted;
    node_store_mount status;
    std::vector<catalog_blob> blobs;
};

auto mount_json(const node_store_mount& value) -> json {
    return json{json::object{{"tree", json_text(to_string(value.tree))},
                             {"path", json_text(value.path.string())},
                             {"references", json_number(value.references)},
                             {"chunks_fetched", json_number(value.chunks_fetched)},
                             {"bytes_fetched", json_number(value.bytes_fetched)},
                             {"bytes_avoided", json_number(value.bytes_avoided)},
                             {"hydrated_bytes", json_number(value.hydrated_bytes)}}};
}

auto status_json(const node_store_status& value) -> result<std::string> {
    auto mounts = json::array{};
    for (const auto& mounted : value.mounts) {
        mounts.push_back(mount_json(mounted));
    }
    auto object = json::object{{"store_bytes", json_number(value.store_bytes)},
                               {"budget_bytes", json_number(value.budget_bytes)},
                               {"evictions", json_number(value.evictions)},
                               {"mounts", json{std::move(mounts)}}};
    if (value.last_error) {
        object.emplace_back("last_error", json_text(*value.last_error));
    } else {
        object.emplace_back("last_error", json{formats::json::null{}});
    }
    return encode_json(json{std::move(object)});
}

auto mounts_json(const node_store_status& value) -> result<std::string> {
    auto mounts = json::array{};
    for (const auto& mounted : value.mounts) {
        mounts.push_back(mount_json(mounted));
    }
    return encode_json(json{std::move(mounts)});
}

auto mount_result_json(const mount_result& value) -> result<std::string> {
    return encode_json(json{json::object{{"path", json_text(value.path.string())},
                                         {"tree", json_text(to_string(value.tree))},
                                         {"records_fetched", json_number(value.records_fetched)},
                                         {"bytes_remote", json_number(value.bytes_remote)}}});
}

auto query_value(std::string_view query, std::string_view name) -> std::optional<std::string> {
    for (const auto part : std::views::split(query, '&')) {
        const auto item = std::string_view{part};
        const auto separator = item.find('=');
        if (separator != std::string_view::npos && item.substr(0, separator) == name) {
            return std::string{item.substr(separator + 1U)};
        }
    }
    return std::nullopt;
}

} // namespace

struct service::impl {
    explicit impl(node_store_options value) : options(std::move(value)) {}

    node_store_options options;
    store storage;
    bool store_open{};
    bool socket_open{};
    std::uint64_t evictions{};
    std::optional<std::string> last_error;
    std::map<std::string, std::shared_ptr<remote_blob_source>> sources;
    std::map<std::string, std::unique_ptr<mounted_image>> mounts;
    std::map<std::string, std::string> content_owner;
    std::map<std::string, digest> remote_content;
    std::map<std::string, std::chrono::steady_clock::time_point> last_read;

    auto collect_blobs(digest root) -> runtime::task<std::vector<catalog_blob>> {
        auto pending = std::vector<digest>{root};
        auto seen_trees = std::set<std::string>{};
        auto seen_blobs = std::set<std::string>{};
        auto output = std::vector<catalog_blob>{};
        while (!pending.empty()) {
            const auto current = pending.back();
            pending.pop_back();
            if (!seen_trees.insert(to_string(current)).second) {
                continue;
            }
            auto bytes = co_await storage.get(current);
            if (!bytes) {
                co_return std::unexpected(bytes.error());
            }
            auto tree = objects::decode_tree(*bytes);
            if (!tree) {
                co_return std::unexpected(tree.error());
            }
            for (const auto& entry : tree->entries) {
                if (entry.kind == node_kind::tree || entry.kind == node_kind::repo) {
                    pending.push_back(entry.ref);
                    continue;
                }
                if (entry.kind != node_kind::blob) {
                    co_return std::unexpected(
                        failure(errc::corrupt, "image tree entry is unsupported", entry.name));
                }
                if (!seen_blobs.insert(to_string(entry.ref)).second) {
                    continue;
                }
                auto record_bytes = co_await storage.get(entry.ref);
                if (!record_bytes) {
                    co_return std::unexpected(record_bytes.error());
                }
                auto record = objects::decode_blob(*record_bytes);
                if (!record) {
                    co_return std::unexpected(record.error());
                }
                output.push_back(
                    {record->content, std::move(record->chunks), std::move(record->chunk_sizes)});
            }
        }
        co_return output;
    }

    auto rebuild_owners() -> void {
        content_owner.clear();
        for (const auto& [tree, mounted] : mounts) {
            for (const auto& blob : mounted->blobs) {
                content_owner.try_emplace(to_string(blob.content), tree);
            }
        }
    }

    auto enforce_budget(std::optional<digest> active = std::nullopt) -> runtime::task<void> {
        if (options.budget_bytes == 0) {
            co_return result<void>{};
        }
        for (;;) {
            auto bytes = co_await storage.local_bytes();
            if (!bytes) {
                co_return std::unexpected(bytes.error());
            }
            if (*bytes <= options.budget_bytes) {
                co_return result<void>{};
            }
            auto candidates =
                std::vector<std::pair<std::chrono::steady_clock::time_point, digest>>{};
            for (const auto& [key, content] : remote_content) {
                if (active && same_digest(*active, content)) {
                    continue;
                }
                const auto read = last_read.find(key);
                candidates.emplace_back(read == last_read.end()
                                            ? std::chrono::steady_clock::time_point{}
                                            : read->second,
                                        content);
            }
            std::ranges::sort(candidates, {}, &decltype(candidates)::value_type::first);
            auto evicted = false;
            auto tried = std::set<std::string>{};
            for (const auto& [unused, content] : candidates) {
                if (!tried.insert(to_string(content)).second) {
                    continue;
                }
                auto available = co_await storage.has(content);
                if (!available) {
                    co_return std::unexpected(available.error());
                }
                if (*available != blob_availability::local) {
                    continue;
                }
                auto removed = co_await storage.evict(content);
                if (!removed) {
                    if (removed.error().code == errc::not_remote) {
                        continue;
                    }
                    co_return std::unexpected(removed.error());
                }
                ++evictions;
                evicted = true;
                break;
            }
            if (!evicted) {
                co_return std::unexpected(failure(errc::unavailable,
                                                  "node-store budget has no eligible content",
                                                  std::to_string(options.budget_bytes)));
            }
        }
    }

    auto hydrated(digest content, std::uint64_t bytes, bool complete) -> runtime::task<void> {
        const auto key = to_string(content);
        last_read[key] = std::chrono::steady_clock::now();
        const auto owner = content_owner.find(key);
        if (owner != content_owner.end()) {
            const auto mounted = mounts.find(owner->second);
            if (mounted != mounts.end()) {
                ++mounted->second->status.chunks_fetched;
                mounted->second->status.bytes_fetched += bytes;
            }
        }
        auto enforced =
            co_await enforce_budget(complete ? std::nullopt : std::optional<digest>{content});
        if (!enforced) {
            last_error = std::format("{} ({})", enforced.error().message, enforced.error().context);
        }
        co_return result<void>{};
    }

    void accessed(digest content) {
        last_read[to_string(content)] = std::chrono::steady_clock::now();
    }
};

service::service(node_store_options options) : impl_(std::make_unique<impl>(std::move(options))) {}

runtime::task<std::shared_ptr<service>> service::open(node_store_options options) {
    if (options.root.empty()) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "node-store root is required", "--root"));
    }
    auto output = std::shared_ptr<service>{new service(std::move(options))};
    const auto weak = std::weak_ptr<service>{output};
    auto store_options = liquid::store_options{};
    store_options.on_hydration =
        [weak](digest content, digest, std::uint64_t bytes, bool complete) -> runtime::task<void> {
        const auto locked = weak.lock();
        if (!locked) {
            co_return result<void>{};
        }
        co_return co_await locked->impl_->hydrated(content, bytes, complete);
    };
    store_options.on_remote_access = [weak](digest content) {
        if (const auto locked = weak.lock()) {
            locked->impl_->accessed(content);
        }
    };
    auto opened = co_await store::open(output->impl_->options.root / "store", store_options);
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    output->impl_->storage = std::move(*opened);
    output->impl_->store_open = true;
    co_return output;
}

runtime::task<mount_result> service::mount_image(mount_request request) {
    const auto tree_key = to_string(request.tree);
    if (const auto found = impl_->mounts.find(tree_key); found != impl_->mounts.end()) {
        ++found->second->status.references;
        co_return mount_result{found->second->status.path, request.tree, 0, 0};
    }
    if (request.store_url.empty()) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "node-store mount requires a store URL", tree_key));
    }
    auto source = impl_->sources.find(request.store_url);
    if (source == impl_->sources.end()) {
        auto endpoint = std::make_shared<sync_endpoint>(
            request.store_url, sync_endpoint_options{.bearer_token = std::move(request.bearer)});
        const auto weak = weak_from_this();
        auto reporting = std::make_shared<reporting_source>(
            std::move(endpoint), [weak, url = request.store_url](const error& value) {
                if (const auto locked = weak.lock()) {
                    locked->note_error(error{value.code, value.message, url});
                }
            });
        source = impl_->sources.emplace(request.store_url, std::move(reporting)).first;
    }
    auto registered =
        co_await impl_->storage.register_remote_source(request.store_url, source->second);
    if (!registered) {
        note_error(registered.error());
        co_return std::unexpected(registered.error());
    }
    auto session =
        sync_session{impl_->storage, *source->second, request.store_url, request.store_url};
    const auto roots = std::array{request.tree};
    auto received = co_await session.receive_metadata(roots);
    if (!received) {
        note_error(received.error());
        if (received.error().code == errc::unavailable || received.error().code == errc::io ||
            received.error().code == errc::not_found) {
            auto unavailable = failure(
                errc::unavailable, "image metadata store is unavailable", request.store_url);
            note_error(unavailable);
            co_return std::unexpected(std::move(unavailable));
        }
        co_return std::unexpected(received.error());
    }
    auto blobs = co_await impl_->collect_blobs(request.tree);
    if (!blobs) {
        note_error(blobs.error());
        co_return std::unexpected(blobs.error());
    }
    auto avoided = std::uint64_t{};
    for (const auto& blob : *blobs) {
        impl_->remote_content.insert_or_assign(to_string(blob.content), blob.content);
        for (auto index = std::size_t{}; index < blob.chunks.size(); ++index) {
            auto available = co_await impl_->storage.has(blob.chunks[index]);
            if (!available) {
                co_return std::unexpected(available.error());
            }
            if (*available == blob_availability::local) {
                avoided += blob.sizes[index];
            }
        }
        impl_->last_read.try_emplace(to_string(blob.content), std::chrono::steady_clock::now());
    }
    const auto image_path = impl_->options.root / "images" / tree_key;
    auto exists =
        co_await runtime::offload([image_path] { return std::filesystem::exists(image_path); });
    if (!exists) {
        co_return std::unexpected(exists.error());
    }
    auto repository_options = liquid::repository_options{.store = &impl_->storage};
    auto image = *exists ? co_await repository::open(image_path, repository_options)
                         : co_await repository::create(image_path, repository_options);
    if (!image) {
        co_return std::unexpected(image.error());
    }
    auto input = change_input{};
    input.author = actor{"liquid:node-store"};
    input.at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    input.message = "mount image root";
    input.ref = "image";
    auto recorded = co_await image->record_from_tree(std::move(input), request.tree);
    if (!recorded) {
        co_return std::unexpected(recorded.error());
    }
    const auto mount_path = impl_->options.root / "mounts" / tree_key;
    auto backend =
        co_await vfs_detail::start_observed_fuse(*image, request.tree, mount_path, {}, {}, true);
    if (!backend) {
        note_error(backend.error());
        co_return std::unexpected(backend.error());
    }
    auto value = std::make_unique<mounted_image>();
    value->image = std::move(*image);
    value->mounted = vfs_detail::mount_factory::make(std::move(*backend));
    value->status = node_store_mount{request.tree, mount_path, 1, 0, 0, avoided, 0};
    value->blobs = std::move(*blobs);
    impl_->mounts.emplace(tree_key, std::move(value));
    impl_->rebuild_owners();
    co_return mount_result{
        mount_path, request.tree, received->trees + received->blobs, received->bytes_remote};
}

runtime::task<void> service::unmount_image(unmount_request request) {
    if (!request.tree && !request.path) {
        co_return std::unexpected(failure(
            errc::invalid_argument, "node-store unmount requires tree or path", "/unmount"));
    }
    auto by_tree = impl_->mounts.end();
    if (request.tree) {
        by_tree = impl_->mounts.find(to_string(*request.tree));
    }
    auto by_path = impl_->mounts.end();
    if (request.path) {
        by_path = std::ranges::find_if(impl_->mounts, [&](const auto& item) {
            return item.second->status.path == *request.path;
        });
    }
    if (request.tree && request.path && by_tree != by_path) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "node-store unmount selectors disagree"));
    }
    auto selected = request.tree ? by_tree : by_path;
    if (selected == impl_->mounts.end()) {
        co_return std::unexpected(
            failure(errc::not_found, "node-store mount is not active", "/unmount"));
    }
    if (--selected->second->status.references != 0) {
        co_return result<void>{};
    }
    auto unmounted = co_await selected->second->mounted.unmount();
    if (!unmounted) {
        note_error(unmounted.error());
        co_return std::unexpected(unmounted.error());
    }
    impl_->mounts.erase(selected);
    impl_->rebuild_owners();
    co_return result<void>{};
}

runtime::task<node_store_status> service::prefetch(prefetch_request request) {
    const auto found = impl_->mounts.find(to_string(request.tree));
    if (found == impl_->mounts.end()) {
        co_return std::unexpected(
            failure(errc::not_found, "node-store mount is not active", to_string(request.tree)));
    }
    for (const auto& blob : found->second->blobs) {
        auto content = co_await impl_->storage.get(blob.content);
        if (!content) {
            note_error(content.error());
            co_return std::unexpected(content.error());
        }
        impl_->last_read[to_string(blob.content)] = std::chrono::steady_clock::now();
    }
    auto enforced = co_await impl_->enforce_budget();
    if (!enforced) {
        note_error(enforced.error());
        co_return std::unexpected(enforced.error());
    }
    co_return co_await status();
}

runtime::task<node_store_status> service::status() {
    auto bytes = co_await impl_->storage.local_bytes();
    if (!bytes) {
        co_return std::unexpected(bytes.error());
    }
    auto output = node_store_status{
        *bytes, impl_->options.budget_bytes, impl_->evictions, {}, impl_->last_error};
    for (auto& [unused, mounted] : impl_->mounts) {
        auto current = mounted->mounted.status();
        if (current) {
            mounted->status.hydrated_bytes = current->hydrated_bytes;
        }
        output.mounts.push_back(mounted->status);
    }
    co_return output;
}

runtime::task<void> service::evict(digest content) {
    auto available = co_await impl_->storage.has(content);
    if (!available) {
        co_return std::unexpected(available.error());
    }
    if (*available != blob_availability::local) {
        co_return std::unexpected(
            failure(errc::not_found, "remote content is not local", to_string(content)));
    }
    auto removed = co_await impl_->storage.evict(content);
    if (!removed) {
        note_error(removed.error());
        co_return std::unexpected(removed.error());
    }
    ++impl_->evictions;
    co_return result<void>{};
}

runtime::task<void> service::set_budget(std::uint64_t bytes) {
    impl_->options.budget_bytes = bytes;
    auto enforced = co_await impl_->enforce_budget();
    if (!enforced) {
        note_error(enforced.error());
        co_return std::unexpected(enforced.error());
    }
    co_return result<void>{};
}

runtime::task<std::string> service::attestations(digest subject) {
    auto values = json::array{};
    for (const auto& id : impl_->storage.attestations(subject)) {
        auto bytes = co_await impl_->storage.get(id);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        values.emplace_back(json::object{{"digest", json_text(to_string(id))},
                                         {"canonical_bytes_base64", json_text(base64(*bytes))}});
    }
    co_return encode_json(json{std::move(values)});
}

runtime::task<digest> service::store_local_attestation(attestation value) {
    auto encoded = objects::encode(value);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    co_return co_await impl_->storage.put(std::move(*encoded));
}

runtime::task<void> service::register_source(std::string id,
                                             std::shared_ptr<remote_blob_source> source) {
    if (id.empty() || !source) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "node-store remote source is invalid"));
    }
    const auto weak = weak_from_this();
    auto reporting =
        std::make_shared<reporting_source>(std::move(source), [weak, url = id](const error& value) {
            if (const auto locked = weak.lock()) {
                locked->note_error(error{value.code, value.message, url});
            }
        });
    impl_->sources.insert_or_assign(std::move(id), std::move(reporting));
    co_return result<void>{};
}

const node_store_options& service::options() const noexcept {
    return impl_->options;
}

bool service::ready() const noexcept {
    return impl_->store_open && impl_->socket_open;
}

void service::socket_bound() noexcept {
    impl_->socket_open = true;
}

void service::note_error(const error& value) {
    impl_->last_error = std::format("{} ({})", value.message, value.context);
}

std::shared_ptr<const serve::route_table> service::unix_routes() {
    const auto self = shared_from_this();
    auto routes = std::make_shared<serve::route_table>();
    routes->add("POST",
                "/mount",
                [self](const serve::http_request& request) -> runtime::task<serve::http_response> {
                    auto body = parse_object(request.body);
                    if (!body) {
                        co_return std::unexpected(body.error());
                    }
                    auto tree = parse_digest_field(*body, "tree");
                    auto store_url = text_field(*body, "store_url");
                    auto bearer = text_field(*body, "bearer", false);
                    if (!tree || !store_url || !bearer) {
                        co_return std::unexpected(!tree        ? tree.error()
                                                  : !store_url ? store_url.error()
                                                               : bearer.error());
                    }
                    auto mounted = co_await self->mount_image({*tree, *store_url, *bearer});
                    if (!mounted) {
                        co_return std::unexpected(mounted.error());
                    }
                    auto encoded = mount_result_json(*mounted);
                    if (!encoded) {
                        co_return std::unexpected(encoded.error());
                    }
                    co_return serve::make_response(200, "application/json", std::move(*encoded));
                });
    routes->add("POST",
                "/unmount",
                [self](const serve::http_request& request) -> runtime::task<serve::http_response> {
                    auto body = parse_object(request.body);
                    if (!body) {
                        co_return std::unexpected(body.error());
                    }
                    auto value = unmount_request{};
                    if (object_field(*body, "tree")) {
                        auto tree = parse_digest_field(*body, "tree");
                        if (!tree) {
                            co_return std::unexpected(tree.error());
                        }
                        value.tree = *tree;
                    }
                    if (object_field(*body, "path")) {
                        auto path = text_field(*body, "path");
                        if (!path) {
                            co_return std::unexpected(path.error());
                        }
                        value.path = *path;
                    }
                    auto unmounted = co_await self->unmount_image(std::move(value));
                    if (!unmounted) {
                        co_return std::unexpected(unmounted.error());
                    }
                    co_return serve::make_response(200, "application/json", "{}\n");
                });
    routes->add("POST",
                "/prefetch",
                [self](const serve::http_request& request) -> runtime::task<serve::http_response> {
                    auto body = parse_object(request.body);
                    if (!body) {
                        co_return std::unexpected(body.error());
                    }
                    auto tree = parse_digest_field(*body, "tree");
                    if (!tree) {
                        co_return std::unexpected(tree.error());
                    }
                    auto status = co_await self->prefetch({*tree});
                    if (!status) {
                        co_return std::unexpected(status.error());
                    }
                    auto encoded = status_json(*status);
                    if (!encoded) {
                        co_return std::unexpected(encoded.error());
                    }
                    co_return serve::make_response(200, "application/json", std::move(*encoded));
                });
    routes->add("GET",
                "/status",
                [self](const serve::http_request&) -> runtime::task<serve::http_response> {
                    auto status = co_await self->status();
                    if (!status) {
                        co_return std::unexpected(status.error());
                    }
                    auto encoded = status_json(*status);
                    if (!encoded) {
                        co_return std::unexpected(encoded.error());
                    }
                    co_return serve::make_response(200, "application/json", std::move(*encoded));
                });
    return routes;
}

namespace {

void add_liveness_routes(const std::shared_ptr<serve::route_table>& routes,
                         const std::shared_ptr<service>& self) {
    routes->add(
        "GET", "/healthz", [](const serve::http_request&) -> runtime::task<serve::http_response> {
            co_return serve::make_response(200, "text/plain", "ok\n");
        });
    routes->add("GET",
                "/readyz",
                [self](const serve::http_request&) -> runtime::task<serve::http_response> {
                    co_return serve::make_response(self->ready() ? 200U : 503U,
                                                   "text/plain",
                                                   self->ready() ? "ready\n" : "not ready\n");
                });
}

void add_status_routes(const std::shared_ptr<serve::route_table>& routes,
                       const std::shared_ptr<service>& self) {
    routes->add(
        "GET", "/v1", [self](const serve::http_request&) -> runtime::task<serve::http_response> {
            auto status = co_await self->status();
            if (!status) {
                co_return std::unexpected(status.error());
            }
            auto encoded = status_json(*status);
            if (!encoded) {
                co_return std::unexpected(encoded.error());
            }
            co_return serve::make_response(200, "application/json", std::move(*encoded));
        });
    routes->add("GET",
                "/v1/mounts",
                [self](const serve::http_request&) -> runtime::task<serve::http_response> {
                    auto status = co_await self->status();
                    if (!status) {
                        co_return std::unexpected(status.error());
                    }
                    auto encoded = mounts_json(*status);
                    if (!encoded) {
                        co_return std::unexpected(encoded.error());
                    }
                    co_return serve::make_response(200, "application/json", std::move(*encoded));
                });
    routes->add(
        "GET",
        "/metrics",
        [self](const serve::http_request&) -> runtime::task<serve::http_response> {
            auto status = co_await self->status();
            if (!status) {
                co_return std::unexpected(status.error());
            }
            auto output = std::string{};
            for (const auto& mounted : status->mounts) {
                const auto tree = to_string(mounted.tree);
                output += std::format("liquid_node_store_chunks_fetched_total{{tree=\"{}\"}} {}\n",
                                      tree,
                                      mounted.chunks_fetched);
                output += std::format("liquid_node_store_bytes_fetched_total{{tree=\"{}\"}} {}\n",
                                      tree,
                                      mounted.bytes_fetched);
                output += std::format("liquid_node_store_bytes_avoided_total{{tree=\"{}\"}} {}\n",
                                      tree,
                                      mounted.bytes_avoided);
            }
            output += std::format("liquid_node_store_store_bytes {}\n", status->store_bytes);
            output += std::format("liquid_node_store_budget_bytes {}\n", status->budget_bytes);
            output += std::format("liquid_node_store_evictions_total {}\n", status->evictions);
            co_return serve::make_response(200, "text/plain; version=0.0.4", std::move(output));
        });
}

void add_admin_routes(const std::shared_ptr<serve::route_table>& routes,
                      const std::shared_ptr<service>& self) {
    routes->add("GET",
                "/v1/attestations",
                [self](const serve::http_request& request) -> runtime::task<serve::http_response> {
                    const auto subject_text = query_value(request.query, "subject");
                    auto subject =
                        subject_text ? parse_digest(*subject_text)
                                     : result<digest>{std::unexpected(
                                           failure(errc::invalid_argument, "subject is required"))};
                    if (!subject) {
                        co_return std::unexpected(subject.error());
                    }
                    auto values = co_await self->attestations(*subject);
                    if (!values) {
                        co_return std::unexpected(values.error());
                    }
                    co_return serve::make_response(200, "application/json", std::move(*values));
                });
    routes->add("POST",
                "/v1/evict",
                [self](const serve::http_request& request) -> runtime::task<serve::http_response> {
                    auto body = parse_object(request.body);
                    if (!body) {
                        co_return std::unexpected(body.error());
                    }
                    auto content = parse_digest_field(*body, "digest");
                    if (!content) {
                        co_return std::unexpected(content.error());
                    }
                    auto evicted = co_await self->evict(*content);
                    if (!evicted) {
                        co_return std::unexpected(evicted.error());
                    }
                    co_return serve::make_response(200, "application/json", "{}\n");
                });
    routes->add("POST",
                "/v1/budget",
                [self](const serve::http_request& request) -> runtime::task<serve::http_response> {
                    auto body = parse_object(request.body);
                    if (!body) {
                        co_return std::unexpected(body.error());
                    }
                    auto bytes = number_field(*body, "bytes");
                    if (!bytes) {
                        co_return std::unexpected(bytes.error());
                    }
                    auto changed = co_await self->set_budget(*bytes);
                    if (!changed) {
                        co_return std::unexpected(changed.error());
                    }
                    co_return serve::make_response(200, "application/json", "{}\n");
                });
}

void add_openapi_route(const std::shared_ptr<serve::route_table>& routes) {
    routes->add(
        "GET",
        "/v1/openapi.json",
        [](const serve::http_request&) -> runtime::task<serve::http_response> {
            co_return serve::make_response(
                200,
                "application/json",
                R"({"openapi":"3.1.0","info":{"title":"liquid node store","version":"v1"},"paths":{"/healthz":{"get":{}},"/readyz":{"get":{}},"/metrics":{"get":{}},"/v1":{"get":{}},"/v1/mounts":{"get":{}},"/v1/attestations":{"get":{}},"/v1/evict":{"post":{}},"/v1/budget":{"post":{}},"/v1/openapi.json":{"get":{}},"/mcp":{"post":{}}}})");
        });
}

void add_mcp_route(const std::shared_ptr<serve::route_table>& routes,
                   const std::shared_ptr<service>& self) {
    routes->add(
        "POST",
        "/mcp",
        [self](const serve::http_request& request) -> runtime::task<serve::http_response> {
            const auto id = serve::json_rpc_id(request.body);
            if (request.body.find("\"method\":\"initialize\"") != std::string::npos) {
                co_return serve::make_response(
                    200,
                    "application/json",
                    std::format(
                        R"({{"jsonrpc":"2.0","id":{},"result":{{"protocolVersion":"2025-06-18","capabilities":{{}},"serverInfo":{{"name":"liquid-node-store","version":"v1"}}}}}})",
                        id));
            }
            if (request.body.find("\"method\":\"tools/list\"") != std::string::npos) {
                co_return serve::make_response(
                    200,
                    "application/json",
                    std::format(
                        R"({{"jsonrpc":"2.0","id":{},"result":{{"tools":[{{"name":"node_store_status","inputSchema":{{"type":"object"}}}},{{"name":"node_store_mounts","inputSchema":{{"type":"object"}}}},{{"name":"node_store_evict","inputSchema":{{"type":"object","properties":{{"digest":{{"type":"string"}}}},"required":["digest"]}}}}]}}}})",
                        id));
            }
            if (request.body.find("node_store_evict") != std::string::npos) {
                auto request_value = parse_object(request.body);
                const auto* parameters =
                    request_value ? object_field(*request_value, "params") : nullptr;
                const auto* arguments =
                    parameters ? object_field(*parameters, "arguments") : nullptr;
                auto content =
                    arguments ? parse_digest_field(*arguments, "digest")
                              : result<digest>{std::unexpected(failure(
                                    errc::invalid_argument, "node_store_evict requires digest"))};
                if (!content) {
                    co_return std::unexpected(content.error());
                }
                auto evicted = co_await self->evict(*content);
                if (!evicted) {
                    co_return std::unexpected(evicted.error());
                }
                co_return serve::make_response(
                    200,
                    "application/json",
                    std::format(
                        R"({{"jsonrpc":"2.0","id":{},"result":{{"content":[{{"type":"text","text":"evicted"}}]}}}})",
                        id));
            }
            auto status = co_await self->status();
            if (!status) {
                co_return std::unexpected(status.error());
            }
            auto encoded = request.body.find("node_store_mounts") != std::string::npos
                               ? mounts_json(*status)
                               : status_json(*status);
            if (!encoded) {
                co_return std::unexpected(encoded.error());
            }
            co_return serve::make_response(
                200,
                "application/json",
                std::format(
                    R"({{"jsonrpc":"2.0","id":{},"result":{{"content":[{{"type":"text","text":"node store status"}}],"structuredContent":{}}}}})",
                    id,
                    *encoded));
        });
}

} // namespace

std::shared_ptr<const serve::route_table> service::control_routes() {
    const auto self = shared_from_this();
    auto routes = std::make_shared<serve::route_table>();
    add_liveness_routes(routes, self);
    add_status_routes(routes, self);
    add_admin_routes(routes, self);
    add_openapi_route(routes);
    add_mcp_route(routes, self);
    return routes;
}

} // namespace liquid::node_store_detail
