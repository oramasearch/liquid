#include "liquid/daemon.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <ranges>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "daemon/fanout.hpp"
#include "encoding/cbor.hpp"
#include "liquid/graph.hpp"
#include "liquid/hash.hpp"
#include "search/index.hpp"

namespace liquid {
namespace {

namespace cbor = encoding::cbor;

constexpr auto maximum_frame = std::uint32_t{16U * 1024U * 1024U};

auto failure(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto map(std::initializer_list<std::pair<cbor::value, cbor::value>> fields) -> cbor::value {
    return cbor::value{cbor::value::map{fields}};
}

auto field(const cbor::value& source, std::string_view name) -> const cbor::value* {
    const auto* fields = std::get_if<cbor::value::map>(&source.data);
    if (!fields) {
        return nullptr;
    }
    for (const auto& [key, value] : *fields) {
        const auto* text = std::get_if<std::string>(&key.data);
        if (text && *text == name) {
            return &value;
        }
    }
    return nullptr;
}

auto text_field(const cbor::value& source, std::string_view name) -> const std::string* {
    const auto* value = field(source, name);
    return value ? std::get_if<std::string>(&value->data) : nullptr;
}

auto unsigned_field(const cbor::value& source, std::string_view name) -> const std::uint64_t* {
    const auto* value = field(source, name);
    return value ? std::get_if<std::uint64_t>(&value->data) : nullptr;
}

auto bool_field(const cbor::value& source, std::string_view name) -> const bool* {
    const auto* value = field(source, name);
    return value ? std::get_if<bool>(&value->data) : nullptr;
}

auto encode_frame(std::uint8_t type, std::span<const std::byte> payload)
    -> result<std::vector<std::byte>> {
    if (payload.size() > maximum_frame) {
        return std::unexpected(failure(errc::invalid_argument, "daemon frame is too large"));
    }
    const auto size = static_cast<std::uint32_t>(payload.size());
    auto output = std::vector<std::byte>{};
    output.reserve(5 + payload.size());
    output.push_back(std::byte{type});
    output.push_back(std::byte{static_cast<std::uint8_t>(size >> 24U)});
    output.push_back(std::byte{static_cast<std::uint8_t>(size >> 16U)});
    output.push_back(std::byte{static_cast<std::uint8_t>(size >> 8U)});
    output.push_back(std::byte{static_cast<std::uint8_t>(size)});
    output.insert(output.end(), payload.begin(), payload.end());
    return output;
}

auto encode_request(std::string_view operation) -> result<std::vector<std::byte>> {
    auto payload = cbor::encode(
        map({{"v", cbor::value{std::uint64_t{1}}}, {"op", cbor::value{std::string{operation}}}}));
    if (!payload) {
        return std::unexpected(payload.error());
    }
    return encode_frame(1, *payload);
}

auto encode_error(const error& value) -> result<std::vector<std::byte>> {
    auto payload = cbor::encode(map({{"v", cbor::value{std::uint64_t{1}}},
                                     {"code", cbor::value{std::string{to_string(value.code)}}},
                                     {"message", cbor::value{value.message}},
                                     {"context", cbor::value{value.context}}}));
    if (!payload) {
        return std::unexpected(payload.error());
    }
    return encode_frame(2, *payload);
}

auto status_value(const daemon_status& status) -> cbor::value {
    auto paths = cbor::value::array{};
    for (const auto& path : status.invalidated_paths) {
        paths.emplace_back(path);
    }
    auto targets = cbor::value::array{};
    for (const auto& target : status.targets) {
        targets.push_back(map({{"name", cbor::value{target.name}},
                               {"cursor", cbor::value{target.cursor.token}},
                               {"behind", cbor::value{target.behind}},
                               {"deliveries", cbor::value{target.deliveries}},
                               {"last_error", cbor::value{target.last_error.value_or("")}}}));
    }
    return map(
        {{"v", cbor::value{std::uint64_t{1}}},
         {"running", cbor::value{status.running}},
         {"pid", cbor::value{status.process_id}},
         {"endpoint", cbor::value{status.endpoint}},
         {"last_drip", cbor::value{status.last_drip ? to_string(status.last_drip->value) : ""}},
         {"invalidated", cbor::value{std::move(paths)}},
         {"targets", cbor::value{std::move(targets)}},
         {"capture_state",
          cbor::value{status.capture.state == capture_status::state::paused ? "paused"
                                                                            : "running"}},
         {"capture_pending", cbor::value{status.capture.pending_drips}},
         {"capture_overflow", cbor::value{status.capture.overflow_count}},
         {"capture_recrawl", cbor::value{status.capture.recrawl_count}},
         {"capture_cursor", cbor::value{status.capture.last_cursor.token}},
         {"graph_keys", cbor::value{status.graph_keys}},
         {"graph_values", cbor::value{status.graph_values}},
         {"graph_memory", cbor::value{status.graph_memory_bytes}},
         {"graph_ceiling", cbor::value{status.graph_memory_ceiling}}});
}

auto encode_status(const daemon_status& status) -> result<std::vector<std::byte>> {
    auto payload = cbor::encode(status_value(status));
    if (!payload) {
        return std::unexpected(payload.error());
    }
    return encode_frame(1, *payload);
}

auto encode_acknowledgement() -> result<std::vector<std::byte>> {
    auto payload =
        cbor::encode(map({{"v", cbor::value{std::uint64_t{1}}}, {"ok", cbor::value{true}}}));
    if (!payload) {
        return std::unexpected(payload.error());
    }
    return encode_frame(1, *payload);
}

auto decode_payload(std::span<const std::byte> frame, std::uint8_t expected_type)
    -> result<cbor::value> {
    if (frame.size() < 5 || std::to_integer<std::uint8_t>(frame.front()) != expected_type) {
        return std::unexpected(failure(errc::corrupt, "invalid daemon frame type"));
    }
    auto size = std::uint32_t{};
    for (std::size_t index = 1; index != 5; ++index) {
        size = (size << 8U) | std::to_integer<std::uint8_t>(frame[index]);
    }
    if (size > maximum_frame || frame.size() != 5 + size) {
        return std::unexpected(failure(errc::corrupt, "invalid daemon frame length"));
    }
    return cbor::decode(frame.subspan(5));
}

auto decode_status(std::span<const std::byte> frame) -> result<daemon_status> {
    auto decoded = decode_payload(frame, 1);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    const auto* running = bool_field(*decoded, "running");
    const auto* pid = unsigned_field(*decoded, "pid");
    const auto* endpoint = text_field(*decoded, "endpoint");
    const auto* last_drip = text_field(*decoded, "last_drip");
    const auto* invalidated_value = field(*decoded, "invalidated");
    const auto* targets_value = field(*decoded, "targets");
    const auto* graph_keys = unsigned_field(*decoded, "graph_keys");
    const auto* graph_values = unsigned_field(*decoded, "graph_values");
    const auto* graph_memory = unsigned_field(*decoded, "graph_memory");
    const auto* graph_ceiling = unsigned_field(*decoded, "graph_ceiling");
    const auto* capture_state = text_field(*decoded, "capture_state");
    const auto* capture_pending = unsigned_field(*decoded, "capture_pending");
    const auto* capture_overflow = unsigned_field(*decoded, "capture_overflow");
    const auto* capture_recrawl = unsigned_field(*decoded, "capture_recrawl");
    const auto* capture_cursor = text_field(*decoded, "capture_cursor");
    const auto* invalidated =
        invalidated_value ? std::get_if<cbor::value::array>(&invalidated_value->data) : nullptr;
    const auto* targets =
        targets_value ? std::get_if<cbor::value::array>(&targets_value->data) : nullptr;
    if (!running || !pid || !endpoint || !last_drip || !invalidated || !targets || !graph_keys ||
        !graph_values || !graph_memory || !graph_ceiling || !capture_state || !capture_pending ||
        !capture_overflow || !capture_recrawl || !capture_cursor) {
        return std::unexpected(failure(errc::corrupt, "invalid daemon status"));
    }
    auto output = daemon_status{.running = *running,
                                .process_id = *pid,
                                .endpoint = *endpoint,
                                .capture = {.state = *capture_state == "paused"
                                                         ? capture_status::state::paused
                                                         : capture_status::state::running,
                                            .pending_drips = *capture_pending,
                                            .overflow_count = *capture_overflow,
                                            .recrawl_count = *capture_recrawl,
                                            .last_cursor = {*capture_cursor}},
                                .graph_keys = *graph_keys,
                                .graph_values = *graph_values,
                                .graph_memory_bytes = *graph_memory,
                                .graph_memory_ceiling = *graph_ceiling};
    if (!last_drip->empty()) {
        auto parsed = parse_digest(*last_drip);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        output.last_drip = drip_id{*parsed};
    }
    for (const auto& value : *invalidated) {
        const auto* path = std::get_if<std::string>(&value.data);
        if (!path) {
            return std::unexpected(failure(errc::corrupt, "invalid daemon path status"));
        }
        output.invalidated_paths.push_back(*path);
    }
    for (const auto& value : *targets) {
        const auto* name = text_field(value, "name");
        const auto* cursor = text_field(value, "cursor");
        const auto* behind = unsigned_field(value, "behind");
        const auto* deliveries = unsigned_field(value, "deliveries");
        const auto* last_error = text_field(value, "last_error");
        if (!name || !cursor || !behind || !deliveries || !last_error) {
            return std::unexpected(failure(errc::corrupt, "invalid daemon target status"));
        }
        output.targets.push_back({.name = *name,
                                  .cursor = {*cursor},
                                  .behind = *behind,
                                  .deliveries = *deliveries,
                                  .last_error = last_error->empty()
                                                    ? std::optional<std::string>{}
                                                    : std::optional<std::string>{*last_error}});
    }
    return output;
}

class no_op_target final : public daemon_target {
public:
    explicit no_op_target(std::string target_name) : name_(std::move(target_name)) {}

    auto name() const -> std::string override {
        return name_;
    }

    runtime::task<void> deliver(daemon_delivery) override {
        co_return {};
    }

private:
    std::string name_;
};

class graph_target final : public daemon_target {
public:
    explicit graph_target(graph& value) : graph_(value) {}

    auto name() const -> std::string override {
        return "build";
    }

    runtime::task<void> deliver(daemon_delivery delivery) override {
        for (const auto& operation : delivery.value.ops) {
            auto invalidated = graph_.mark_changed(
                {graph_input_kind::working_tree_blob, operation.path}, durability::low);
            if (!invalidated) {
                co_return std::unexpected(invalidated.error());
            }
        }
        co_return {};
    }

private:
    graph& graph_;
};

#ifndef _WIN32

struct accepted_request {
    int connection{-1};
    std::vector<std::byte> frame;
};

struct unix_handles {
    int lock{-1};
    int listener{-1};
};

auto close_descriptor(int descriptor) -> void {
    if (descriptor >= 0) {
        ::close(descriptor);
    }
}

auto write_all(int descriptor, std::span<const std::byte> bytes) -> result<void> {
    auto offset = std::size_t{};
    while (offset != bytes.size()) {
        const auto written = ::write(
            descriptor, bytes.data() + offset, static_cast<std::size_t>(bytes.size() - offset));
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return std::unexpected(failure(errc::io, std::strerror(errno), "daemon socket"));
        }
        offset += static_cast<std::size_t>(written);
    }
    return {};
}

auto read_all(int descriptor, std::span<std::byte> bytes) -> result<void> {
    auto offset = std::size_t{};
    while (offset != bytes.size()) {
        const auto received = ::read(
            descriptor, bytes.data() + offset, static_cast<std::size_t>(bytes.size() - offset));
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            return std::unexpected(
                failure(errc::io,
                        received == 0 ? "daemon socket closed" : std::strerror(errno),
                        "daemon socket"));
        }
        offset += static_cast<std::size_t>(received);
    }
    return {};
}

auto read_frame(int descriptor) -> result<std::vector<std::byte>> {
    auto header = std::array<std::byte, 5>{};
    auto header_read = read_all(descriptor, header);
    if (!header_read) {
        return std::unexpected(header_read.error());
    }
    auto size = std::uint32_t{};
    for (std::size_t index = 1; index != header.size(); ++index) {
        size = (size << 8U) | std::to_integer<std::uint8_t>(header[index]);
    }
    if (size > maximum_frame) {
        return std::unexpected(failure(errc::corrupt, "daemon frame is too large"));
    }
    auto output = std::vector<std::byte>{header.begin(), header.end()};
    output.resize(header.size() + size);
    auto payload_read = read_all(descriptor, std::span{output}.subspan(header.size()));
    if (!payload_read) {
        return std::unexpected(payload_read.error());
    }
    return output;
}

auto socket_address(const std::filesystem::path& endpoint) -> result<sockaddr_un> {
    const auto text = endpoint.string();
    auto output = sockaddr_un{};
    output.sun_family = AF_UNIX;
    if (text.size() >= sizeof(output.sun_path)) {
        return std::unexpected(
            failure(errc::invalid_argument, "daemon socket path is too long", text));
    }
    std::memcpy(output.sun_path, text.c_str(), text.size() + 1);
    return output;
}

auto open_listener(const std::filesystem::path& directory, const std::filesystem::path& endpoint)
    -> result<unix_handles> {
    std::filesystem::create_directories(directory);
    const auto lock_path = directory / "lock";
    const auto lock_descriptor = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0600);
    if (lock_descriptor < 0) {
        return std::unexpected(failure(errc::io, std::strerror(errno), lock_path.string()));
    }
    if (::flock(lock_descriptor, LOCK_EX | LOCK_NB) != 0) {
        close_descriptor(lock_descriptor);
        return std::unexpected(
            failure(errc::already_exists, "daemon is already running", directory.string()));
    }
    auto address = socket_address(endpoint);
    if (!address) {
        close_descriptor(lock_descriptor);
        return std::unexpected(address.error());
    }
    std::filesystem::remove(endpoint);
    const auto listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0) {
        close_descriptor(lock_descriptor);
        return std::unexpected(failure(errc::io, std::strerror(errno), endpoint.string()));
    }
    if (::bind(listener, reinterpret_cast<const sockaddr*>(&*address), sizeof(*address)) != 0 ||
        ::listen(listener, 16) != 0) {
        const auto message = std::string{std::strerror(errno)};
        close_descriptor(listener);
        close_descriptor(lock_descriptor);
        std::filesystem::remove(endpoint);
        return std::unexpected(failure(errc::io, message, endpoint.string()));
    }
    if (::chmod(endpoint.c_str(), 0600) != 0) {
        const auto message = std::string{std::strerror(errno)};
        close_descriptor(listener);
        close_descriptor(lock_descriptor);
        std::filesystem::remove(endpoint);
        return std::unexpected(failure(errc::io, message, endpoint.string()));
    }
    return unix_handles{lock_descriptor, listener};
}

auto accept_request(int listener) -> result<std::optional<accepted_request>> {
    auto descriptor = pollfd{.fd = listener, .events = POLLIN, .revents = 0};
    const auto ready = ::poll(&descriptor, 1, 50);
    if (ready < 0 && errno == EINTR) {
        return std::optional<accepted_request>{};
    }
    if (ready < 0) {
        return std::unexpected(failure(errc::io, std::strerror(errno), "daemon socket"));
    }
    if (ready == 0) {
        return std::optional<accepted_request>{};
    }
    const auto connection = ::accept(listener, nullptr, nullptr);
    if (connection < 0) {
        return std::unexpected(failure(errc::io, std::strerror(errno), "daemon socket"));
    }
    auto frame = read_frame(connection);
    if (!frame) {
        close_descriptor(connection);
        return std::unexpected(frame.error());
    }
    return std::optional<accepted_request>{{connection, std::move(*frame)}};
}

auto request(const std::filesystem::path& endpoint, std::string operation)
    -> result<std::vector<std::byte>> {
    auto address = socket_address(endpoint);
    if (!address) {
        return std::unexpected(address.error());
    }
    const auto descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor < 0) {
        return std::unexpected(failure(errc::io, std::strerror(errno), endpoint.string()));
    }
    if (::connect(descriptor, reinterpret_cast<const sockaddr*>(&*address), sizeof(*address)) !=
        0) {
        const auto code = errno;
        close_descriptor(descriptor);
        return std::unexpected(
            failure(code == ENOENT || code == ECONNREFUSED ? errc::unavailable : errc::io,
                    std::strerror(code),
                    endpoint.string()));
    }
    auto frame = encode_request(operation);
    if (!frame) {
        close_descriptor(descriptor);
        return std::unexpected(frame.error());
    }
    auto written = write_all(descriptor, *frame);
    if (!written) {
        close_descriptor(descriptor);
        return std::unexpected(written.error());
    }
    auto response = read_frame(descriptor);
    close_descriptor(descriptor);
    return response;
}

#endif

struct service_state {
    ~service_state() {
#ifndef _WIN32
        const auto owns_endpoint = handles.listener >= 0;
        close_descriptor(handles.listener);
        if (handles.lock >= 0) {
            ::flock(handles.lock, LOCK_UN);
            close_descriptor(handles.lock);
        }
        if (owns_endpoint) {
            auto ignored = std::error_code{};
            std::filesystem::remove(endpoint, ignored);
        }
#endif
    }

    std::filesystem::path checkout;
    std::filesystem::path directory;
    std::filesystem::path endpoint;
    daemon_options options;
    graph query_graph;
    std::unique_ptr<daemon_detail::fanout> fanout;
    std::optional<capture> capture_handle;
    std::optional<subscription> capture_subscription;
    std::mutex queue_mutex;
    std::mutex completion_mutex;
    std::deque<daemon_delivery> queue;
    std::optional<error> completion_error;
    std::atomic<bool> stopping{};
    std::atomic<bool> stopped{};
    std::atomic<std::int64_t> last_activity_ns{};
#ifndef _WIN32
    unix_handles handles;
#endif

    auto touch() -> void {
        last_activity_ns.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                               std::memory_order_release);
    }

    auto enqueue(daemon_delivery delivery) -> void;
    auto dequeue() -> std::optional<daemon_delivery>;
};

auto current_status(const std::shared_ptr<service_state>& state) -> daemon_status {
    auto capture_cursor = journal_cursor{};
    auto current_capture = capture_status{};
    if (state->capture_handle) {
        auto status = state->capture_handle->status();
        if (status) {
            current_capture = *status;
            capture_cursor = status->last_cursor;
        }
    }
    const auto graph_status = state->query_graph.statistics();
#ifdef _WIN32
    const auto process_id = std::uint64_t{};
#else
    const auto process_id = static_cast<std::uint64_t>(::getpid());
#endif
    return {.running = !state->stopped.load(std::memory_order_acquire),
            .process_id = process_id,
            .endpoint = state->endpoint.string(),
            .last_drip = state->fanout->last_drip(),
            .invalidated_paths = state->fanout->invalidated_paths(),
            .targets = state->fanout->target_status(capture_cursor),
            .capture = std::move(current_capture),
            .graph_keys = graph_status.keys,
            .graph_values = graph_status.resident_values,
            .graph_memory_bytes = graph_status.resident_value_bytes,
            .graph_memory_ceiling = graph_status.memory_ceiling};
}

auto service_state::enqueue(daemon_delivery delivery) -> void {
    {
        std::lock_guard lock(queue_mutex);
        queue.push_back(std::move(delivery));
    }
    touch();
}

auto service_state::dequeue() -> std::optional<daemon_delivery> {
    std::lock_guard lock(queue_mutex);
    if (queue.empty()) {
        return std::nullopt;
    }
    auto output = std::move(queue.front());
    queue.pop_front();
    return output;
}

#ifndef _WIN32
runtime::task<void> run_service(std::shared_ptr<service_state> state) {
    auto retry_after = std::chrono::steady_clock::time_point::min();
    while (!state->stopping.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= retry_after) {
            if (auto delivery = state->dequeue()) {
                auto delivered = co_await state->fanout->deliver(*delivery);
                if (!delivered) {
                    {
                        std::lock_guard lock(state->queue_mutex);
                        state->queue.push_front(std::move(*delivery));
                    }
                    retry_after = std::chrono::steady_clock::now() + std::chrono::milliseconds{100};
                } else {
                    state->touch();
                    retry_after = std::chrono::steady_clock::time_point::min();
                }
                continue;
            }
        }
        const auto idle = std::chrono::steady_clock::duration{
            std::chrono::steady_clock::now().time_since_epoch().count() -
            state->last_activity_ns.load(std::memory_order_acquire)};
        if (idle >= state->options.idle_timeout) {
            state->stopping.store(true, std::memory_order_release);
            break;
        }
        auto accepted = co_await runtime::offload(
            [listener = state->handles.listener] { return accept_request(listener); });
        if (!accepted) {
            co_return std::unexpected(accepted.error());
        }
        if (!*accepted) {
            continue;
        }
        if (!(**accepted)) {
            continue;
        }
        auto connection = std::move(***accepted);
        state->touch();
        auto decoded = decode_payload(connection.frame, 1);
        auto response = result<std::vector<std::byte>>{};
        if (!decoded) {
            response = encode_error(decoded.error());
        } else {
            const auto* version = unsigned_field(*decoded, "v");
            const auto* operation = text_field(*decoded, "op");
            if (!version || *version != 1 || !operation) {
                response = encode_error(failure(errc::invalid_argument, "invalid daemon request"));
            } else if (*operation == "status") {
                response = encode_status(current_status(state));
            } else if (*operation == "shutdown") {
                response = encode_acknowledgement();
                state->stopping.store(true, std::memory_order_release);
            } else if (*operation == "pause" || *operation == "resume") {
                auto changed = *operation == "pause" ? co_await state->capture_handle->pause()
                                                     : co_await state->capture_handle->resume();
                response = changed ? encode_acknowledgement() : encode_error(changed.error());
            } else {
                response = encode_error(
                    failure(errc::invalid_argument, "unknown daemon request", *operation));
            }
        }
        if (!response) {
            close_descriptor(connection.connection);
            co_return std::unexpected(response.error());
        }
        auto written = co_await runtime::offload(
            [descriptor = connection.connection, bytes = std::move(*response)] {
                auto output = write_all(descriptor, bytes);
                close_descriptor(descriptor);
                return output;
            });
        if (!written) {
            co_return std::unexpected(written.error());
        }
        if (!*written) {
            co_return std::unexpected(written->error());
        }
    }
    if (state->capture_handle) {
        auto closed = co_await state->capture_handle->close();
        if (!closed) {
            co_return std::unexpected(closed.error());
        }
    }
    auto persisted = co_await state->query_graph.flush();
    if (!persisted) {
        co_return std::unexpected(persisted.error());
    }
    close_descriptor(state->handles.listener);
    state->handles.listener = -1;
    std::filesystem::remove(state->endpoint);
    if (state->handles.lock >= 0) {
        ::flock(state->handles.lock, LOCK_UN);
        close_descriptor(state->handles.lock);
        state->handles.lock = -1;
    }
    co_return {};
}
#endif

auto unsupported_endpoint() -> error {
    return failure(errc::unsupported,
                   "the current Corosio dependency has no Windows named-pipe transport",
                   "daemon endpoint");
}

auto endpoint_path(const std::filesystem::path& checkout) -> result<std::filesystem::path> {
    const auto text = checkout.generic_string();
    auto id = hash(algorithm::blake3,
                   std::span{reinterpret_cast<const std::byte*>(text.data()), text.size()});
    if (!id) {
        return std::unexpected(id.error());
    }
    const auto digest_text = to_string(*id);
    return std::filesystem::temp_directory_path() /
           ("liquid-daemon-" + digest_text.substr(digest_text.find(':') + 1, 24) + ".sock");
}

} // namespace

struct daemon::impl {
    std::filesystem::path endpoint;
    std::shared_ptr<service_state> hosted;
};

daemon::daemon() noexcept = default;
daemon::~daemon() = default;
daemon::daemon(daemon&&) noexcept = default;
daemon& daemon::operator=(daemon&&) noexcept = default;
daemon::daemon(std::unique_ptr<impl> implementation) noexcept : impl_(std::move(implementation)) {}

runtime::task<daemon> daemon::start(std::filesystem::path checkout, daemon_options options) {
#ifdef _WIN32
    static_cast<void>(checkout);
    static_cast<void>(options);
    co_return std::unexpected(unsupported_endpoint());
#else
    try {
        checkout = std::filesystem::weakly_canonical(std::filesystem::absolute(checkout));
        if (!std::filesystem::exists(checkout / "LIQUID_REPO")) {
            co_return std::unexpected(
                failure(errc::not_found, "checkout is not a liquid repository", checkout.string()));
        }
        if (options.idle_timeout <= std::chrono::milliseconds{0}) {
            co_return std::unexpected(
                failure(errc::invalid_argument, "daemon idle timeout must be positive"));
        }
        auto state = std::make_shared<service_state>();
        state->checkout = std::move(checkout);
        state->directory = state->checkout / ".liquid" / "daemon";
        auto endpoint = endpoint_path(state->checkout);
        if (!endpoint) {
            co_return std::unexpected(endpoint.error());
        }
        state->endpoint = std::move(*endpoint);
        state->options = std::move(options);
        state->options.capture.exclude.push_back(".liquid-store/**");
        state->options.capture.exclude.push_back("LIQUID_REPO");
        state->options.capture.exclude.push_back("LIQUID_JOURNAL");
        auto handles =
            co_await runtime::offload([directory = state->directory, endpoint = state->endpoint] {
                return open_listener(directory, endpoint);
            });
        if (!handles) {
            co_return std::unexpected(handles.error());
        }
        if (!*handles) {
            co_return std::unexpected(handles->error());
        }
        state->handles = **handles;
        auto loaded_graph = co_await graph::open(
            state->directory / "query-graph",
            query_registry{},
            {.working_tree = state->checkout, .memory_ceiling = state->options.graph_memory});
        if (!loaded_graph) {
            co_return std::unexpected(loaded_graph.error());
        }
        state->query_graph = std::move(*loaded_graph);
        auto targets = std::vector<std::shared_ptr<daemon_target>>{};
        targets.push_back(std::make_shared<graph_target>(state->query_graph));
        if (state->options.targets.empty()) {
            targets.push_back(search_detail::make_daemon_target(state->checkout));
            targets.push_back(std::make_shared<no_op_target>("collaborators"));
            targets.push_back(std::make_shared<no_op_target>("lake"));
        } else {
            targets.insert(
                targets.end(), state->options.targets.begin(), state->options.targets.end());
        }
        auto names = std::set<std::string>{};
        for (const auto& target : targets) {
            if (!target || target->name().empty() || !names.insert(target->name()).second) {
                co_return std::unexpected(
                    failure(errc::invalid_argument, "daemon target names must be unique"));
            }
        }
        state->fanout = std::make_unique<daemon_detail::fanout>(state->directory / "state.cbor",
                                                                std::move(targets));
        auto loaded_state = co_await runtime::offload([state] { return state->fanout->load(); });
        if (!loaded_state) {
            co_return std::unexpected(loaded_state.error());
        }
        if (!*loaded_state) {
            co_return std::unexpected(loaded_state->error());
        }
        auto capture_handle = co_await capture::open(state->checkout, state->options.capture);
        if (!capture_handle) {
            co_return std::unexpected(capture_handle.error());
        }
        state->capture_handle.emplace(std::move(*capture_handle));
        auto subscribed = state->capture_handle->subscribe_from(
            state->fanout->replay_cursor(),
            [weak = std::weak_ptr{state}](const drip& value, const journal_cursor& cursor) {
                if (auto shared = weak.lock()) {
                    shared->enqueue({value, cursor});
                }
            });
        if (!subscribed) {
            co_return std::unexpected(subscribed.error());
        }
        state->capture_subscription.emplace(std::move(*subscribed));
        state->touch();
        runtime::spawn(run_service(state), [state](result<void> completed) {
            if (!completed) {
                std::lock_guard lock(state->completion_mutex);
                state->completion_error = completed.error();
            }
            state->stopped.store(true, std::memory_order_release);
        });
        auto implementation = std::make_unique<impl>();
        implementation->endpoint = state->endpoint;
        implementation->hosted = std::move(state);
        co_return daemon{std::move(implementation)};
    } catch (const std::exception& caught) {
        co_return std::unexpected(failure(errc::internal, caught.what(), "daemon::start"));
    }
#endif
}

runtime::task<daemon> daemon::attach(std::filesystem::path checkout) {
#ifdef _WIN32
    static_cast<void>(checkout);
    co_return std::unexpected(unsupported_endpoint());
#else
    checkout = std::filesystem::weakly_canonical(std::filesystem::absolute(checkout));
    auto implementation = std::make_unique<impl>();
    auto endpoint = endpoint_path(checkout);
    if (!endpoint) {
        co_return std::unexpected(endpoint.error());
    }
    implementation->endpoint = std::move(*endpoint);
    auto response = co_await runtime::offload(
        [endpoint = implementation->endpoint] { return request(endpoint, "status"); });
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (!*response) {
        co_return std::unexpected(response->error());
    }
    auto decoded = decode_status(**response);
    if (!decoded) {
        co_return std::unexpected(decoded.error());
    }
    co_return daemon{std::move(implementation)};
#endif
}

runtime::task<daemon_status> daemon::status() {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "daemon is not attached"));
    }
#ifdef _WIN32
    co_return std::unexpected(unsupported_endpoint());
#else
    auto response = co_await runtime::offload(
        [endpoint = impl_->endpoint] { return request(endpoint, "status"); });
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (!*response) {
        co_return std::unexpected(response->error());
    }
    co_return decode_status(**response);
#endif
}

runtime::task<void> daemon::shutdown() {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "daemon is not attached"));
    }
#ifdef _WIN32
    co_return std::unexpected(unsupported_endpoint());
#else
    auto response = co_await runtime::offload(
        [endpoint = impl_->endpoint] { return request(endpoint, "shutdown"); });
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (!*response) {
        co_return std::unexpected(response->error());
    }
    auto decoded = decode_payload(**response, 1);
    if (!decoded) {
        co_return std::unexpected(decoded.error());
    }
    for (auto attempt = 0; attempt != 500; ++attempt) {
        auto probe = co_await runtime::offload(
            [endpoint = impl_->endpoint] { return request(endpoint, "status"); });
        if (!probe) {
            co_return std::unexpected(probe.error());
        }
        if (!*probe &&
            (probe->error().code == errc::unavailable || probe->error().code == errc::io)) {
            co_return {};
        }
        if (!*probe) {
            co_return std::unexpected(probe->error());
        }
        auto [delay_error] = co_await runtime::delay(std::chrono::milliseconds{10});
        if (delay_error) {
            co_return std::unexpected(
                failure(errc::internal, delay_error.message(), "daemon shutdown"));
        }
    }
    co_return std::unexpected(failure(
        errc::unavailable, "daemon endpoint remained after shutdown", impl_->endpoint.string()));
#endif
}

runtime::task<void> daemon::pause_capture() {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "daemon is not attached"));
    }
#ifdef _WIN32
    co_return std::unexpected(unsupported_endpoint());
#else
    auto response = co_await runtime::offload(
        [endpoint = impl_->endpoint] { return request(endpoint, "pause"); });
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (!*response) {
        co_return std::unexpected(response->error());
    }
    auto decoded = decode_payload(**response, 1);
    if (!decoded) {
        co_return std::unexpected(decoded.error());
    }
    co_return {};
#endif
}

runtime::task<void> daemon::resume_capture() {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "daemon is not attached"));
    }
#ifdef _WIN32
    co_return std::unexpected(unsupported_endpoint());
#else
    auto response = co_await runtime::offload(
        [endpoint = impl_->endpoint] { return request(endpoint, "resume"); });
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (!*response) {
        co_return std::unexpected(response->error());
    }
    auto decoded = decode_payload(**response, 1);
    if (!decoded) {
        co_return std::unexpected(decoded.error());
    }
    co_return {};
#endif
}

runtime::task<void> daemon::wait() {
    if (!impl_ || !impl_->hosted) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "only a started daemon can be waited"));
    }
    while (!impl_->hosted->stopped.load(std::memory_order_acquire)) {
        auto [delay_error] = co_await runtime::delay(std::chrono::milliseconds{5});
        if (delay_error) {
            co_return std::unexpected(
                failure(errc::internal, delay_error.message(), "daemon wait"));
        }
    }
    {
        std::lock_guard lock(impl_->hosted->completion_mutex);
        if (impl_->hosted->completion_error) {
            co_return std::unexpected(*impl_->hosted->completion_error);
        }
    }
    co_return {};
}

} // namespace liquid
