#include "build/events.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <system_error>
#include <utility>
#include <vector>

#include <boost/capy/concept/executor.hpp>
#include <boost/capy/continuation.hpp>
#include <boost/capy/ex/io_env.hpp>

#include "build/wrap.hpp"
#include "liquid/repository.hpp"
#include "objects/records.hpp"

namespace liquid {
namespace build_detail {

struct channel_waiter {
    boost::capy::continuation continuation;
    boost::capy::executor_ref executor;
    std::atomic<bool> claimed{};

    void wake() {
        if (!claimed.exchange(true, std::memory_order_acq_rel)) {
            executor.post(continuation);
        }
    }
};

struct subscriber_state {
    std::mutex mutex;
    std::deque<build_event> events;
    std::shared_ptr<channel_waiter> waiter;
    bool closed{};
};

struct event_state {
    std::mutex mutex;
    std::vector<std::weak_ptr<subscriber_state>> subscribers;
    std::vector<std::shared_ptr<channel_waiter>> completion_waiters;
    std::optional<result<build_summary>> completion;
    std::atomic<bool> observed{};
    std::uint64_t next_sequence{};
    bool finished{};
};

class subscriber_awaiter {
public:
    explicit subscriber_awaiter(std::shared_ptr<subscriber_state> state)
        : state_(std::move(state)), waiter_(std::make_shared<channel_waiter>()) {}

    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }

    auto await_suspend(std::coroutine_handle<> handle, const boost::capy::io_env* environment)
        -> std::coroutine_handle<> {
        waiter_->continuation.h = handle;
        waiter_->executor = environment->executor;
        auto lock = std::lock_guard{state_->mutex};
        if (!state_->events.empty() || state_->closed) {
            waiter_->claimed.store(true, std::memory_order_release);
            return handle;
        }
        state_->waiter = waiter_;
        return std::noop_coroutine();
    }

    void await_resume() const noexcept {}

private:
    std::shared_ptr<subscriber_state> state_;
    std::shared_ptr<channel_waiter> waiter_;
};

class completion_awaiter {
public:
    explicit completion_awaiter(std::shared_ptr<event_state> state)
        : state_(std::move(state)), waiter_(std::make_shared<channel_waiter>()) {}

    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }

    auto await_suspend(std::coroutine_handle<> handle, const boost::capy::io_env* environment)
        -> std::coroutine_handle<> {
        waiter_->continuation.h = handle;
        waiter_->executor = environment->executor;
        auto lock = std::lock_guard{state_->mutex};
        if (state_->completion) {
            waiter_->claimed.store(true, std::memory_order_release);
            return handle;
        }
        state_->completion_waiters.push_back(waiter_);
        return std::noop_coroutine();
    }

    void await_resume() const noexcept {}

private:
    std::shared_ptr<event_state> state_;
    std::shared_ptr<channel_waiter> waiter_;
};

auto terminal_diagnostic(error value) -> frontend_diagnostic {
    return {.severity = frontend_diagnostic_severity::error,
            .message = value.message + (value.context.empty() ? "" : ": " + value.context)};
}

void event_producer::publish(build_event event) const {
    if (!state_) {
        return;
    }
    auto subscribers = std::vector<std::shared_ptr<subscriber_state>>{};
    {
        auto lock = std::lock_guard{state_->mutex};
        if (state_->finished) {
            return;
        }
        event.sequence = ++state_->next_sequence;
        auto retained = std::vector<std::weak_ptr<subscriber_state>>{};
        retained.reserve(state_->subscribers.size());
        for (const auto& weak : state_->subscribers) {
            if (auto subscriber = weak.lock()) {
                retained.push_back(subscriber);
                subscribers.push_back(std::move(subscriber));
            }
        }
        state_->subscribers = std::move(retained);
    }
    auto waiters = std::vector<std::shared_ptr<channel_waiter>>{};
    for (const auto& subscriber : subscribers) {
        auto lock = std::lock_guard{subscriber->mutex};
        subscriber->events.push_back(event);
        if (subscriber->waiter) {
            waiters.push_back(std::exchange(subscriber->waiter, {}));
        }
    }
    for (const auto& waiter : waiters) {
        waiter->wake();
    }
}

void event_producer::finish(build_summary summary) const {
    if (!state_) {
        return;
    }
    publish({.kind = build_event_kind::build_finished, .summary = summary});
    auto subscribers = std::vector<std::shared_ptr<subscriber_state>>{};
    auto waiters = std::vector<std::shared_ptr<channel_waiter>>{};
    {
        auto lock = std::lock_guard{state_->mutex};
        if (state_->finished) {
            return;
        }
        state_->finished = true;
        state_->completion = summary;
        waiters = std::move(state_->completion_waiters);
        for (const auto& weak : state_->subscribers) {
            if (auto subscriber = weak.lock()) {
                subscribers.push_back(std::move(subscriber));
            }
        }
    }
    for (const auto& subscriber : subscribers) {
        auto lock = std::lock_guard{subscriber->mutex};
        subscriber->closed = true;
        if (subscriber->waiter) {
            waiters.push_back(std::exchange(subscriber->waiter, {}));
        }
    }
    for (const auto& waiter : waiters) {
        waiter->wake();
    }
}

void event_producer::fail(error failure, build_summary summary) const {
    summary.exit = summary.exit == 0 ? 1 : summary.exit;
    summary.failures = std::max(std::uint64_t{1}, summary.failures);
    publish({.kind = build_event_kind::action_failed,
             .diagnostics = {terminal_diagnostic(std::move(failure))}});
    finish(summary);
}

runtime::task<void> event_producer::await_observer() const {
    if (!state_) {
        co_return std::unexpected(
            error{errc::invalid_argument, "build event producer is empty", {}});
    }
    constexpr auto attempts = std::uint16_t{1000};
    for (auto attempt = std::uint16_t{};
         attempt != attempts && !state_->observed.load(std::memory_order_acquire);
         ++attempt) {
        auto [delay_error] = co_await runtime::delay(std::chrono::milliseconds{1});
        if (delay_error) {
            co_return std::unexpected(
                error{errc::internal, delay_error.message(), "build event start"});
        }
    }
    co_return {};
}

} // namespace build_detail

struct build_event_subscription::impl {
    std::shared_ptr<build_detail::subscriber_state> state;
};

struct build_handle::impl {
    std::shared_ptr<build_detail::event_state> state;
};

build_detail::started_event_stream build_detail::event_access::create() {
    auto state = std::make_shared<event_state>();
    auto implementation = std::make_unique<build_handle::impl>();
    implementation->state = state;
    return {build_handle{std::move(implementation)}, event_producer{std::move(state)}};
}

build_event_subscription::build_event_subscription() noexcept = default;
build_event_subscription::~build_event_subscription() = default;
build_event_subscription::build_event_subscription(build_event_subscription&&) noexcept = default;
auto build_event_subscription::operator=(build_event_subscription&&) noexcept
    -> build_event_subscription& = default;
build_event_subscription::build_event_subscription(std::unique_ptr<impl> implementation) noexcept
    : impl_(std::move(implementation)) {}

runtime::task<std::optional<build_event>> build_event_subscription::next() {
    if (!impl_ || !impl_->state) {
        co_return std::unexpected(
            error{errc::invalid_argument, "build event subscription is empty", {}});
    }
    while (true) {
        {
            auto lock = std::lock_guard{impl_->state->mutex};
            if (!impl_->state->events.empty()) {
                auto event = std::move(impl_->state->events.front());
                impl_->state->events.pop_front();
                co_return std::optional<build_event>{std::move(event)};
            }
            if (impl_->state->closed) {
                co_return std::optional<build_event>{};
            }
        }
        co_await build_detail::subscriber_awaiter{impl_->state};
    }
}

build_handle::build_handle() noexcept = default;
build_handle::~build_handle() = default;
build_handle::build_handle(build_handle&&) noexcept = default;
auto build_handle::operator=(build_handle&&) noexcept -> build_handle& = default;
build_handle::build_handle(std::unique_ptr<impl> implementation) noexcept
    : impl_(std::move(implementation)) {}

result<build_event_subscription> build_handle::events() const {
    if (!impl_ || !impl_->state) {
        return std::unexpected(error{errc::invalid_argument, "build handle is empty", {}});
    }
    auto state = std::make_shared<build_detail::subscriber_state>();
    {
        auto lock = std::lock_guard{impl_->state->mutex};
        state->closed = impl_->state->finished;
        if (!state->closed) {
            impl_->state->subscribers.push_back(state);
        }
        impl_->state->observed.store(true, std::memory_order_release);
    }
    auto implementation = std::make_unique<build_event_subscription::impl>();
    implementation->state = std::move(state);
    return build_event_subscription{std::move(implementation)};
}

runtime::task<build_summary> build_handle::wait() {
    if (!impl_ || !impl_->state) {
        co_return std::unexpected(error{errc::invalid_argument, "build handle is empty", {}});
    }
    impl_->state->observed.store(true, std::memory_order_release);
    while (true) {
        {
            auto lock = std::lock_guard{impl_->state->mutex};
            if (impl_->state->completion) {
                co_return *impl_->state->completion;
            }
        }
        co_await build_detail::completion_awaiter{impl_->state};
    }
}

namespace {

auto json_string(std::string_view value) -> std::string {
    auto output = std::string{"\""};
    for (const auto character : value) {
        switch (character) {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20U) {
                output += std::format("\\u{:04x}", static_cast<unsigned char>(character));
            } else {
                output.push_back(character);
            }
        }
    }
    output.push_back('"');
    return output;
}

auto event_kind_name(build_event_kind kind) -> std::string_view {
    switch (kind) {
    case build_event_kind::action_queued:
        return "action_queued";
    case build_event_kind::action_started:
        return "action_started";
    case build_event_kind::action_hit:
        return "action_hit";
    case build_event_kind::action_done:
        return "action_done";
    case build_event_kind::action_failed:
        return "action_failed";
    case build_event_kind::critical_path_updated:
        return "critical_path_updated";
    case build_event_kind::test_result:
        return "test_result";
    case build_event_kind::build_finished:
        return "build_finished";
    }
    return "action_failed";
}

auto severity_name(frontend_diagnostic_severity severity) -> std::string_view {
    switch (severity) {
    case frontend_diagnostic_severity::note:
        return "note";
    case frontend_diagnostic_severity::warning:
        return "warning";
    case frontend_diagnostic_severity::error:
        return "error";
    case frontend_diagnostic_severity::fatal:
        return "fatal";
    }
    return "error";
}

auto optional_digest(const std::optional<digest>& value) -> std::string {
    return value ? json_string(to_string(*value)) : "null";
}

auto optional_result(const std::optional<result_id>& value) -> std::string {
    return value ? json_string(to_string(value->value)) : "null";
}

} // namespace

result<std::string> event_json(const build_event& event) {
    auto output = std::format("{{\"v\":1,\"sequence\":{},\"type\":{},\"action\":{},\"result\":{}",
                              event.sequence,
                              json_string(event_kind_name(event.kind)),
                              optional_digest(event.action),
                              optional_result(event.result));
    if (event.execution) {
        output += std::format(
            ",\"execution\":{{\"kind\":{},\"worker\":{}}}",
            json_string(event.execution->kind == action_execution_kind::local ? "local" : "remote"),
            event.execution->worker.empty() ? "null" : json_string(event.execution->worker));
    }
    if (!event.outputs.empty() || event.kind == build_event_kind::action_done) {
        output += ",\"outputs\":[";
        for (auto index = std::size_t{}; index != event.outputs.size(); ++index) {
            if (index != 0) {
                output.push_back(',');
            }
            const auto& value = event.outputs[index];
            output += std::format("{{\"path\":{},\"mode\":{},\"blob\":{}}}",
                                  json_string(value.path),
                                  value.mode,
                                  json_string(to_string(value.blob)));
        }
        output.push_back(']');
    }
    if (!event.diagnostics.empty() || event.kind == build_event_kind::action_failed) {
        output += ",\"diagnostics\":[";
        for (auto index = std::size_t{}; index != event.diagnostics.size(); ++index) {
            if (index != 0) {
                output.push_back(',');
            }
            const auto& value = event.diagnostics[index];
            output += std::format(
                "{{\"file\":{},\"line\":{},\"column\":{},\"severity\":{},\"message\":{}}}",
                json_string(value.file),
                value.line,
                value.column,
                json_string(severity_name(value.severity)),
                json_string(value.message));
        }
        output.push_back(']');
    }
    if (event.kind == build_event_kind::critical_path_updated) {
        output += ",\"critical_path\":[";
        for (auto index = std::size_t{}; index != event.critical_path.size(); ++index) {
            if (index != 0) {
                output.push_back(',');
            }
            const auto& value = event.critical_path[index];
            output += std::format("{{\"action\":{},\"result\":{},\"duration_ms\":{},\"hit\":{}}}",
                                  json_string(to_string(value.action)),
                                  json_string(to_string(value.result.value)),
                                  value.duration.count(),
                                  value.cache == cache_status::hit ? "true" : "false");
        }
        output.push_back(']');
    }
    if (event.test) {
        output += std::format(
            ",\"test\":{{\"name\":{},\"status\":{},\"output\":{},\"duration_ms\":{},\"hit\":{}}}",
            json_string(event.test->name),
            json_string(event.test->status == test_status::passed ? "passed" : "failed"),
            json_string(event.test->output),
            event.test->duration.count(),
            event.test->hit ? "true" : "false");
    }
    if (event.summary) {
        output += std::format(",\"summary\":{{\"exit\":{},\"actions\":{},\"hits\":{},\"failures\":{"
                              "},\"duration_ms\":{}}}",
                              event.summary->exit,
                              event.summary->actions,
                              event.summary->hits,
                              event.summary->failures,
                              event.summary->duration.count());
    }
    output.push_back('}');
    return output;
}

namespace build {
namespace {

auto elapsed(std::chrono::steady_clock::time_point started) -> std::chrono::milliseconds {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 started);
}

runtime::task<std::string>
diagnostic_text(repository& source, const action_result& value, std::string fallback) {
    auto bytes = co_await source.blob(value.stderr_output);
    if (!bytes || bytes->empty()) {
        co_return fallback;
    }
    co_return std::string{reinterpret_cast<const char*>(bytes->data()), bytes->size()};
}

auto execution_for(const action_result& value) -> action_execution {
    const auto remote = value.schedule && value.schedule->decision == execution_decision::remote;
    return {.kind = remote ? action_execution_kind::remote : action_execution_kind::local,
            .worker = remote ? value.worker : ""};
}

runtime::task<void> run_action_events(build_detail::event_producer producer,
                                      repository& source,
                                      action_input input,
                                      remote_executor* remote) {
    auto yielded = co_await producer.await_observer();
    if (!yielded) {
        co_return std::unexpected(yielded.error());
    }
    const auto started = std::chrono::steady_clock::now();
    producer.publish({.kind = build_event_kind::action_queued});
    if (!remote) {
        producer.publish({.kind = build_event_kind::action_started,
                          .execution = action_execution{action_execution_kind::local, {}}});
    }
    auto id = remote ? co_await execute(source, std::move(input), *remote)
                     : co_await execute(source, std::move(input));
    if (!id) {
        if (remote) {
            producer.publish({.kind = build_event_kind::action_started,
                              .execution = action_execution{action_execution_kind::remote, {}}});
        }
        producer.fail(id.error(), {.actions = 1, .duration = elapsed(started)});
        co_return {};
    }
    auto value = co_await result(source, *id);
    if (!value) {
        producer.fail(value.error(), {.actions = 1, .duration = elapsed(started)});
        co_return {};
    }
    if (remote) {
        producer.publish({.kind = build_event_kind::action_started,
                          .action = value->action,
                          .result = *id,
                          .execution = execution_for(*value)});
    }
    auto terminal = build_event{.action = value->action, .result = *id};
    if (value->cache == cache_status::hit) {
        terminal.kind = build_event_kind::action_hit;
    } else if (value->exit == 0) {
        terminal.kind = build_event_kind::action_done;
        terminal.outputs = value->outputs;
    } else {
        terminal.kind = build_event_kind::action_failed;
        auto text = co_await diagnostic_text(source, *value, "action failed");
        if (!text) {
            text = "action failed";
        }
        terminal.diagnostics.push_back(
            {.severity = frontend_diagnostic_severity::error, .message = std::move(*text)});
    }
    producer.publish(std::move(terminal));
    const auto duration = value->timing.queued + value->timing.sandbox + value->timing.exec +
                          value->timing.hash + value->timing.store;
    producer.publish({.kind = build_event_kind::critical_path_updated,
                      .critical_path = {{.result = *id,
                                         .action = value->action,
                                         .duration = duration,
                                         .timing = value->timing,
                                         .cache = value->cache,
                                         .schedule = value->schedule}}});
    producer.finish({.exit = value->exit,
                     .actions = 1,
                     .hits = value->cache == cache_status::hit ? 1U : 0U,
                     .failures = value->exit == 0 ? 0U : 1U,
                     .duration = elapsed(started)});
    co_return {};
}

struct file_stamp {
    std::uintmax_t size{};
    std::filesystem::file_time_type modified;

    auto operator==(const file_stamp&) const -> bool = default;
};

auto tree_snapshot(const std::filesystem::path& root) -> std::map<std::string, file_stamp> {
    auto output = std::map<std::string, file_stamp>{};
    auto code = std::error_code{};
    for (auto iterator =
             std::filesystem::recursive_directory_iterator{
                 root, std::filesystem::directory_options::skip_permission_denied, code};
         !code && iterator != std::filesystem::recursive_directory_iterator{};
         iterator.increment(code)) {
        const auto relative = iterator->path().lexically_relative(root).generic_string();
        if (iterator->is_directory(code) &&
            (relative == ".git" || relative == ".liquid" || relative == ".liquid-store" ||
             relative == ".liquid-build" || relative.starts_with(".git/") ||
             relative.starts_with(".liquid/"))) {
            iterator.disable_recursion_pending();
            continue;
        }
        if (!iterator->is_regular_file(code)) {
            continue;
        }
        const auto size = iterator->file_size(code);
        const auto modified = iterator->last_write_time(code);
        if (!code) {
            output.emplace(relative, file_stamp{size, modified});
        }
    }
    return output;
}

auto source_changed(const std::map<std::string, file_stamp>& before,
                    const std::map<std::string, file_stamp>& after,
                    const wrap_report& report) -> bool {
    auto outputs = std::set<std::string>{};
    for (const auto& action : report.trace_actions) {
        outputs.insert(action.outputs.begin(), action.outputs.end());
    }
    const auto changed = [&outputs](const auto& left, const auto& right) {
        return left != right && !outputs.contains(left.first);
    };
    return !std::ranges::equal(before, after) &&
           (std::ranges::any_of(before,
                                [&after, &changed](const auto& entry) {
                                    const auto found = after.find(entry.first);
                                    return found == after.end() || changed(entry, *found);
                                }) ||
            std::ranges::any_of(after, [&before, &outputs](const auto& entry) {
                return !before.contains(entry.first) && !outputs.contains(entry.first);
            }));
}

runtime::task<void> publish_wrap_report(build_detail::event_producer producer,
                                        const std::filesystem::path& working_tree,
                                        const wrap_options& options,
                                        const wrap_report& report) {
    auto cache_directory = options.cache_directory
                               ? liquid::result<std::filesystem::path>{*options.cache_directory}
                               : wrap_cache_directory(working_tree);
    auto source = liquid::result<repository>{std::unexpected(
        error{errc::not_found, "wrap repository is unavailable", working_tree.string()})};
    if (cache_directory) {
        source = co_await repository::open(*cache_directory / "repository");
    }
    for (const auto& action : report.trace_actions) {
        producer.publish({.kind = build_event_kind::action_queued, .action = action.action});
        auto value = liquid::result<action_result>{std::unexpected(error{
            errc::not_found, "wrapped result is unavailable", to_string(action.result.value)})};
        if (source) {
            value = co_await result(*source, action.result);
        }
        producer.publish({.kind = build_event_kind::action_started,
                          .action = action.action,
                          .execution = value ? execution_for(*value) : action_execution{}});
        if (action.cache == cache_status::hit) {
            producer.publish({.kind = build_event_kind::action_hit,
                              .action = action.action,
                              .result = action.result});
            continue;
        }
        if (value && value->exit != 0) {
            auto text = co_await diagnostic_text(*source, *value, "action failed");
            if (!text) {
                text = "action failed";
            }
            producer.publish({.kind = build_event_kind::action_failed,
                              .action = action.action,
                              .result = action.result,
                              .diagnostics = {{.severity = frontend_diagnostic_severity::error,
                                               .message = std::move(*text)}}});
            continue;
        }
        producer.publish({.kind = build_event_kind::action_done,
                          .action = action.action,
                          .result = action.result,
                          .outputs = value ? value->outputs : std::vector<action_output>{}});
    }
    producer.publish(
        {.kind = build_event_kind::critical_path_updated, .critical_path = report.critical_path});
    co_return {};
}

runtime::task<void> run_wrap_events(build_detail::event_producer producer,
                                    std::filesystem::path working_tree,
                                    std::vector<std::string> command,
                                    wrap_options options) {
    auto yielded = co_await producer.await_observer();
    if (!yielded) {
        co_return std::unexpected(yielded.error());
    }
    const auto started = std::chrono::steady_clock::now();
    producer.publish({.kind = build_event_kind::action_queued});
    producer.publish({.kind = build_event_kind::action_started,
                      .execution = action_execution{action_execution_kind::local, {}}});
    auto before = co_await runtime::offload([working_tree] { return tree_snapshot(working_tree); });
    if (!before) {
        producer.fail(before.error(), {.duration = elapsed(started)});
        co_return {};
    }
    auto report = co_await wrap(working_tree, command, options);
    if (!report) {
        producer.fail(report.error(), {.duration = elapsed(started)});
        co_return {};
    }
    if (report->exit == 0) {
        producer.publish({.kind = build_event_kind::action_done});
    } else {
        producer.publish(
            {.kind = build_event_kind::action_failed,
             .diagnostics = {
                 {.severity = frontend_diagnostic_severity::error,
                  .message = std::format("build command exited with status {}", report->exit)}}});
    }
    auto published = co_await publish_wrap_report(producer, working_tree, options, *report);
    if (!published) {
        producer.fail(published.error(), {.duration = elapsed(started)});
        co_return {};
    }
    auto after = co_await runtime::offload([working_tree] { return tree_snapshot(working_tree); });
    if (!after) {
        producer.fail(after.error(), {.duration = elapsed(started)});
        co_return {};
    }
    if (source_changed(*before, *after, *report) && report->exit == 0) {
        auto replanned = co_await wrap(working_tree, command, options);
        if (!replanned) {
            producer.fail(replanned.error(), {.duration = elapsed(started)});
            co_return {};
        }
        report = std::move(replanned);
        published = co_await publish_wrap_report(producer, working_tree, options, *report);
        if (!published) {
            producer.fail(published.error(), {.duration = elapsed(started)});
            co_return {};
        }
    }
    producer.finish({.exit = report->exit,
                     .actions = report->actions,
                     .hits = report->hits,
                     .failures = report->exit == 0 ? 0U : 1U,
                     .duration = elapsed(started)});
    co_return {};
}

void spawn_event_stream(runtime::task<void> operation, build_detail::event_producer producer) {
    runtime::spawn_serial(std::move(operation), [producer](liquid::result<void> completed) {
        if (!completed) {
            producer.fail(completed.error(), {});
        }
    });
}

} // namespace

liquid::result<build_handle> start(repository& source, action_input input) {
    auto stream = build_detail::event_access::create();
    auto producer = stream.producer;
    spawn_event_stream(run_action_events(producer, source, std::move(input), nullptr), producer);
    return std::move(stream.handle);
}

liquid::result<build_handle>
start(repository& source, action_input input, remote_executor& remote) {
    auto stream = build_detail::event_access::create();
    auto producer = stream.producer;
    spawn_event_stream(run_action_events(producer, source, std::move(input), &remote), producer);
    return std::move(stream.handle);
}

liquid::result<build_handle> start_wrap(std::filesystem::path working_tree,
                                        std::vector<std::string> command,
                                        wrap_options options) {
    auto stream = build_detail::event_access::create();
    auto producer = stream.producer;
    spawn_event_stream(
        run_wrap_events(producer, std::move(working_tree), std::move(command), std::move(options)),
        producer);
    return std::move(stream.handle);
}

} // namespace build
} // namespace liquid
