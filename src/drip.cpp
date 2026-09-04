#include "liquid/drip.hpp"

#include <algorithm>
#include <atomic>
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
#include "liquid/hash.hpp"
#include "liquid/store.hpp"

namespace liquid {
namespace {

namespace capture_detail = liquid::capture_detail;

auto capture_error(errc code, std::string message, const std::filesystem::path& path) -> error {
    return {code, std::move(message), path.string()};
}

auto token(capture_detail::capture_cursor cursor) -> std::string {
    return std::to_string(cursor.journal.offset) + ":" + std::to_string(cursor.platform_token);
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
                       .at = std::chrono::sys_seconds{std::chrono::seconds{record.at}}};
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
        std::function<void(const drip&)> callback;
    };

    std::filesystem::path workspace;
    std::filesystem::path control;
    capture_options options;
    std::optional<crdt::journal> journal;
    std::optional<crdt::document> document;
    std::optional<store> objects;
    std::unique_ptr<capture_detail::watcher> watcher;
    std::shared_ptr<impl> shared;
    capture_detail::workspace_state workspace_state;
    capture_detail::capture_cursor cursor;
    capture_detail::allow_all_filter filter;
    executor_type executor;
    std::mutex mutex;
    std::mutex journal_mutex;
    boost::capy::async_event events_ready;
    std::vector<capture_detail::watch_event> events;
    std::vector<subscriber> subscribers;
    std::uint64_t next_subscription{1};
    std::uint64_t overflows{0};
    std::uint64_t recrawls{0};
    bool paused{false};
    bool closed{false};
    bool cookie_seen{false};
    std::atomic<bool> stopped{false};
    std::atomic<bool> wake_pending{false};
    std::atomic<std::uint64_t> last_append_ns{0};

    impl() : executor(runtime::default_context().executor()) {}

    static runtime::task<void> wake(std::shared_ptr<impl> state) {
        {
            std::lock_guard lock(state->mutex);
            state->cookie_seen =
                state->cookie_seen || std::ranges::any_of(state->events, [](const auto& event) {
                    return event.kind == capture_detail::watch_event_kind::cookie;
                });
        }
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
        {
            std::lock_guard lock(mutex);
            if (closed)
                return;
            events.push_back(std::move(event));
        }
        schedule_wake();
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

    void notify(const drip& value) {
        auto callbacks = std::vector<std::function<void(const drip&)>>{};
        {
            std::lock_guard lock(mutex);
            for (const auto& subscriber : subscribers)
                callbacks.push_back(subscriber.callback);
        }
        for (const auto& callback : callbacks) {
            try {
                callback(value);
            } catch (...) {
            }
        }
    }

    [[nodiscard]] runtime::task<void>
    append(std::vector<capture_detail::watch_event> events_to_append) {
        auto accepted = filter.accept(events_to_append);
        if (!accepted)
            co_return std::unexpected(accepted.error());
        if (!*accepted || events_to_append.empty())
            co_return {};

        auto operations = std::vector<crdt::operation>{};
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
            co_return {};
        auto base = document->heads();
        if (!base)
            co_return std::unexpected(base.error());
        auto applied = document->transact(std::move(operations));
        if (!applied)
            co_return std::unexpected(applied.error());
        auto record =
            crdt::drip_record{.id = applied->front().id,
                              .base = *base,
                              .ops = *applied,
                              .by = document->actor(),
                              .at = std::chrono::duration_cast<std::chrono::seconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count()};
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
        notify(public_drip(record));
        co_return {};
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
        if (report) {
            std::lock_guard lock(mutex);
            ++recrawls;
        }
        auto appended = co_await append(std::move((**scanned).changes));
        if (!appended)
            co_return std::unexpected(appended.error());
        workspace_state = std::move((**scanned).state);
        auto saved = co_await runtime::offload([path = control / "state", state = workspace_state] {
            return capture_detail::save_workspace_state(path, state);
        });
        if (!saved)
            co_return std::unexpected(saved.error());
        if (!*saved)
            co_return std::unexpected(saved->error());
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
        if (state->options.coalesce_window.count() < 0)
            co_return std::unexpected(capture_error(
                errc::invalid_argument, "coalescing window is negative", state->workspace));
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
        auto opened_store = co_await store::open(state->control / "store");
        if (!opened_store)
            co_return std::unexpected(opened_store.error());
        state->objects.emplace(std::move(*opened_store));
        auto initial = co_await state->recrawl(true);
        if (!initial)
            co_return std::unexpected(initial.error());
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
            {
                std::lock_guard lock(state->mutex);
                state->cookie_seen = false;
            }
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
                    armed = state->cookie_seen;
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
            while (true) {
                auto [event_error] = co_await state->events_ready.wait();
                if (event_error)
                    co_return std::unexpected(
                        error{errc::internal, event_error.message(), "capture queue"});
                auto [delay_error] = co_await runtime::delay(state->options.coalesce_window);
                if (delay_error)
                    co_return std::unexpected(
                        error{errc::internal, delay_error.message(), "capture queue"});
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
                {
                    std::lock_guard lock(state->mutex);
                    batch.insert(batch.end(),
                                 std::make_move_iterator(state->events.begin()),
                                 std::make_move_iterator(state->events.end()));
                    state->events.clear();
                    state->wake_pending.store(false, std::memory_order_release);
                }
                auto coalesced = capture_detail::coalescer{};
                auto needs_recrawl = false;
                for (auto& event : batch) {
                    if (event.kind == capture_detail::watch_event_kind::cookie)
                        continue;
                    const auto relative = event.path.lexically_relative(state->workspace).empty()
                                              ? event.path
                                              : event.path.lexically_relative(state->workspace);
                    if (state->excluded(relative))
                        continue;
                    if (event.kind == capture_detail::watch_event_kind::reconcile) {
                        std::lock_guard lock(state->mutex);
                        ++state->overflows;
                        needs_recrawl = true;
                    } else {
                        coalesced.push(std::move(event));
                    }
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
        co_return output;
    } catch (const std::exception& caught) {
        co_return std::unexpected(error{errc::internal, caught.what(), "capture::open"});
    }
}

result<subscription> capture::subscribe(std::function<void(const drip&)> callback) {
    if (!impl_ || !impl_->shared)
        return std::unexpected(
            error{errc::invalid_argument, "capture is not open", "capture::subscribe"});
    auto state = impl_->shared;
    const auto id = [&] {
        std::lock_guard lock(state->mutex);
        const auto next = state->next_subscription++;
        state->subscribers.push_back({next, callback});
        return next;
    }();
    std::vector<crdt::drip_record> records;
    {
        std::lock_guard lock(state->journal_mutex);
        auto replay = state->journal->read({}, static_cast<std::size_t>(-1));
        if (!replay)
            return std::unexpected(replay.error());
        records = std::move(replay->first);
    }
    for (const auto& record : records) {
        try {
            callback(public_drip(record));
        } catch (...) {
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
    state->watcher->stop();
    state->schedule_wake();
    while (!state->stopped.load(std::memory_order_acquire)) {
        auto [wait_error] = co_await runtime::delay(std::chrono::milliseconds{1});
        if (wait_error)
            co_return std::unexpected(
                error{errc::internal, wait_error.message(), "capture::close"});
    }
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

} // namespace liquid
