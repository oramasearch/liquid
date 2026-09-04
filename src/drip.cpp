#include "liquid/drip.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <ranges>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/ex/strand.hpp>

#include "capture/coalesce.hpp"
#include "capture/cursor.hpp"
#include "capture/filter.hpp"
#include "capture/reconcile.hpp"
#include "capture/watcher.hpp"
#include "crdt/document.hpp"
#include "crdt/journal.hpp"
#include "identity/taap.hpp"
#include "liquid/hash.hpp"
#include "liquid/store.hpp"
#include "objects/records.hpp"
#include "store/chunker.hpp"

namespace liquid {
namespace {

namespace capture_detail = liquid::capture_detail;

auto capture_error(errc code, std::string message, const std::filesystem::path& path) -> error {
    return {code, std::move(message), path.string()};
}

auto token(capture_detail::capture_cursor cursor) -> std::string {
    return std::to_string(cursor.journal.offset) + ":" + std::to_string(cursor.platform_token);
}

auto parse_token(std::string_view value) -> result<capture_detail::capture_cursor> {
    if (value.empty()) {
        return capture_detail::capture_cursor{};
    }
    const auto separator = value.find(':');
    if (separator == std::string_view::npos) {
        return std::unexpected(
            error{errc::invalid_argument, "invalid journal cursor", std::string{value}});
    }
    auto output = capture_detail::capture_cursor{};
    const auto offset = value.substr(0, separator);
    const auto platform = value.substr(separator + 1);
    const auto [offset_end, offset_error] =
        std::from_chars(offset.data(), offset.data() + offset.size(), output.journal.offset);
    const auto [platform_end, platform_error] =
        std::from_chars(platform.data(), platform.data() + platform.size(), output.platform_token);
    if (offset_error != std::errc{} || offset_end != offset.data() + offset.size() ||
        platform_error != std::errc{} || platform_end != platform.data() + platform.size()) {
        return std::unexpected(
            error{errc::invalid_argument, "invalid journal cursor", std::string{value}});
    }
    return output;
}

auto path_id(const std::filesystem::path& path) -> crdt::node_id {
    const auto text = path.generic_string();
    const auto bytes = std::span{reinterpret_cast<const std::byte*>(text.data()), text.size()};
    auto hashed = hash(algorithm::blake3, bytes);
    if (!hashed)
        throw std::runtime_error(hashed.error().message);
    auto output = crdt::node_id{};
    std::ranges::copy_n(hashed->bytes.begin(), output.size(), output.begin());
    return output;
}

auto read_consistent(const std::filesystem::path& path) -> std::vector<std::byte> {
    const auto before_size = std::filesystem::file_size(path);
    const auto before_time = std::filesystem::last_write_time(path);
    auto input = std::ifstream{path, std::ios::binary};
    if (!input)
        throw std::system_error(errno, std::generic_category(), "read captured file");
    auto output = std::vector<std::byte>{};
    for (char byte; input.get(byte);)
        output.push_back(std::byte{static_cast<unsigned char>(byte)});
    if (!input.eof())
        throw std::system_error(errno, std::generic_category(), "read captured file");
    if (std::filesystem::file_size(path) != before_size ||
        std::filesystem::last_write_time(path) != before_time)
        throw std::runtime_error("file changed while capture was reading it");
    return output;
}

auto as_digest(const crdt::head_set& heads) -> digest {
    auto value = digest{algorithm::blake3, {}};
    if (!heads.empty())
        value.bytes = heads.front();
    return value;
}

auto public_drip(const crdt::drip_record& record) -> drip {
    auto id = digest{algorithm::blake3, {}};
    std::ranges::copy(record.id, id.bytes.begin());
    auto output = drip{.id = {id},
                       .base = {as_digest(record.base)},
                       .ops = {},
                       .by = {"taap:local:capture"},
                       .signature = record.signature.value_or(std::vector<std::byte>{}),
                       .at = std::chrono::sys_seconds{std::chrono::seconds{record.at}},
                       .scan_attestation = record.scan_attestation,
                       .block = std::nullopt,
                       .redacted = record.redacted};
    output.ops.reserve(record.ops.size());
    for (const auto& operation : record.ops)
        output.ops.push_back({operation.path});
    return output;
}

} // namespace

struct subscription::impl {
    std::weak_ptr<capture::impl> capture;
    std::uint64_t id{0};

    ~impl();
};

struct capture::impl : std::enable_shared_from_this<capture::impl> {
    using executor_type = boost::capy::strand<boost::capy::any_executor>;

    struct subscriber {
        std::uint64_t id;
        capture::cursor_callback callback;
    };

    std::filesystem::path workspace;
    std::filesystem::path control;
    capture_options options;
    std::optional<crdt::journal> journal;
    std::optional<crdt::document> document;
    std::optional<store> objects;
    std::optional<signer> scanning_key;
    std::unique_ptr<capture_detail::watcher> watcher;
    std::shared_ptr<impl> shared;
    capture_detail::workspace_state workspace_state;
    capture_detail::capture_cursor cursor;
    std::map<std::string, std::vector<digest>> scanned_chunks;
    capture_detail::allow_all_filter filter;
    capture_detail::secret_filter scanner;
    executor_type executor;
    std::mutex mutex;
    std::mutex journal_mutex;
    boost::capy::async_event events_ready;
    std::vector<capture_detail::watch_event> events;
    std::vector<subscriber> subscribers;
    std::uint64_t next_subscription{1};
    std::uint64_t overflows{0};
    std::uint64_t recrawls{0};
    std::uint64_t blocked_drips{0};
    std::optional<scan_block> last_block;
    bool paused{false};
    bool closed{false};
    std::atomic<bool> stopped{false};
    std::atomic<bool> ticker_stopped{false};
    std::atomic<bool> wake_pending{false};
    std::atomic<std::uint64_t> event_count{0};
    std::atomic<std::uint64_t> last_append_ns{0};
    std::atomic<std::uint64_t> last_scan_ns{0};

    impl() : executor(runtime::default_context().executor()) {}

    static runtime::task<void> wake(std::shared_ptr<impl> state) {
        state->events_ready.set();
        co_return {};
    }

    void schedule_wake() {
        if (wake_pending.exchange(true, std::memory_order_acq_rel))
            return;
        boost::capy::run_async(executor)(wake(shared_from_this()));
    }

    void enqueue(capture_detail::watch_event event) {
        const auto relative = event.path.lexically_relative(workspace).empty()
                                  ? event.path
                                  : event.path.lexically_relative(workspace);
        if (relative.generic_string().starts_with(".liquid-capture-cookie-"))
            event.kind = capture_detail::watch_event_kind::cookie;
        else if (excluded(relative))
            return;
        const auto counts_as_hint = event.kind != capture_detail::watch_event_kind::cookie;
        {
            std::lock_guard lock(mutex);
            if (closed)
                return;
            events.push_back(std::move(event));
            if (counts_as_hint)
                event_count.fetch_add(1, std::memory_order_release);
        }
        schedule_wake();
    }

    void enqueue_periodic_reconcile() {
        {
            std::lock_guard lock(mutex);
            if (closed)
                return;
            events.push_back(
                {.kind = capture_detail::watch_event_kind::periodic_reconcile, .path = workspace});
        }
        events_ready.set();
    }

    [[nodiscard]] bool has_reconcile_hint() {
        std::lock_guard lock(mutex);
        return std::ranges::any_of(events, [](const auto& event) {
            return event.kind == capture_detail::watch_event_kind::reconcile ||
                   event.kind == capture_detail::watch_event_kind::periodic_reconcile;
        });
    }

    static runtime::task<void> ticker(std::shared_ptr<impl> state) {
        auto observed = state->event_count.load(std::memory_order_acquire);
        while (!state->ticker_stopped.load(std::memory_order_acquire)) {
            auto [delay_error] = co_await runtime::delay(state->options.reconcile_interval);
            if (delay_error)
                co_return std::unexpected(
                    error{errc::internal, delay_error.message(), "capture reconcile ticker"});
            if (state->ticker_stopped.load(std::memory_order_acquire))
                break;
            const auto current = state->event_count.load(std::memory_order_acquire);
            if (current == observed)
                state->enqueue_periodic_reconcile();
            else
                state->events_ready.set();
            observed = current;
        }
        co_return {};
    }

    [[nodiscard]] auto excluded(const std::filesystem::path& path) const -> bool {
        const auto text = path.generic_string();
        if (text.starts_with(".liquid-capture-cookie-") || text == ".liquid" ||
            text.starts_with(".liquid/"))
            return true;
        for (const auto& pattern : options.exclude) {
            auto regex = std::string{"^"};
            for (std::size_t index = 0; index < pattern.size(); ++index) {
                const auto character = pattern[index];
                if (character == '*') {
                    if (index + 1 < pattern.size() && pattern[index + 1] == '*') {
                        regex += ".*";
                        ++index;
                    } else {
                        regex += "[^/]*";
                    }
                } else if (character == '?') {
                    regex += "[^/]";
                } else if (std::string_view{".^$|()[]{}+\\"}.contains(character)) {
                    regex += '\\';
                    regex += character;
                } else {
                    regex += character;
                }
            }
            regex += '$';
            if (std::regex_match(text, std::regex{regex}))
                return true;
        }
        return false;
    }

    void notify(const drip& value, const journal_cursor& cursor_value) {
        auto callbacks = std::vector<capture::cursor_callback>{};
        {
            std::lock_guard lock(mutex);
            for (const auto& subscriber : subscribers)
                callbacks.push_back(subscriber.callback);
        }
        for (const auto& callback : callbacks) {
            try {
                callback(value, cursor_value);
            } catch (...) {
            }
        }
    }

    [[nodiscard]] runtime::task<bool>
    append(std::vector<capture_detail::watch_event> events_to_append) {
        auto accepted = filter.accept(events_to_append);
        if (!accepted)
            co_return std::unexpected(accepted.error());
        if (!*accepted || events_to_append.empty())
            co_return true;

        auto operations = std::vector<crdt::operation>{};
        auto detector_ids = std::vector<std::string>{};
        auto allowances = std::vector<scan_allowance>{};
        auto findings_count = std::uint64_t{};
        auto scan_duration = std::chrono::nanoseconds{};
        auto scanned_updates = std::map<std::string, std::vector<digest>>{};
        auto removed_paths = std::vector<std::string>{};
        for (const auto& event : events_to_append) {
            const auto relative = event.path.lexically_relative(workspace).empty()
                                      ? event.path
                                      : event.path.lexically_relative(workspace);
            if (excluded(relative))
                continue;
            if (event.kind == capture_detail::watch_event_kind::remove) {
                auto id = co_await runtime::offload([relative] { return path_id(relative); });
                if (!id)
                    co_return std::unexpected(id.error());
                operations.push_back({.id = *id,
                                      .kind = crdt::operation_kind::remove,
                                      .path = relative.generic_string()});
                removed_paths.push_back(relative.generic_string());
                continue;
            }
            auto absolute = workspace / relative;
            auto bytes = std::optional<std::vector<std::byte>>{};
            for (unsigned attempt = 0; attempt < 3 && !bytes; ++attempt) {
                auto read =
                    co_await runtime::offload([absolute] { return read_consistent(absolute); });
                if (read)
                    bytes = std::move(*read);
                else if (attempt != 2) {
                    auto [delay_error] = co_await runtime::delay(options.coalesce_window);
                    if (delay_error)
                        co_return std::unexpected(
                            error{errc::internal, delay_error.message(), "capture retry"});
                }
            }
            if (!bytes)
                continue;
            if (options.scan) {
                const auto scan_started = std::chrono::steady_clock::now();
                auto chunk_ids = std::vector<digest>{};
                auto changed = std::vector<scan_detail::byte_range>{};
                const auto ranges = store_detail::fastcdc(*bytes);
                const auto previous = scanned_chunks.find(relative.generic_string());
                for (std::size_t index = 0; index != ranges.size(); ++index) {
                    const auto& range = ranges[index];
                    auto id =
                        hash(algorithm::blake3,
                             std::span<const std::byte>{bytes->data() + range.offset, range.size});
                    if (!id)
                        co_return std::unexpected(id.error());
                    const auto differs = previous == scanned_chunks.end() ||
                                         index >= previous->second.size() ||
                                         to_string(previous->second[index]) != to_string(*id);
                    if (differs)
                        changed.push_back({range.offset, range.size});
                    chunk_ids.push_back(*id);
                }
                auto scanned =
                    scanner.inspect(relative.generic_string(), *bytes, std::span{changed});
                scan_duration += std::chrono::steady_clock::now() - scan_started;
                detector_ids = scanned.detectors;
                if (!scanned.blocked.empty()) {
                    const auto& finding = scanned.blocked.front();
                    auto block = scan_block{.code = errc::unauthorized,
                                            .path = relative.generic_string(),
                                            .detector = finding.detector,
                                            .line = finding.line};
                    {
                        std::lock_guard lock(mutex);
                        ++blocked_drips;
                        last_block = block;
                    }
                    notify(drip{.block = std::move(block)}, {token(cursor)});
                    last_scan_ns.store(scan_duration.count(), std::memory_order_release);
                    co_return false;
                }
                scanned_updates[relative.generic_string()] = std::move(chunk_ids);
                findings_count += scanned.allowed.size();
                for (const auto& allowed : scanned.allowed) {
                    allowances.push_back({relative.generic_string(),
                                          allowed.value.detector,
                                          allowed.value.line,
                                          allowed.reason});
                }
            }
            auto id = co_await runtime::offload([relative] { return path_id(relative); });
            if (!id)
                co_return std::unexpected(id.error());
            auto stored = co_await objects->put(std::move(*bytes));
            if (!stored)
                co_return std::unexpected(stored.error());
            operations.push_back({.id = *id,
                                  .kind = crdt::operation_kind::write,
                                  .path = relative.generic_string(),
                                  .ref = to_string(*stored),
                                  .mode = 0100644,
                                  .entry_id = *id});
        }
        if (operations.empty())
            co_return true;
        last_scan_ns.store(scan_duration.count(), std::memory_order_release);
        auto base = document->heads();
        if (!base)
            co_return std::unexpected(base.error());
        const auto at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        const auto time =
            std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count();
        auto applied =
            document->transact(std::move(operations), crdt::commit_options{.time = time});
        if (!applied)
            co_return std::unexpected(applied.error());
        auto record = crdt::drip_record{.id = applied->front().id,
                                        .base = *base,
                                        .ops = *applied,
                                        .by = document->actor(),
                                        .at = at.time_since_epoch().count()};
        if (options.scan) {
            auto subject = digest{algorithm::blake3, {}};
            std::ranges::copy(record.id, subject.bytes.begin());
            auto scan_record = attestation{.subject = subject,
                                           .profile = "scan_result/1",
                                           .detectors = std::move(detector_ids),
                                           .findings_count = findings_count,
                                           .at = at,
                                           .issuer = scanning_key->identity(),
                                           .allowances = std::move(allowances)};
            auto body = objects::encode_attestation_body(scan_record);
            if (!body)
                co_return std::unexpected(body.error());
            auto signed_value = co_await scanning_key->sign("attestation", *body);
            if (!signed_value)
                co_return std::unexpected(signed_value.error());
            auto signature = identity_detail::canonical_signature(*signed_value);
            if (!signature)
                co_return std::unexpected(signature.error());
            scan_record.signature = std::move(*signature);
            auto encoded = objects::encode(scan_record);
            if (!encoded)
                co_return std::unexpected(encoded.error());
            auto stored = co_await objects->put(std::move(*encoded));
            if (!stored)
                co_return std::unexpected(stored.error());
            record.scan_attestation = *stored;
        }
        {
            std::lock_guard lock(journal_mutex);
            const auto append_started = std::chrono::steady_clock::now();
            auto appended = journal->append(record);
            last_append_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - append_started)
                                     .count(),
                                 std::memory_order_release);
            if (!appended)
                co_return std::unexpected(appended.error());
            auto end = journal->end();
            if (!end)
                co_return std::unexpected(end.error());
            cursor.journal = *end;
        }
        auto persisted = co_await runtime::offload([path = control / "cursor", value = cursor] {
            return capture_detail::save_cursor(path, value);
        });
        if (!persisted)
            co_return std::unexpected(persisted.error());
        if (!*persisted)
            co_return std::unexpected(persisted->error());
        notify(public_drip(record), {token(cursor)});
        for (auto& [path, chunks] : scanned_updates)
            scanned_chunks[path] = std::move(chunks);
        for (const auto& path : removed_paths)
            scanned_chunks.erase(path);
        co_return true;
    }

    [[nodiscard]] runtime::task<void> recrawl(bool report) {
        auto scanned = co_await runtime::offload([this] {
            return capture_detail::reconcile(
                workspace, workspace_state, [this](const std::filesystem::path& path) {
                    return excluded(path);
                });
        });
        if (!scanned)
            co_return std::unexpected(scanned.error());
        if (!*scanned)
            co_return std::unexpected(scanned->error());
        auto next_state = std::move((**scanned).state);
        if (report) {
            std::lock_guard lock(mutex);
            ++recrawls;
        }
        auto appended = co_await append(std::move((**scanned).changes));
        if (!appended)
            co_return std::unexpected(appended.error());
        workspace_state = std::move(next_state);
        auto saved = co_await runtime::offload([path = control / "state", state = workspace_state] {
            return capture_detail::save_workspace_state(path, state);
        });
        if (!saved)
            co_return std::unexpected(saved.error());
        if (!*saved)
            co_return std::unexpected(saved->error());
        if (!*appended)
            co_return result<void>{};
        co_return {};
    }
};

subscription::impl::~impl() {
    if (auto owner = capture.lock()) {
        std::lock_guard lock(owner->mutex);
        std::erase_if(owner->subscribers,
                      [id = id](const capture::impl::subscriber& entry) { return entry.id == id; });
    }
}

subscription::subscription() noexcept = default;
subscription::~subscription() = default;
subscription::subscription(subscription&&) noexcept = default;
subscription& subscription::operator=(subscription&&) noexcept = default;

capture::capture(std::unique_ptr<impl> implementation) noexcept
    : impl_(std::move(implementation)) {}
capture::~capture() = default;
capture::capture(capture&&) noexcept = default;
capture& capture::operator=(capture&&) noexcept = default;

runtime::task<capture> capture::open(std::filesystem::path workspace, capture_options options) {
    try {
        workspace =
            std::filesystem::weakly_canonical(std::filesystem::absolute(std::move(workspace)));
        if (!std::filesystem::is_directory(workspace))
            co_return std::unexpected(
                capture_error(errc::not_found, "workspace is not a directory", workspace));
        auto state = std::make_shared<impl>();
        state->workspace = std::move(workspace);
        state->control = state->workspace / ".liquid" / "capture";
        state->options = std::move(options);
        state->scanner = capture_detail::secret_filter{state->options.scan_config};
        if (state->options.coalesce_window.count() < 0 ||
            state->options.reconcile_interval.count() < 0)
            co_return std::unexpected(capture_error(
                errc::invalid_argument, "capture window is negative", state->workspace));
        auto ready = co_await runtime::offload(
            [path = state->control] { std::filesystem::create_directories(path); });
        if (!ready)
            co_return std::unexpected(ready.error());
        state->journal.emplace(state->control / "drips.cbor");
        auto previous = co_await runtime::offload([path = state->control / "state"] {
            return capture_detail::load_workspace_state(path);
        });
        if (!previous)
            co_return std::unexpected(previous.error());
        if (!*previous)
            co_return std::unexpected(previous->error());
        state->workspace_state = std::move(**previous);
        auto saved_cursor = co_await runtime::offload(
            [path = state->control / "cursor"] { return capture_detail::load_cursor(path); });
        if (!saved_cursor)
            co_return std::unexpected(saved_cursor.error());
        if (!*saved_cursor)
            co_return std::unexpected(saved_cursor->error());
        state->cursor = std::move(**saved_cursor);
        auto repository = crdt::repo_id{};
        const auto root_text = state->workspace.generic_string();
        auto root_hash = co_await runtime::offload([root_text] {
            auto value = hash(
                algorithm::blake3,
                std::span{reinterpret_cast<const std::byte*>(root_text.data()), root_text.size()});
            if (!value)
                throw std::runtime_error(value.error().message);
            return *value;
        });
        if (!root_hash)
            co_return std::unexpected(root_hash.error());
        std::ranges::copy_n(root_hash->bytes.begin(), repository.size(), repository.begin());
        const auto cut = state->control / "drips.cbor.cut";
        if (std::filesystem::exists(cut)) {
            auto replayed = state->journal->replay("taap:local:capture", "capture");
            if (!replayed)
                co_return std::unexpected(replayed.error());
            state->document.emplace(std::move(*replayed));
        } else {
            auto created = crdt::document::create(repository, "taap:local:capture", "capture");
            if (!created)
                co_return std::unexpected(created.error());
            state->document.emplace(std::move(*created));
            auto cut_result = state->journal->cut(*state->document, 1);
            if (!cut_result)
                co_return std::unexpected(cut_result.error());
        }
        state->watcher =
            capture_detail::make_watcher(state->workspace, state->options.coalesce_window);
        auto started =
            state->watcher->start([weak = std::weak_ptr{state}](capture_detail::watch_event event) {
                if (auto locked = weak.lock())
                    locked->enqueue(std::move(event));
            });
        if (!started)
            co_return std::unexpected(started.error());
        auto armed = false;
        for (auto attempt = 0U; attempt < 50 && !armed; ++attempt) {
            const auto cookie =
                state->workspace / (".liquid-capture-cookie-" + std::to_string(attempt));
            auto written = co_await runtime::offload([cookie] {
                auto output = std::ofstream{cookie, std::ios::trunc};
                if (!output)
                    throw std::system_error(errno, std::generic_category(), "write capture cookie");
                output << "armed\n";
                output.flush();
                if (!output)
                    throw std::system_error(errno, std::generic_category(), "flush capture cookie");
            });
            if (!written)
                co_return std::unexpected(written.error());
            for (auto elapsed = std::chrono::milliseconds{};
                 elapsed < std::chrono::milliseconds{100};
                 elapsed += std::chrono::milliseconds{1}) {
                {
                    std::lock_guard lock(state->mutex);
                    armed = std::ranges::any_of(state->events, [&cookie](const auto& event) {
                        return event.kind == capture_detail::watch_event_kind::cookie &&
                               event.path.filename() == cookie.filename();
                    });
                }
                if (armed)
                    break;
                auto [wait_error] = co_await runtime::delay(std::chrono::milliseconds{1});
                if (wait_error)
                    co_return std::unexpected(
                        error{errc::internal, wait_error.message(), "capture watcher arming"});
            }
            auto removed = co_await runtime::offload([cookie] { std::filesystem::remove(cookie); });
            if (!removed)
                co_return std::unexpected(removed.error());
        }
        if (!armed)
            co_return std::unexpected(
                capture_error(errc::unavailable,
                              std::string{state->watcher->name()} + " did not arm for workspace",
                              state->workspace));
        auto output = capture{std::make_unique<impl>()};
        output.impl_->shared = std::move(state);
        boost::capy::run_async(output.impl_->shared->executor,
                               [state = output.impl_->shared](result<void>) {
                                   state->stopped.store(true, std::memory_order_release);
                               })([](std::shared_ptr<impl> state) -> runtime::task<void> {
            auto opened_store = co_await store::open(state->control / "store");
            if (!opened_store)
                co_return std::unexpected(opened_store.error());
            state->objects.emplace(std::move(*opened_store));
            auto scanning_key = co_await identity_detail::local_signer();
            if (!scanning_key)
                co_return std::unexpected(scanning_key.error());
            state->scanning_key.emplace(std::move(*scanning_key));
            auto [initial_delay_error] = co_await runtime::delay(state->options.coalesce_window);
            if (initial_delay_error)
                co_return std::unexpected(
                    error{errc::internal, initial_delay_error.message(), "capture startup"});
            auto initial = co_await state->recrawl(true);
            if (!initial)
                co_return std::unexpected(initial.error());
            while (true) {
                auto [event_error] = co_await state->events_ready.wait();
                if (event_error)
                    co_return std::unexpected(
                        error{errc::internal, event_error.message(), "capture queue"});
                if (!state->has_reconcile_hint()) {
                    auto [delay_error] = co_await runtime::delay(state->options.coalesce_window);
                    if (delay_error)
                        co_return std::unexpected(
                            error{errc::internal, delay_error.message(), "capture queue"});
                }
                {
                    std::lock_guard lock(state->mutex);
                    if (state->closed)
                        break;
                    if (state->paused) {
                        state->events_ready.clear();
                        state->wake_pending.store(false, std::memory_order_release);
                        continue;
                    }
                }
                auto batch = std::vector<capture_detail::watch_event>{};
                {
                    std::lock_guard lock(state->mutex);
                    batch.swap(state->events);
                }
                state->events_ready.clear();
                state->wake_pending.store(false, std::memory_order_release);
                {
                    std::lock_guard lock(state->mutex);
                    batch.insert(batch.end(),
                                 std::make_move_iterator(state->events.begin()),
                                 std::make_move_iterator(state->events.end()));
                    state->events.clear();
                }
                auto coalesced = capture_detail::coalescer{};
                auto needs_recrawl = false;
                for (auto& event : batch) {
                    if (event.kind == capture_detail::watch_event_kind::cookie)
                        continue;
                    if (event.kind == capture_detail::watch_event_kind::reconcile) {
                        std::lock_guard lock(state->mutex);
                        ++state->overflows;
                        needs_recrawl = true;
                        continue;
                    }
                    if (event.kind == capture_detail::watch_event_kind::periodic_reconcile) {
                        needs_recrawl = true;
                        continue;
                    }
                    const auto relative = event.path.lexically_relative(state->workspace).empty()
                                              ? event.path
                                              : event.path.lexically_relative(state->workspace);
                    if (state->excluded(relative))
                        continue;
                    coalesced.push(std::move(event));
                }
                const auto hints = coalesced.take();
                if (!needs_recrawl && hints.empty())
                    continue;
                auto result = co_await state->recrawl(needs_recrawl);
                if (!result)
                    co_return std::unexpected(result.error());
            }
            co_return {};
        }(output.impl_->shared));
        if (output.impl_->shared->options.reconcile_interval.count() > 0) {
            boost::capy::run_async(output.impl_->shared->executor,
                                   [](result<void>) {})(impl::ticker(output.impl_->shared));
        }
        co_return output;
    } catch (const std::exception& caught) {
        co_return std::unexpected(error{errc::internal, caught.what(), "capture::open"});
    }
}

result<subscription> capture::subscribe(std::function<void(const drip&)> callback) {
    return subscribe_from(
        {}, [callback = std::move(callback)](const drip& value, const journal_cursor&) {
            callback(value);
        });
}

result<subscription> capture::subscribe_from(journal_cursor cursor_value,
                                             cursor_callback callback) {
    if (!impl_ || !impl_->shared)
        return std::unexpected(
            error{errc::invalid_argument, "capture is not open", "capture::subscribe_from"});
    auto start = parse_token(cursor_value.token);
    if (!start) {
        return std::unexpected(start.error());
    }
    auto state = impl_->shared;
    auto id = std::uint64_t{};
    {
        std::lock_guard lock(state->journal_mutex);
        auto next = start->journal;
        while (true) {
            auto replay = state->journal->read(next, 1);
            if (!replay) {
                return std::unexpected(replay.error());
            }
            if (replay->first.empty()) {
                break;
            }
            next = replay->second;
            try {
                callback(public_drip(replay->first.front()),
                         {token({next, state->cursor.platform_token})});
            } catch (...) {
            }
        }
        {
            std::lock_guard subscriber_lock(state->mutex);
            id = state->next_subscription++;
            state->subscribers.push_back({id, std::move(callback)});
        }
    }
    subscription output;
    output.impl_ = std::make_unique<subscription::impl>();
    output.impl_->capture = state;
    output.impl_->id = id;
    return output;
}

runtime::task<void> capture::close() {
    if (!impl_ || !impl_->shared)
        co_return {};
    auto state = std::move(impl_->shared);
    {
        std::lock_guard lock(state->mutex);
        state->closed = true;
    }
    state->ticker_stopped.store(true, std::memory_order_release);
    state->watcher->stop();
    state->schedule_wake();
    while (!state->stopped.load(std::memory_order_acquire) || !state->watcher->stopped()) {
        auto [wait_error] = co_await runtime::delay(std::chrono::milliseconds{1});
        if (wait_error)
            co_return std::unexpected(
                error{errc::internal, wait_error.message(), "capture::close"});
    }
    state->watcher->stop();
    co_return {};
}

runtime::task<void> capture::pause() {
    if (!impl_ || !impl_->shared)
        co_return std::unexpected(
            error{errc::invalid_argument, "capture is not open", "capture::pause"});
    std::lock_guard lock(impl_->shared->mutex);
    impl_->shared->paused = true;
    co_return {};
}

runtime::task<void> capture::resume() {
    if (!impl_ || !impl_->shared)
        co_return std::unexpected(
            error{errc::invalid_argument, "capture is not open", "capture::resume"});
    {
        std::lock_guard lock(impl_->shared->mutex);
        impl_->shared->paused = false;
    }
    impl_->shared->schedule_wake();
    co_return {};
}

result<capture_status> capture::status() {
    if (!impl_ || !impl_->shared)
        return std::unexpected(
            error{errc::invalid_argument, "capture is not open", "capture::status"});
    std::lock_guard lock(impl_->shared->mutex);
    return capture_status{.state = impl_->shared->paused ? capture_status::state::paused
                                                         : capture_status::state::running,
                          .pending_drips = impl_->shared->events.size(),
                          .overflow_count = impl_->shared->overflows,
                          .recrawl_count = impl_->shared->recrawls,
                          .blocked_drips = impl_->shared->blocked_drips,
                          .last_block = impl_->shared->last_block,
                          .last_cursor = {token(impl_->shared->cursor)}};
}

void capture_detail::inject_overflow_for_test(capture& handle) {
    if (handle.impl_ && handle.impl_->shared && handle.impl_->shared->watcher)
        handle.impl_->shared->watcher->inject_overflow_for_test();
}

auto capture_detail::last_journal_append_duration_for_test(const capture& handle)
    -> std::chrono::nanoseconds {
    if (!handle.impl_ || !handle.impl_->shared)
        return {};
    return std::chrono::nanoseconds{
        handle.impl_->shared->last_append_ns.load(std::memory_order_acquire)};
}

auto capture_detail::last_scan_duration_for_test(const capture& handle)
    -> std::chrono::nanoseconds {
    if (!handle.impl_ || !handle.impl_->shared)
        return {};
    return std::chrono::nanoseconds{
        handle.impl_->shared->last_scan_ns.load(std::memory_order_acquire)};
}

} // namespace liquid
