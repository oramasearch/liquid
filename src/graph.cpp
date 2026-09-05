#include "liquid/graph.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <stop_token>
#include <system_error>
#include <utility>

#include <boost/capy/ex/run.hpp>

#include "encoding/cbor.hpp"
#include "liquid/store.hpp"

namespace liquid {
namespace {

namespace cbor = encoding::cbor;

auto failure(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto map(std::initializer_list<std::pair<cbor::value, cbor::value>> fields) -> cbor::value {
    return cbor::value{cbor::value::map{fields}};
}

auto bytes(std::span<const std::byte> input) -> cbor::value {
    return cbor::value{cbor::value::bytes{input.begin(), input.end()}};
}

auto find(const cbor::value& source, std::string_view name) -> const cbor::value* {
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

auto encode_query_key(const query_key& key) -> cbor::value {
    return map({{"type", "query"}, {"kind", key.kind}, {"arguments", bytes(key.arguments)}});
}

auto encode_input_key(const graph_input_key& key) -> cbor::value {
    return map({{"type", "input"},
                {"input_kind", static_cast<std::uint64_t>(key.kind)},
                {"name", key.name}});
}

auto encode_dependency_key(const graph_dependency_key& key) -> cbor::value {
    return std::visit(
        [](const auto& value) -> cbor::value {
            using value_type = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<value_type, query_key>) {
                return encode_query_key(value);
            } else {
                return encode_input_key(value);
            }
        },
        key);
}

auto decode_query_key(const cbor::value& value) -> result<query_key> {
    const auto* type_value = find(value, "type");
    const auto* kind_value = find(value, "kind");
    const auto* arguments_value = find(value, "arguments");
    const auto* type = type_value ? std::get_if<std::string>(&type_value->data) : nullptr;
    const auto* kind = kind_value ? std::get_if<std::string>(&kind_value->data) : nullptr;
    const auto* arguments =
        arguments_value ? std::get_if<cbor::value::bytes>(&arguments_value->data) : nullptr;
    if (!type || *type != "query" || !kind || !arguments) {
        return std::unexpected(failure(errc::corrupt, "invalid query key", "query graph"));
    }
    return query_key{*kind, *arguments};
}

auto decode_dependency_key(const cbor::value& value) -> result<graph_dependency_key> {
    const auto* type_value = find(value, "type");
    const auto* type = type_value ? std::get_if<std::string>(&type_value->data) : nullptr;
    if (!type) {
        return std::unexpected(failure(errc::corrupt, "dependency has no type", "query graph"));
    }
    if (*type == "query") {
        auto key = decode_query_key(value);
        if (!key) {
            return std::unexpected(key.error());
        }
        return graph_dependency_key{std::move(*key)};
    }
    const auto* kind_value = find(value, "input_kind");
    const auto* name_value = find(value, "name");
    const auto* kind = kind_value ? std::get_if<std::uint64_t>(&kind_value->data) : nullptr;
    const auto* name = name_value ? std::get_if<std::string>(&name_value->data) : nullptr;
    if (*type != "input" || !kind || *kind < 1 || *kind > 4 || !name) {
        return std::unexpected(failure(errc::corrupt, "invalid input key", "query graph"));
    }
    return graph_dependency_key{
        graph_input_key{static_cast<graph_input_kind>(*kind), std::move(*name)}};
}

auto encode_dependencies(std::span<const recorded_dependency> values) -> cbor::value {
    auto output = cbor::value::array{};
    for (const auto& value : values) {
        output.emplace_back(map({{"key", encode_dependency_key(value.key)},
                                 {"fingerprint", to_string(value.fingerprint)}}));
    }
    return cbor::value{std::move(output)};
}

auto decode_dependencies(const cbor::value& value) -> result<std::vector<recorded_dependency>> {
    const auto* values = std::get_if<cbor::value::array>(&value.data);
    if (!values) {
        return std::unexpected(
            failure(errc::corrupt, "invalid recorded dependency list", "query graph"));
    }
    auto output = std::vector<recorded_dependency>{};
    for (const auto& dependency_value : *values) {
        const auto* dependency_key_value = find(dependency_value, "key");
        const auto* dependency_fingerprint_value = find(dependency_value, "fingerprint");
        const auto* dependency_fingerprint_text =
            dependency_fingerprint_value
                ? std::get_if<std::string>(&dependency_fingerprint_value->data)
                : nullptr;
        if (!dependency_key_value || !dependency_fingerprint_text) {
            return std::unexpected(
                failure(errc::corrupt, "invalid recorded dependency", "query graph"));
        }
        auto dependency_key = decode_dependency_key(*dependency_key_value);
        auto dependency_fingerprint = parse_digest(*dependency_fingerprint_text);
        if (!dependency_key || !dependency_fingerprint) {
            return std::unexpected(
                failure(errc::corrupt, "invalid recorded dependency fields", "query graph"));
        }
        output.push_back({std::move(*dependency_key), *dependency_fingerprint});
    }
    return output;
}

auto encode_validation_record(const graph_validation_record& value)
    -> result<std::vector<std::byte>> {
    return cbor::encode(
        map({{"v", std::uint64_t{1}},
             {"kind", "query-validation"},
             {"query", encode_query_key(value.query)},
             {"inputs", encode_dependencies(value.inputs)},
             {"incremental", to_string(value.incremental)},
             {"clean", to_string(value.clean)},
             {"agreement", value.agreement},
             {"severity", value.agreement ? "notice" : "critical"},
             {"at", static_cast<std::uint64_t>(value.at.time_since_epoch().count())}}));
}

auto decode_validation_record(const cbor::value& value, digest id)
    -> result<graph_validation_record> {
    const auto* version_value = find(value, "v");
    const auto* kind_value = find(value, "kind");
    const auto* query_value = find(value, "query");
    const auto* inputs_value = find(value, "inputs");
    const auto* incremental_value = find(value, "incremental");
    const auto* clean_value = find(value, "clean");
    const auto* agreement_value = find(value, "agreement");
    const auto* severity_value = find(value, "severity");
    const auto* at_value = find(value, "at");
    const auto* version =
        version_value ? std::get_if<std::uint64_t>(&version_value->data) : nullptr;
    const auto* kind = kind_value ? std::get_if<std::string>(&kind_value->data) : nullptr;
    const auto* incremental =
        incremental_value ? std::get_if<std::string>(&incremental_value->data) : nullptr;
    const auto* clean = clean_value ? std::get_if<std::string>(&clean_value->data) : nullptr;
    const auto* agreement = agreement_value ? std::get_if<bool>(&agreement_value->data) : nullptr;
    const auto* severity =
        severity_value ? std::get_if<std::string>(&severity_value->data) : nullptr;
    const auto* at = at_value ? std::get_if<std::uint64_t>(&at_value->data) : nullptr;
    if (!version || *version != 1 || !kind || *kind != "query-validation" || !query_value ||
        !inputs_value || !incremental || !clean || !agreement || !severity ||
        *severity != (*agreement ? "notice" : "critical") || !at) {
        return std::unexpected(
            failure(errc::corrupt, "invalid query validation record", "query graph"));
    }
    auto query = decode_query_key(*query_value);
    auto inputs = decode_dependencies(*inputs_value);
    auto incremental_id = parse_digest(*incremental);
    auto clean_id = parse_digest(*clean);
    if (!query || !inputs || !incremental_id || !clean_id) {
        return std::unexpected(
            failure(errc::corrupt, "invalid query validation fields", "query graph"));
    }
    return graph_validation_record{.id = id,
                                   .query = std::move(*query),
                                   .inputs = std::move(*inputs),
                                   .incremental = *incremental_id,
                                   .clean = *clean_id,
                                   .agreement = *agreement,
                                   .at = std::chrono::sys_seconds{std::chrono::seconds{*at}}};
}

auto key_token(const graph_dependency_key& key) -> result<std::string> {
    auto encoded = cbor::encode(encode_dependency_key(key));
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    auto output = std::string{};
    output.reserve(encoded->size() + 1);
    output.push_back(std::holds_alternative<query_key>(key) ? 'q' : 'i');
    output.append(reinterpret_cast<const char*>(encoded->data()), encoded->size());
    return output;
}

auto query_token(const query_key& key) -> result<std::string> {
    return key_token(graph_dependency_key{key});
}

auto fingerprint(std::string_view domain, std::span<const std::byte> value) -> result<digest> {
    auto encoded = cbor::encode(
        map({{"v", std::uint64_t{1}}, {"kind", std::string{domain}}, {"value", bytes(value)}}));
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return hash(algorithm::blake3, *encoded);
}

auto minimum(durability left, durability right) -> durability {
    return static_cast<durability>(
        std::min(static_cast<std::uint8_t>(left), static_cast<std::uint8_t>(right)));
}

auto same_digest(const digest& left, const digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

auto default_durability(graph_input_kind kind) -> durability {
    switch (kind) {
    case graph_input_kind::overlay_buffer:
        return durability::volatile_;
    case graph_input_kind::working_tree_blob:
        return durability::low;
    case graph_input_kind::environment:
        return durability::medium;
    case graph_input_kind::toolchain:
        return durability::high;
    }
    return durability::volatile_;
}

auto read_file(const std::filesystem::path& path) -> result<std::vector<std::byte>> {
    auto input = std::ifstream{path, std::ios::binary};
    if (!input) {
        return std::unexpected(
            failure(errc::not_found, "graph input does not exist", path.string()));
    }
    input.seekg(0, std::ios::end);
    const auto length = input.tellg();
    if (length < 0) {
        return std::unexpected(failure(errc::io, "could not size graph input", path.string()));
    }
    input.seekg(0, std::ios::beg);
    auto output = std::vector<std::byte>(static_cast<std::size_t>(length));
    input.read(reinterpret_cast<char*>(output.data()), length);
    if (!input && length != 0) {
        return std::unexpected(failure(errc::io, "could not read graph input", path.string()));
    }
    return output;
}

auto read_text_file(const std::filesystem::path& path) -> result<std::string> {
    auto input = std::ifstream{path, std::ios::binary};
    if (!input) {
        return std::unexpected(
            failure(errc::not_found, "graph root does not exist", path.string()));
    }
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

auto write_root_file(const std::filesystem::path& path, std::string contents) -> result<void> {
    const auto temporary = path.parent_path() / "ROOT.tmp";
    auto output = std::ofstream{temporary, std::ios::binary | std::ios::trunc};
    if (!output) {
        return std::unexpected(failure(errc::io, "could not open graph root", temporary.string()));
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.close();
    if (!output) {
        return std::unexpected(failure(errc::io, "could not write graph root", temporary.string()));
    }
    auto code = std::error_code{};
    std::filesystem::rename(temporary, path, code);
    if (code) {
        std::filesystem::remove(path, code);
        code.clear();
        std::filesystem::rename(temporary, path, code);
    }
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), path.string()));
    }
    return {};
}

auto remove_validation_directory(const std::filesystem::path& path) -> result<void> {
    auto code = std::error_code{};
    std::filesystem::remove_all(path, code);
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), path.string()));
    }
    return {};
}

auto create_validation_directory(const std::filesystem::path& parent)
    -> result<std::filesystem::path> {
    auto code = std::error_code{};
    std::filesystem::create_directories(parent, code);
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), parent.string()));
    }
    static auto sequence = std::atomic_uint64_t{};
    for (auto attempt = 0U; attempt != 100U; ++attempt) {
        const auto timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
        const auto candidate =
            parent / (std::to_string(timestamp) + "-" + std::to_string(sequence.fetch_add(1)));
        const auto created = std::filesystem::create_directory(candidate, code);
        if (created) {
            return candidate;
        }
        if (code) {
            return std::unexpected(failure(errc::io, code.message(), candidate.string()));
        }
    }
    return std::unexpected(
        failure(errc::already_exists, "could not allocate validation directory", parent.string()));
}

} // namespace

struct query_registry::impl {
    std::map<std::string, query_kind> kinds;
};

query_registry::query_registry() : impl_(std::make_shared<impl>()) {}
query_registry::~query_registry() = default;
query_registry::query_registry(const query_registry&) noexcept = default;
query_registry& query_registry::operator=(const query_registry&) noexcept = default;
query_registry::query_registry(query_registry&&) noexcept = default;
query_registry& query_registry::operator=(query_registry&&) noexcept = default;

result<void> query_registry::add(query_kind kind) {
    if (kind.name.empty() || !kind.compute) {
        return std::unexpected(
            failure(errc::invalid_argument, "query kind requires a name and compute function"));
    }
    if (impl_->kinds.contains(kind.name)) {
        return std::unexpected(
            failure(errc::already_exists, "query kind is already registered", kind.name));
    }
    impl_->kinds.emplace(kind.name, std::move(kind));
    return {};
}

result<query_key> query_registry::key(std::string kind,
                                      std::span<const std::byte> arguments) const {
    const auto found = impl_->kinds.find(kind);
    if (found == impl_->kinds.end()) {
        return std::unexpected(failure(errc::not_found, "query kind is not registered", kind));
    }
    auto encoded = std::vector<std::byte>{arguments.begin(), arguments.end()};
    if (found->second.encode_arguments) {
        auto canonical = found->second.encode_arguments(arguments);
        if (!canonical) {
            return std::unexpected(canonical.error());
        }
        encoded = std::move(*canonical);
    }
    return query_key{std::move(kind), std::move(encoded)};
}

struct graph::impl {
    struct node {
        query_key key;
        digest fingerprint;
        std::vector<recorded_dependency> dependencies;
        durability tier{durability::high};
        std::optional<digest> value_id;
        std::optional<digest> record_id;
        std::optional<digest> validated_fingerprint;
        std::optional<std::vector<std::byte>> value;
        bool stale{true};
        std::uint64_t generation{};
        std::uint64_t last_demand{};
        std::shared_ptr<std::stop_source> running;
    };

    struct input_override {
        graph_input_key key;
        std::vector<std::byte> value;
        durability tier{durability::high};
    };

    std::filesystem::path directory;
    graph_options options;
    query_registry registry;
    store records;
    mutable std::mutex mutex;
    std::map<std::string, node> nodes;
    std::map<std::string, input_override> inputs;
    std::map<std::string, std::set<std::string>> reverse;
    std::vector<graph_validation_record> validations;
    std::vector<digest> validation_ids;
    graph_statistics counters;
    std::uint64_t resident_value_bytes{};
    std::uint64_t demand_sequence{};
    bool transient{};

    [[nodiscard]] runtime::task<graph_value> demand(query_key key, std::vector<std::string> stack);
    [[nodiscard]] runtime::task<graph_value> read_input(graph_input_key key);
    [[nodiscard]] runtime::task<void> persist_node(std::string token);
    [[nodiscard]] runtime::task<void> persist_root();
    [[nodiscard]] runtime::task<void> load();
    [[nodiscard]] result<std::uint64_t>
    invalidate(std::string token, durability changed_tier, bool include_token);
    void replace_reverse_edges(const std::string& token,
                               std::span<const recorded_dependency> before,
                               std::span<const recorded_dependency> after);
    void cache_value(node& target, std::vector<std::byte> value);
    void discard_value(node& target);
    void evict_values();
    [[nodiscard]] std::vector<recorded_dependency> recorded_inputs(const std::string& token) const;
};

struct query_context::impl {
    graph::impl* owner{};
    std::vector<std::string> stack;
    std::vector<recorded_dependency> dependencies;
    durability tier{durability::high};
};

query_context::query_context(std::unique_ptr<impl> implementation)
    : impl_(std::move(implementation)) {}
query_context::query_context(query_context&&) noexcept = default;
query_context& query_context::operator=(query_context&&) noexcept = default;
query_context::~query_context() = default;

runtime::task<graph_value> query_context::read(graph_dependency_key key) {
    const auto stop = co_await boost::capy::this_coro::stop_token;
    if (stop.stop_requested()) {
        co_return std::unexpected(
            failure(errc::unavailable, "query computation was cancelled", "query graph"));
    }
    auto value = std::holds_alternative<query_key>(key)
                     ? co_await impl_->owner->demand(std::get<query_key>(key), impl_->stack)
                     : co_await impl_->owner->read_input(std::get<graph_input_key>(key));
    if (!value) {
        co_return std::unexpected(value.error());
    }
    if (stop.stop_requested()) {
        co_return std::unexpected(
            failure(errc::unavailable, "query computation was cancelled", "query graph"));
    }
    impl_->dependencies.push_back(recorded_dependency{std::move(key), value->fingerprint});
    impl_->tier = minimum(impl_->tier, value->durability_tier);
    {
        auto lock = std::scoped_lock{impl_->owner->mutex};
        ++impl_->owner->counters.dependency_reads;
    }
    co_return std::move(*value);
}

runtime::task<graph_value> graph::impl::read_input(graph_input_key key) {
    auto token = key_token(graph_dependency_key{key});
    if (!token) {
        co_return std::unexpected(token.error());
    }
    auto override = std::optional<input_override>{};
    {
        auto lock = std::scoped_lock{mutex};
        ++counters.fingerprint_reads;
        const auto found = inputs.find(*token);
        if (found != inputs.end()) {
            override = found->second;
        }
    }
    auto value = std::vector<std::byte>{};
    auto tier = default_durability(key.kind);
    auto missing = false;
    if (override) {
        value = std::move(override->value);
        tier = override->tier;
    } else if (key.kind == graph_input_kind::working_tree_blob) {
        auto path = std::filesystem::path{key.name};
        if (path.is_absolute() || key.name.empty()) {
            co_return std::unexpected(
                failure(errc::invalid_argument, "working-tree input must be relative", key.name));
        }
        for (const auto& part : path) {
            if (part == "..") {
                co_return std::unexpected(failure(
                    errc::invalid_argument, "working-tree input escapes its root", key.name));
            }
        }
        auto loaded = co_await runtime::offload(
            [path = options.working_tree / path] { return read_file(path); });
        if (!loaded) {
            co_return std::unexpected(loaded.error());
        }
        if (!*loaded) {
            if (loaded->error().code == errc::not_found) {
                missing = true;
            } else {
                co_return std::unexpected(loaded->error());
            }
        } else {
            value = std::move(**loaded);
        }
    } else if (key.kind == graph_input_kind::environment) {
        if (const auto* found = std::getenv(key.name.c_str())) {
            const auto text = std::string_view{found};
            value.resize(text.size());
            std::ranges::copy(text, reinterpret_cast<char*>(value.data()));
        }
    } else {
        co_return std::unexpected(failure(errc::not_found, "graph input is not set", key.name));
    }
    auto id = fingerprint(missing ? "query-input-missing" : "query-input", value);
    if (!id) {
        co_return std::unexpected(id.error());
    }
    co_return graph_value{std::move(value), *id, tier};
}

void graph::impl::replace_reverse_edges(const std::string& token,
                                        std::span<const recorded_dependency> before,
                                        std::span<const recorded_dependency> after) {
    for (const auto& dependency : before) {
        auto dependency_token = key_token(dependency.key);
        if (dependency_token) {
            reverse[*dependency_token].erase(token);
        }
    }
    for (const auto& dependency : after) {
        auto dependency_token = key_token(dependency.key);
        if (dependency_token) {
            reverse[*dependency_token].insert(token);
        }
    }
}

void graph::impl::cache_value(node& target, std::vector<std::byte> value) {
    discard_value(target);
    resident_value_bytes += value.size();
    target.value = std::move(value);
    target.last_demand = ++demand_sequence;
    evict_values();
}

void graph::impl::discard_value(node& target) {
    if (!target.value) {
        return;
    }
    resident_value_bytes -= target.value->size();
    target.value.reset();
}

void graph::impl::evict_values() {
    while (options.memory_ceiling != 0 && resident_value_bytes > options.memory_ceiling) {
        auto candidate = nodes.end();
        for (auto iterator = nodes.begin(); iterator != nodes.end(); ++iterator) {
            if (!iterator->second.value) {
                continue;
            }
            if (candidate == nodes.end() ||
                iterator->second.last_demand < candidate->second.last_demand) {
                candidate = iterator;
            }
        }
        if (candidate == nodes.end()) {
            break;
        }
        discard_value(candidate->second);
    }
}

std::vector<recorded_dependency> graph::impl::recorded_inputs(const std::string& token) const {
    auto output = std::vector<recorded_dependency>{};
    auto visited_queries = std::set<std::string>{};
    auto visited_inputs = std::set<std::string>{};
    auto visit = std::function<void(const std::string&)>{};
    visit = [&](const std::string& current) {
        if (!visited_queries.insert(current).second) {
            return;
        }
        const auto found = nodes.find(current);
        if (found == nodes.end()) {
            return;
        }
        for (const auto& dependency : found->second.dependencies) {
            auto dependency_token = key_token(dependency.key);
            if (!dependency_token) {
                continue;
            }
            if (std::holds_alternative<graph_input_key>(dependency.key)) {
                if (visited_inputs.insert(*dependency_token).second) {
                    output.push_back(dependency);
                }
                continue;
            }
            visit(*dependency_token);
        }
    };
    visit(token);
    return output;
}

result<std::uint64_t>
graph::impl::invalidate(std::string token, durability changed_tier, bool include_token) {
    auto pending = std::vector<std::string>{};
    auto seen = std::set<std::string>{};
    if (include_token) {
        pending.push_back(token);
    } else if (const auto found = reverse.find(token); found != reverse.end()) {
        pending.insert(pending.end(), found->second.begin(), found->second.end());
    }
    std::uint64_t walked{};
    while (!pending.empty()) {
        auto current = std::move(pending.back());
        pending.pop_back();
        if (!seen.insert(current).second) {
            continue;
        }
        const auto found = nodes.find(current);
        if (found == nodes.end() || found->second.tier > changed_tier) {
            continue;
        }
        ++walked;
        found->second.stale = true;
        ++found->second.generation;
        if (found->second.running) {
            found->second.running->request_stop();
        }
        const auto dependents = reverse.find(current);
        if (dependents != reverse.end()) {
            pending.insert(pending.end(), dependents->second.begin(), dependents->second.end());
        }
    }
    counters.invalidation_walks += walked;
    return walked;
}

runtime::task<graph_value> graph::impl::demand(query_key key, std::vector<std::string> stack) {
    auto token = query_token(key);
    if (!token) {
        co_return std::unexpected(token.error());
    }
    if (std::ranges::find(stack, *token) != stack.end()) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "query dependency cycle", key.kind));
    }
    stack.push_back(*token);

    node snapshot;
    bool exists{};
    {
        auto lock = std::scoped_lock{mutex};
        ++counters.demands;
        const auto found = nodes.find(*token);
        if (found != nodes.end()) {
            snapshot = found->second;
            exists = true;
        }
    }
    const auto has_recorded_value = exists && snapshot.value_id.has_value();
    if (has_recorded_value && !snapshot.stale) {
        auto value = snapshot.value;
        if (!value) {
            auto loaded = co_await records.get(*snapshot.value_id);
            if (!loaded) {
                co_return std::unexpected(loaded.error());
            }
            value = std::move(*loaded);
            auto lock = std::scoped_lock{mutex};
            if (const auto found = nodes.find(*token);
                found != nodes.end() && found->second.generation == snapshot.generation) {
                cache_value(found->second, *value);
            }
        } else {
            auto lock = std::scoped_lock{mutex};
            if (const auto found = nodes.find(*token);
                found != nodes.end() && found->second.generation == snapshot.generation) {
                found->second.last_demand = ++demand_sequence;
            }
        }
        {
            auto lock = std::scoped_lock{mutex};
            ++counters.hits;
        }
        co_return graph_value{std::move(*value), snapshot.fingerprint, snapshot.tier};
    }

    if (has_recorded_value) {
        auto unchanged = true;
        for (const auto& dependency : snapshot.dependencies) {
            auto current = std::holds_alternative<query_key>(dependency.key)
                               ? co_await demand(std::get<query_key>(dependency.key), stack)
                               : co_await read_input(std::get<graph_input_key>(dependency.key));
            {
                auto lock = std::scoped_lock{mutex};
                ++counters.dependency_reads;
            }
            if (!current || !same_digest(current->fingerprint, dependency.fingerprint)) {
                unchanged = false;
                break;
            }
        }
        if (unchanged) {
            auto value = snapshot.value;
            if (!value) {
                auto loaded = co_await records.get(*snapshot.value_id);
                if (!loaded) {
                    co_return std::unexpected(loaded.error());
                }
                value = std::move(*loaded);
            }
            {
                auto lock = std::scoped_lock{mutex};
                const auto found = nodes.find(*token);
                if (found != nodes.end() && found->second.generation == snapshot.generation) {
                    found->second.stale = false;
                    cache_value(found->second, *value);
                }
                ++counters.hits;
            }
            co_return graph_value{std::move(*value), snapshot.fingerprint, snapshot.tier};
        }
    }

    const auto registered = registry.impl_->kinds.find(key.kind);
    if (registered == registry.impl_->kinds.end()) {
        co_return std::unexpected(
            failure(errc::not_found, "query kind is not registered", key.kind));
    }
    auto stop = std::make_shared<std::stop_source>();
    std::uint64_t generation{};
    {
        auto lock = std::scoped_lock{mutex};
        auto& current = nodes[*token];
        if (current.running) {
            current.running->request_stop();
        }
        current.key = key;
        generation = ++current.generation;
        current.running = stop;
        ++counters.executions;
    }

    auto context = query_context{std::make_unique<query_context::impl>()};
    context.impl_->owner = this;
    context.impl_->stack = std::move(stack);
    auto computed = co_await boost::capy::run(stop->get_token())(
        registered->second.compute(context, key.arguments));
    if (!computed) {
        auto lock = std::scoped_lock{mutex};
        const auto found = nodes.find(*token);
        if (stop->stop_requested() || found == nodes.end() ||
            found->second.generation != generation) {
            ++counters.discarded_computations;
        } else {
            found->second.running.reset();
        }
        co_return std::unexpected(computed.error());
    }
    auto computed_fingerprint = fingerprint("query-value", *computed);
    if (!computed_fingerprint) {
        co_return std::unexpected(computed_fingerprint.error());
    }
    auto stored_value = result<digest>{*computed_fingerprint};
    if (!transient) {
        stored_value = co_await records.put(*computed);
        if (!stored_value) {
            co_return std::unexpected(stored_value.error());
        }
    }

    auto discarded = false;
    {
        auto lock = std::scoped_lock{mutex};
        auto found = nodes.find(*token);
        if (stop->stop_requested() || found == nodes.end() ||
            found->second.generation != generation) {
            ++counters.discarded_computations;
            discarded = true;
        } else {
            auto before = found->second.dependencies;
            if (!same_digest(found->second.fingerprint, *computed_fingerprint)) {
                found->second.validated_fingerprint.reset();
            }
            found->second.fingerprint = *computed_fingerprint;
            found->second.dependencies = context.impl_->dependencies;
            found->second.tier = context.impl_->tier;
            found->second.value_id = *stored_value;
            cache_value(found->second, *computed);
            found->second.stale = false;
            found->second.running.reset();
            replace_reverse_edges(*token, before, found->second.dependencies);
        }
    }
    if (discarded) {
        co_return std::unexpected(
            failure(errc::unavailable, "query computation was cancelled", key.kind));
    }
    auto persisted = co_await persist_node(*token);
    if (!persisted) {
        co_return std::unexpected(persisted.error());
    }
    co_return graph_value{std::move(*computed), *computed_fingerprint, context.impl_->tier};
}

runtime::task<void> graph::impl::persist_node(std::string token) {
    if (transient) {
        co_return result<void>{};
    }
    node snapshot;
    {
        auto lock = std::scoped_lock{mutex};
        const auto found = nodes.find(token);
        if (found == nodes.end()) {
            co_return std::unexpected(failure(errc::not_found, "query disappeared", "query graph"));
        }
        snapshot = found->second;
    }
    auto encoded = cbor::encode(
        map({{"v", std::uint64_t{2}},
             {"kind", "query-node"},
             {"key", encode_query_key(snapshot.key)},
             {"fingerprint", to_string(snapshot.fingerprint)},
             {"durability", static_cast<std::uint64_t>(snapshot.tier)},
             {"dependencies", encode_dependencies(snapshot.dependencies)},
             {"value", to_string(*snapshot.value_id)},
             {"validated",
              snapshot.validated_fingerprint ? to_string(*snapshot.validated_fingerprint)
                                             : std::string{}}}));
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    auto record_id = co_await records.put(std::move(*encoded));
    if (!record_id) {
        co_return std::unexpected(record_id.error());
    }
    {
        auto lock = std::scoped_lock{mutex};
        const auto found = nodes.find(token);
        if (found != nodes.end() && found->second.generation == snapshot.generation) {
            found->second.record_id = *record_id;
        }
    }
    co_return co_await persist_root();
}

runtime::task<void> graph::impl::persist_root() {
    auto ids = cbor::value::array{};
    auto validation_records = cbor::value::array{};
    {
        auto lock = std::scoped_lock{mutex};
        for (const auto& [token, value] : nodes) {
            static_cast<void>(token);
            if (value.record_id) {
                ids.emplace_back(to_string(*value.record_id));
            }
        }
        for (const auto& id : validation_ids) {
            validation_records.emplace_back(to_string(id));
        }
    }
    auto encoded = cbor::encode(map({{"v", std::uint64_t{2}},
                                     {"kind", "query-graph"},
                                     {"nodes", cbor::value{std::move(ids)}},
                                     {"validations", cbor::value{std::move(validation_records)}}}));
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    auto root = co_await records.put(std::move(*encoded));
    if (!root) {
        co_return std::unexpected(root.error());
    }
    auto written =
        co_await runtime::offload([path = directory / "ROOT", text = to_string(*root)]() mutable {
            return write_root_file(path, std::move(text));
        });
    if (!written) {
        co_return std::unexpected(written.error());
    }
    if (!*written) {
        co_return std::unexpected(written->error());
    }
    co_return result<void>{};
}

runtime::task<void> graph::impl::load() {
    auto root_text =
        co_await runtime::offload([path = directory / "ROOT"] { return read_text_file(path); });
    if (!root_text) {
        co_return std::unexpected(root_text.error());
    }
    if (!*root_text) {
        if (root_text->error().code == errc::not_found) {
            co_return result<void>{};
        }
        co_return std::unexpected(root_text->error());
    }
    auto root_id = parse_digest(**root_text);
    if (!root_id) {
        co_return std::unexpected(root_id.error());
    }
    auto root_bytes = co_await records.get(*root_id);
    if (!root_bytes) {
        co_return std::unexpected(root_bytes.error());
    }
    auto decoded = cbor::decode(*root_bytes);
    if (!decoded) {
        co_return std::unexpected(decoded.error());
    }
    const auto* kind_value = find(*decoded, "kind");
    const auto* version_value = find(*decoded, "v");
    const auto* nodes_value = find(*decoded, "nodes");
    const auto* validations_value = find(*decoded, "validations");
    const auto* kind = kind_value ? std::get_if<std::string>(&kind_value->data) : nullptr;
    const auto* version =
        version_value ? std::get_if<std::uint64_t>(&version_value->data) : nullptr;
    const auto* node_ids =
        nodes_value ? std::get_if<cbor::value::array>(&nodes_value->data) : nullptr;
    const auto* validation_record_ids =
        validations_value ? std::get_if<cbor::value::array>(&validations_value->data) : nullptr;
    if (!kind || *kind != "query-graph" || !version || (*version != 1 && *version != 2) ||
        !node_ids || (*version == 2 && !validation_record_ids)) {
        co_return std::unexpected(
            failure(errc::corrupt, "invalid query graph root", "query graph"));
    }
    for (const auto& item : *node_ids) {
        const auto* id_text = std::get_if<std::string>(&item.data);
        if (!id_text) {
            co_return std::unexpected(
                failure(errc::corrupt, "invalid query node id", "query graph"));
        }
        auto id = parse_digest(*id_text);
        if (!id) {
            co_return std::unexpected(id.error());
        }
        auto node_bytes = co_await records.get(*id);
        if (!node_bytes) {
            co_return std::unexpected(node_bytes.error());
        }
        auto node_value = cbor::decode(*node_bytes);
        if (!node_value) {
            co_return std::unexpected(node_value.error());
        }
        const auto* node_kind_value = find(*node_value, "kind");
        const auto* node_version_value = find(*node_value, "v");
        const auto* key_value = find(*node_value, "key");
        const auto* fingerprint_value = find(*node_value, "fingerprint");
        const auto* durability_value = find(*node_value, "durability");
        const auto* dependencies_value = find(*node_value, "dependencies");
        const auto* value_value = find(*node_value, "value");
        const auto* validated_value = find(*node_value, "validated");
        const auto* node_kind =
            node_kind_value ? std::get_if<std::string>(&node_kind_value->data) : nullptr;
        const auto* node_version =
            node_version_value ? std::get_if<std::uint64_t>(&node_version_value->data) : nullptr;
        const auto* fingerprint_text =
            fingerprint_value ? std::get_if<std::string>(&fingerprint_value->data) : nullptr;
        const auto* tier =
            durability_value ? std::get_if<std::uint64_t>(&durability_value->data) : nullptr;
        const auto* dependencies = dependencies_value
                                       ? std::get_if<cbor::value::array>(&dependencies_value->data)
                                       : nullptr;
        const auto* value_text =
            value_value ? std::get_if<std::string>(&value_value->data) : nullptr;
        const auto* validated_text =
            validated_value ? std::get_if<std::string>(&validated_value->data) : nullptr;
        if (!node_kind || *node_kind != "query-node" || !node_version ||
            (*node_version != 1 && *node_version != 2) || !key_value || !fingerprint_text ||
            !tier || *tier > 3 || !dependencies || !value_text) {
            co_return std::unexpected(failure(errc::corrupt, "invalid query node", "query graph"));
        }
        auto key = decode_query_key(*key_value);
        auto fingerprint_id = parse_digest(*fingerprint_text);
        auto value_id = parse_digest(*value_text);
        if (!key || !fingerprint_id || !value_id) {
            co_return std::unexpected(
                failure(errc::corrupt, "invalid query node fields", "query graph"));
        }
        auto recorded_dependencies = decode_dependencies(*dependencies_value);
        if (!recorded_dependencies) {
            co_return std::unexpected(recorded_dependencies.error());
        }
        auto validated_fingerprint = std::optional<digest>{};
        if (validated_text && !validated_text->empty()) {
            auto parsed = parse_digest(*validated_text);
            if (!parsed) {
                co_return std::unexpected(parsed.error());
            }
            validated_fingerprint = *parsed;
        }
        auto token = query_token(*key);
        if (!token) {
            co_return std::unexpected(token.error());
        }
        auto loaded = node{.key = std::move(*key),
                           .fingerprint = *fingerprint_id,
                           .dependencies = std::move(*recorded_dependencies),
                           .tier = static_cast<durability>(*tier),
                           .value_id = *value_id,
                           .record_id = *id,
                           .validated_fingerprint = validated_fingerprint,
                           .stale = true};
        replace_reverse_edges(*token, {}, loaded.dependencies);
        nodes[*token] = std::move(loaded);
    }
    if (validation_record_ids) {
        for (const auto& item : *validation_record_ids) {
            const auto* id_text = std::get_if<std::string>(&item.data);
            if (!id_text) {
                co_return std::unexpected(
                    failure(errc::corrupt, "invalid validation record id", "query graph"));
            }
            auto id = parse_digest(*id_text);
            if (!id) {
                co_return std::unexpected(id.error());
            }
            auto record_bytes = co_await records.get(*id);
            if (!record_bytes) {
                co_return std::unexpected(record_bytes.error());
            }
            auto record_value = cbor::decode(*record_bytes);
            if (!record_value) {
                co_return std::unexpected(record_value.error());
            }
            auto record = decode_validation_record(*record_value, *id);
            if (!record) {
                co_return std::unexpected(record.error());
            }
            validation_ids.push_back(*id);
            validations.push_back(std::move(*record));
        }
    }
    co_return result<void>{};
}

graph::graph() noexcept = default;
graph::~graph() = default;
graph::graph(graph&&) noexcept = default;
graph& graph::operator=(graph&&) noexcept = default;

runtime::task<graph>
graph::open(std::filesystem::path directory, query_registry registry, graph_options options) {
    auto prepared = co_await runtime::offload([directory] {
        auto code = std::error_code{};
        std::filesystem::create_directories(directory, code);
        if (code) {
            return result<void>{
                std::unexpected(failure(errc::io, code.message(), directory.string()))};
        }
        return result<void>{};
    });
    if (!prepared) {
        co_return std::unexpected(prepared.error());
    }
    if (!*prepared) {
        co_return std::unexpected(prepared->error());
    }
    auto records = co_await store::open(directory / "store");
    if (!records) {
        co_return std::unexpected(records.error());
    }
    auto output = graph{};
    output.impl_ = std::make_unique<impl>();
    output.impl_->directory = std::move(directory);
    output.impl_->options = std::move(options);
    output.impl_->registry = std::move(registry);
    output.impl_->records = std::move(*records);
    auto loaded = co_await output.impl_->load();
    if (!loaded) {
        co_return std::unexpected(loaded.error());
    }
    co_return std::move(output);
}

runtime::task<graph_value> graph::demand(query_key key) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "query graph is not open"));
    }
    co_return co_await impl_->demand(std::move(key), {});
}

runtime::task<void>
graph::set_input(graph_input_key key, std::vector<std::byte> value, durability tier) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "query graph is not open"));
    }
    auto token = key_token(graph_dependency_key{key});
    if (!token) {
        co_return std::unexpected(token.error());
    }
    auto value_fingerprint = fingerprint("query-input", value);
    if (!value_fingerprint) {
        co_return std::unexpected(value_fingerprint.error());
    }
    {
        auto lock = std::scoped_lock{impl_->mutex};
        const auto found = impl_->inputs.find(*token);
        if (found != impl_->inputs.end() && found->second.value == value &&
            found->second.tier == tier) {
            co_return result<void>{};
        }
        auto changed = found != impl_->inputs.end();
        auto invalidation_tier = tier;
        if (found != impl_->inputs.end()) {
            invalidation_tier = static_cast<durability>(std::max(
                static_cast<std::uint8_t>(found->second.tier), static_cast<std::uint8_t>(tier)));
        }
        if (found == impl_->inputs.end()) {
            const auto dependents = impl_->reverse.find(*token);
            if (dependents != impl_->reverse.end()) {
                for (const auto& dependent : dependents->second) {
                    const auto node = impl_->nodes.find(dependent);
                    if (node == impl_->nodes.end()) {
                        continue;
                    }
                    for (const auto& dependency : node->second.dependencies) {
                        auto dependency_token = key_token(dependency.key);
                        if (dependency_token && *dependency_token == *token &&
                            !same_digest(dependency.fingerprint, *value_fingerprint)) {
                            changed = true;
                        }
                    }
                }
            }
        }
        impl_->inputs[*token] = impl::input_override{key, std::move(value), tier};
        if (changed) {
            auto invalidated = impl_->invalidate(*token, invalidation_tier, false);
            if (!invalidated) {
                co_return std::unexpected(invalidated.error());
            }
        }
    }
    co_return result<void>{};
}

runtime::task<void> graph::clear_input(graph_input_key key, durability tier) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "query graph is not open"));
    }
    auto token = key_token(graph_dependency_key{key});
    if (!token) {
        co_return std::unexpected(token.error());
    }
    {
        auto lock = std::scoped_lock{impl_->mutex};
        impl_->inputs.erase(*token);
        auto invalidated = impl_->invalidate(*token, tier, false);
        if (!invalidated) {
            co_return std::unexpected(invalidated.error());
        }
    }
    co_return result<void>{};
}

result<std::uint64_t> graph::mark_changed(graph_input_key key, durability tier) {
    if (!impl_) {
        return std::unexpected(failure(errc::invalid_argument, "query graph is not open"));
    }
    auto token = key_token(graph_dependency_key{std::move(key)});
    if (!token) {
        return std::unexpected(token.error());
    }
    auto lock = std::scoped_lock{impl_->mutex};
    return impl_->invalidate(*token, tier, false);
}

result<std::uint64_t> graph::mark_changed(query_key key) {
    if (!impl_) {
        return std::unexpected(failure(errc::invalid_argument, "query graph is not open"));
    }
    auto token = query_token(key);
    if (!token) {
        return std::unexpected(token.error());
    }
    auto lock = std::scoped_lock{impl_->mutex};
    auto invalidated = impl_->invalidate(*token, durability::high, true);
    if (!invalidated) {
        return std::unexpected(invalidated.error());
    }
    const auto found = impl_->nodes.find(*token);
    if (found != impl_->nodes.end()) {
        impl_->discard_value(found->second);
        found->second.value_id.reset();
        found->second.record_id.reset();
    }
    return invalidated;
}

runtime::task<std::optional<recorded_query>> graph::recorded(query_key key) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "query graph is not open"));
    }
    auto token = query_token(key);
    if (!token) {
        co_return std::unexpected(token.error());
    }
    auto snapshot = impl::node{};
    {
        auto lock = std::scoped_lock{impl_->mutex};
        const auto found = impl_->nodes.find(*token);
        if (found == impl_->nodes.end()) {
            co_return std::optional<recorded_query>{};
        }
        snapshot = found->second;
    }
    auto value = snapshot.value;
    if (!value && !snapshot.value_id) {
        co_return std::optional<recorded_query>{};
    }
    if (!value) {
        auto loaded = co_await impl_->records.get(*snapshot.value_id);
        if (!loaded) {
            co_return std::unexpected(loaded.error());
        }
        value = std::move(*loaded);
    }
    co_return std::optional<recorded_query>{
        recorded_query{snapshot.key,
                       snapshot.fingerprint,
                       snapshot.dependencies,
                       graph_value{std::move(*value), snapshot.fingerprint, snapshot.tier}}};
}

runtime::task<graph_validation_report> graph::validate(graph_validation_options options) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "query graph is not open"));
    }
    if (options.rate == 0) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "graph validation rate must be positive"));
    }
    struct candidate {
        std::string token;
        impl::node value;
    };
    auto candidates = std::vector<candidate>{};
    auto overrides = std::vector<impl::input_override>{};
    {
        auto lock = std::scoped_lock{impl_->mutex};
        auto backlog_index = std::uint64_t{};
        for (const auto& [token, value] : impl_->nodes) {
            if (!value.value_id) {
                continue;
            }
            if (!options.all && value.validated_fingerprint &&
                same_digest(*value.validated_fingerprint, value.fingerprint)) {
                continue;
            }
            const auto selected = options.all || backlog_index % options.rate == 0;
            ++backlog_index;
            if (!selected || (options.limit != 0 && candidates.size() >= options.limit)) {
                continue;
            }
            candidates.push_back({token, value});
        }
        for (const auto& [token, value] : impl_->inputs) {
            static_cast<void>(token);
            overrides.push_back(value);
        }
    }
    auto report = graph_validation_report{};
    report.considered = candidates.size();
    const auto started = std::chrono::steady_clock::now();
    auto clean_values = std::vector<graph_value>{};
    if (!candidates.empty()) {
        auto allocated = co_await runtime::offload([parent = impl_->directory / "validation"] {
            return create_validation_directory(parent);
        });
        if (!allocated) {
            co_return std::unexpected(allocated.error());
        }
        if (!*allocated) {
            co_return std::unexpected(allocated->error());
        }
        const auto sandbox = std::move(**allocated);
        auto clean = co_await graph::open(
            sandbox, impl_->registry, {.working_tree = impl_->options.working_tree});
        if (!clean) {
            static_cast<void>(co_await runtime::offload(
                [sandbox] { return remove_validation_directory(sandbox); }));
            co_return std::unexpected(clean.error());
        }
        clean->impl_->transient = true;
        for (const auto& input : overrides) {
            auto set = co_await clean->set_input(input.key, input.value, input.tier);
            if (!set) {
                *clean = graph{};
                static_cast<void>(co_await runtime::offload(
                    [sandbox] { return remove_validation_directory(sandbox); }));
                co_return std::unexpected(set.error());
            }
        }
        clean_values.reserve(candidates.size());
        for (const auto& candidate : candidates) {
            auto clean_value = co_await clean->demand(candidate.value.key);
            if (!clean_value) {
                *clean = graph{};
                static_cast<void>(co_await runtime::offload(
                    [sandbox] { return remove_validation_directory(sandbox); }));
                co_return std::unexpected(clean_value.error());
            }
            clean_values.push_back(std::move(*clean_value));
        }
        *clean = graph{};
        auto removed =
            co_await runtime::offload([sandbox] { return remove_validation_directory(sandbox); });
        if (!removed) {
            co_return std::unexpected(removed.error());
        }
        if (!*removed) {
            co_return std::unexpected(removed->error());
        }
    }
    for (auto index = std::size_t{}; index != candidates.size(); ++index) {
        auto& candidate = candidates[index];
        const auto& clean_value = clean_values[index];
        auto record = graph_validation_record{
            .query = candidate.value.key,
            .incremental = candidate.value.fingerprint,
            .clean = clean_value.fingerprint,
            .agreement = same_digest(candidate.value.fingerprint, clean_value.fingerprint),
            .at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now())};
        {
            auto lock = std::scoped_lock{impl_->mutex};
            record.inputs = impl_->recorded_inputs(candidate.token);
        }
        auto encoded = encode_validation_record(record);
        if (!encoded) {
            co_return std::unexpected(encoded.error());
        }
        auto stored = co_await impl_->records.put(std::move(*encoded));
        if (!stored) {
            co_return std::unexpected(stored.error());
        }
        record.id = *stored;
        {
            auto lock = std::scoped_lock{impl_->mutex};
            const auto current = impl_->nodes.find(candidate.token);
            const auto current_matches =
                current != impl_->nodes.end() && current->second.value_id &&
                same_digest(current->second.fingerprint, candidate.value.fingerprint);
            if (!current_matches) {
                continue;
            }
            impl_->validation_ids.push_back(*stored);
            impl_->validations.push_back(record);
            if (record.agreement) {
                current->second.validated_fingerprint = candidate.value.fingerprint;
            } else {
                auto invalidated = impl_->invalidate(candidate.token, durability::high, true);
                if (!invalidated) {
                    co_return std::unexpected(invalidated.error());
                }
                impl_->discard_value(current->second);
                current->second.value_id.reset();
                current->second.record_id.reset();
                current->second.validated_fingerprint.reset();
            }
        }
        if (record.agreement) {
            auto persisted = co_await impl_->persist_node(candidate.token);
            if (!persisted) {
                co_return std::unexpected(persisted.error());
            }
        } else {
            auto persisted = co_await impl_->persist_root();
            if (!persisted) {
                co_return std::unexpected(persisted.error());
            }
        }
        ++report.validated;
        if (!record.agreement) {
            ++report.divergences;
            report.last_divergence = record;
        }
        report.records.push_back(std::move(record));
    }
    const auto status = validation_status();
    report.backlog = status.backlog;
    if (!report.last_divergence) {
        report.last_divergence = status.last_divergence;
    }
    report.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    co_return report;
}

graph_validation_status graph::validation_status() const {
    if (!impl_) {
        return {};
    }
    auto lock = std::scoped_lock{impl_->mutex};
    auto output = graph_validation_status{};
    output.records = impl_->validations.size();
    for (const auto& [token, value] : impl_->nodes) {
        static_cast<void>(token);
        if (!value.value_id) {
            continue;
        }
        ++output.eligible;
        if (value.validated_fingerprint &&
            same_digest(*value.validated_fingerprint, value.fingerprint)) {
            ++output.validated;
        }
    }
    output.backlog = output.eligible - output.validated;
    for (auto iterator = impl_->validations.rbegin(); iterator != impl_->validations.rend();
         ++iterator) {
        if (!iterator->agreement) {
            output.last_divergence = *iterator;
            break;
        }
    }
    return output;
}

runtime::task<std::vector<graph_validation_record>> graph::validation_records() {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "query graph is not open"));
    }
    auto lock = std::scoped_lock{impl_->mutex};
    co_return impl_->validations;
}

runtime::task<void> graph::inject_stale_value_for_testing(query_key key,
                                                          std::vector<std::byte> value) {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "query graph is not open"));
    }
    auto token = query_token(key);
    if (!token) {
        co_return std::unexpected(token.error());
    }
    auto value_fingerprint = fingerprint("query-value", value);
    if (!value_fingerprint) {
        co_return std::unexpected(value_fingerprint.error());
    }
    auto stored_value = co_await impl_->records.put(value);
    if (!stored_value) {
        co_return std::unexpected(stored_value.error());
    }
    {
        auto lock = std::scoped_lock{impl_->mutex};
        const auto found = impl_->nodes.find(*token);
        if (found == impl_->nodes.end() || !found->second.value_id) {
            co_return std::unexpected(
                failure(errc::not_found, "query has no cached value", key.kind));
        }
        found->second.fingerprint = *value_fingerprint;
        found->second.value_id = *stored_value;
        found->second.validated_fingerprint.reset();
        found->second.stale = false;
        impl_->cache_value(found->second, std::move(value));
    }
    co_return co_await impl_->persist_node(*token);
}

runtime::task<void> graph::flush() {
    if (!impl_) {
        co_return std::unexpected(failure(errc::invalid_argument, "query graph is not open"));
    }
    co_return co_await impl_->persist_root();
}

graph_statistics graph::statistics() const {
    if (!impl_) {
        return {};
    }
    auto lock = std::scoped_lock{impl_->mutex};
    auto output = impl_->counters;
    output.keys = impl_->nodes.size();
    output.resident_values = static_cast<std::uint64_t>(std::ranges::count_if(
        impl_->nodes, [](const auto& item) { return item.second.value.has_value(); }));
    output.resident_value_bytes = impl_->resident_value_bytes;
    output.memory_ceiling = impl_->options.memory_ceiling;
    return output;
}

} // namespace liquid
