#include "liquid/admission.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <boost/capy/buffers.hpp>
#include <boost/capy/buffers/buffer_copy.hpp>
#include <boost/capy/buffers/vector_dynamic_buffer.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/http.hpp>

#include "admission/internal.hpp"
#include "formats/json.hpp"
#include "http1/server.hpp"

namespace liquid {
namespace {

namespace json = formats::json;

auto admission_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto field(const json::value& value, std::string_view name) -> const json::value* {
    const auto* object = std::get_if<json::value::object>(&value.data);
    if (!object) {
        return nullptr;
    }
    const auto found = std::ranges::find(
        *object, name, [](const auto& entry) { return std::string_view{entry.first}; });
    return found == object->end() ? nullptr : &found->second;
}

auto string_field(const json::value& value, std::string_view name) -> const std::string* {
    const auto* child = field(value, name);
    return child ? std::get_if<std::string>(&child->data) : nullptr;
}

auto object_field(const json::value& value, std::string_view name) -> const json::value* {
    const auto* child = field(value, name);
    return child && std::holds_alternative<json::value::object>(child->data) ? child : nullptr;
}

auto number(std::uint64_t value) -> json::value {
    return json::value{json::number{std::to_string(value)}};
}

auto text(std::span<const std::byte> bytes) -> std::string {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

auto read_file(std::filesystem::path path) -> result<std::string> {
    auto input = std::ifstream{path, std::ios::binary};
    if (!input) {
        return std::unexpected(
            admission_error(errc::io, "cannot open admission policy", path.string()));
    }
    auto output =
        std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    if (input.bad()) {
        return std::unexpected(
            admission_error(errc::io, "cannot read admission policy", path.string()));
    }
    return output;
}

auto glob_matches(std::string_view pattern, std::string_view value) -> bool {
    auto previous = std::vector<bool>(value.size() + 1);
    previous[0] = true;
    for (const auto pattern_character : pattern) {
        auto current = std::vector<bool>(value.size() + 1);
        if (pattern_character == '*') {
            current[0] = previous[0];
            for (auto index = std::size_t{1}; index <= value.size(); ++index) {
                current[index] = previous[index] || current[index - 1];
            }
        } else {
            for (auto index = std::size_t{1}; index <= value.size(); ++index) {
                current[index] = previous[index - 1] && (pattern_character == '?' ||
                                                         pattern_character == value[index - 1]);
            }
        }
        previous = std::move(current);
    }
    return previous[value.size()];
}

auto percent_decode(std::string_view input) -> result<std::string> {
    const auto hex = [](char value) {
        if (value >= '0' && value <= '9') {
            return value - '0';
        }
        if (value >= 'a' && value <= 'f') {
            return value - 'a' + 10;
        }
        if (value >= 'A' && value <= 'F') {
            return value - 'A' + 10;
        }
        return -1;
    };
    auto output = std::string{};
    for (auto offset = std::size_t{}; offset < input.size(); ++offset) {
        if (input[offset] != '%') {
            output.push_back(input[offset] == '+' ? ' ' : input[offset]);
            continue;
        }
        if (input.size() - offset < 3) {
            return std::unexpected(admission_error(errc::invalid_argument, "truncated URL escape"));
        }
        const auto high = hex(input[offset + 1]);
        const auto low = hex(input[offset + 2]);
        if (high < 0 || low < 0) {
            return std::unexpected(admission_error(errc::invalid_argument, "invalid URL escape"));
        }
        output.push_back(static_cast<char>((high << 4) | low));
        offset += 2;
    }
    return output;
}

auto query_parameter(std::string_view query, std::string_view name) -> result<std::string> {
    while (!query.empty()) {
        const auto separator = query.find('&');
        const auto item = query.substr(0, separator);
        const auto equals = item.find('=');
        if (equals != std::string_view::npos && item.substr(0, equals) == name) {
            return percent_decode(item.substr(equals + 1));
        }
        if (separator == std::string_view::npos) {
            break;
        }
        query.remove_prefix(separator + 1);
    }
    return std::unexpected(
        admission_error(errc::invalid_argument, "missing query parameter", std::string{name}));
}

auto optional_query_parameter(std::string_view query, std::string_view name)
    -> std::optional<std::string> {
    auto value = query_parameter(query, name);
    if (!value) {
        return std::nullopt;
    }
    return std::move(*value);
}

auto unsigned_value(std::string_view value, std::string_view context) -> result<std::uint64_t> {
    auto output = std::uint64_t{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), output);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        return std::unexpected(admission_error(
            errc::invalid_argument, "expected an unsigned integer", std::string{context}));
    }
    return output;
}

struct policy_document {
    std::string name;
    std::string contents;
};

struct policy_documents {
    std::string fingerprint;
    std::vector<policy_document> values;
};

auto append_fingerprint(std::string& output, std::string_view value) -> void {
    output += std::to_string(value.size());
    output.push_back(':');
    output.append(value);
}

auto read_policy_documents(const admission_options& options) -> result<policy_documents> {
    auto output = policy_documents{};
    if (!options.policies.empty()) {
        auto code = std::error_code{};
        if (!std::filesystem::is_directory(options.policies, code) || code) {
            return std::unexpected(admission_error(errc::invalid_argument,
                                                   "admission policies path is not a directory",
                                                   options.policies.string()));
        }
        for (const auto& entry : std::filesystem::directory_iterator{options.policies}) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json") {
                continue;
            }
            const auto name = entry.path().stem().string();
            if (name.empty()) {
                return std::unexpected(admission_error(errc::invalid_argument,
                                                       "admission policy name is empty",
                                                       entry.path().string()));
            }
            auto contents = read_file(entry.path());
            if (!contents) {
                return std::unexpected(contents.error());
            }
            output.values.push_back({.name = name, .contents = std::move(*contents)});
        }
        std::ranges::sort(output.values, {}, &policy_document::name);
        if (output.values.empty()) {
            return std::unexpected(admission_error(errc::invalid_argument,
                                                   "admission policy directory has no JSON files",
                                                   options.policies.string()));
        }
        const auto duplicate =
            std::ranges::adjacent_find(output.values, {}, &policy_document::name);
        if (duplicate != output.values.end()) {
            return std::unexpected(admission_error(
                errc::invalid_argument, "duplicate admission policy name", duplicate->name));
        }
    } else {
        auto contents = read_file(options.policy);
        if (!contents) {
            return std::unexpected(contents.error());
        }
        output.values.push_back({.name = "default", .contents = std::move(*contents)});
    }
    for (const auto& document : output.values) {
        append_fingerprint(output.fingerprint, document.name);
        append_fingerprint(output.fingerprint, document.contents);
    }
    return output;
}

class unavailable_source final : public attestation_source {
public:
    explicit unavailable_source(error cause) : cause_(std::move(cause)) {}

    runtime::task<std::vector<attestation>> by_subject(digest subject) override {
        static_cast<void>(subject);
        co_return std::unexpected(cause_);
    }

private:
    error cause_;
};

struct service_snapshot {
    admission_policy policy;
    std::shared_ptr<attestation_source> source;
    std::shared_ptr<log_verifier> log;
    bool source_ready{};
};

struct policy_status {
    std::vector<std::string> names;
    std::optional<error> reload_error;
};

class service_state {
public:
    explicit service_state(admission_options options) : options_(std::move(options)) {}

    runtime::task<bool> reload(bool force) {
        auto loaded = co_await runtime::offload(
            [options = options_] { return read_policy_documents(options); });
        if (!loaded) {
            set_reload_error(loaded.error());
            co_return std::unexpected(loaded.error());
        }
        if (!*loaded) {
            set_reload_error(loaded->error());
            co_return std::unexpected(loaded->error());
        }
        auto documents = std::move(**loaded);
        {
            auto lock = std::scoped_lock{mutex_};
            if (!force && documents.fingerprint == fingerprint_) {
                rejected_fingerprint_.clear();
                reload_error_.reset();
                co_return false;
            }
            if (!force && documents.fingerprint == rejected_fingerprint_) {
                co_return false;
            }
        }
        auto policies = std::map<std::string, service_snapshot>{};
        for (auto& document : documents.values) {
            auto policy = decode_admission_policy(document.contents);
            if (!policy) {
                auto failure = policy.error();
                failure.context = document.name + ".json: " + failure.context;
                set_reload_error(failure, documents.fingerprint);
                co_return std::unexpected(std::move(failure));
            }
            if (!options_.store.empty()) {
                policy->attestation_source = options_.store.string();
            } else if (!options_.attestation_source.empty()) {
                policy->attestation_source = options_.attestation_source;
            }
            auto source_ready = false;
            auto selected_source = std::shared_ptr<attestation_source>{};
            if (policy->mode == admission_mode::off) {
                selected_source = std::make_shared<unavailable_source>(admission_error(
                    errc::unavailable, "audit-only policy does not consult attestations"));
                source_ready = true;
            } else {
                auto source = co_await admission_detail::open_source(policy->attestation_source);
                if (source) {
                    selected_source = *source;
                    auto probe = co_await selected_source->by_subject(
                        digest{.alg = algorithm::sha256, .bytes = {}});
                    source_ready = probe.has_value();
                } else {
                    selected_source = std::make_shared<unavailable_source>(source.error());
                }
            }
            auto selected_log =
                admission_detail::make_log_verifier(policy->log, options_.log_cache);
            policies.emplace(document.name,
                             service_snapshot{.policy = std::move(*policy),
                                              .source = std::move(selected_source),
                                              .log = std::move(selected_log),
                                              .source_ready = source_ready});
        }
        {
            auto lock = std::scoped_lock{mutex_};
            policies_ = std::move(policies);
            fingerprint_ = std::move(documents.fingerprint);
            rejected_fingerprint_.clear();
            reload_error_.reset();
        }
        co_return true;
    }

    auto snapshot(std::string_view name) const -> result<service_snapshot> {
        auto lock = std::scoped_lock{mutex_};
        const auto found = policies_.find(std::string{name});
        if (found == policies_.end()) {
            return std::unexpected(admission_error(
                errc::not_found, "admission policy set was not found", std::string{name}));
        }
        return found->second;
    }

    auto ready() const -> bool {
        auto lock = std::scoped_lock{mutex_};
        return !policies_.empty() && std::ranges::all_of(policies_, [](const auto& entry) {
            return entry.second.source_ready;
        });
    }

    auto status() const -> policy_status {
        auto lock = std::scoped_lock{mutex_};
        auto output = policy_status{.reload_error = reload_error_};
        output.names.reserve(policies_.size());
        for (const auto& [name, policy] : policies_) {
            static_cast<void>(policy);
            output.names.push_back(name);
        }
        return output;
    }

    auto publish(admission_decision decision) -> admission_decision {
        auto lock = std::scoped_lock{decision_mutex_};
        decision.sequence = ++next_sequence_;
        decisions_.push_back(decision);
        while (decisions_.size() > options_.decision_replay_capacity) {
            decisions_.pop_front();
        }
        return decision;
    }

    auto decisions_after(std::uint64_t sequence) const -> std::vector<admission_decision> {
        auto lock = std::scoped_lock{decision_mutex_};
        auto output = std::vector<admission_decision>{};
        for (const auto& decision : decisions_) {
            if (decision.sequence > sequence) {
                output.push_back(decision);
            }
        }
        return output;
    }

    auto record_review(bool allowed, std::string reason, double seconds) -> void {
        auto lock = std::scoped_lock{metrics_mutex_};
        ++reviews_[{allowed ? "allowed" : "denied", std::move(reason)}];
        review_seconds_sum_ += seconds;
        ++review_seconds_count_;
        for (auto index = std::size_t{}; index < buckets_.size(); ++index) {
            if (seconds <= bucket_limits_[index]) {
                ++buckets_[index];
            }
        }
    }

    auto record_image(bool allowed) -> void {
        auto lock = std::scoped_lock{metrics_mutex_};
        ++images_[allowed ? "allowed" : "denied"];
    }

    auto metrics() const -> std::string {
        auto lock = std::scoped_lock{metrics_mutex_};
        auto output = std::string{"# TYPE liquid_admission_reviews_total counter\n"};
        for (const auto& [labels, count] : reviews_) {
            output +=
                std::format("liquid_admission_reviews_total{{decision=\"{}\",reason=\"{}\"}} {}\n",
                            labels.first,
                            labels.second,
                            count);
        }
        output += "# TYPE liquid_admission_images_total counter\n";
        for (const auto& [decision, count] : images_) {
            output += std::format(
                "liquid_admission_images_total{{decision=\"{}\"}} {}\n", decision, count);
        }
        output += "# TYPE liquid_admission_review_seconds histogram\n";
        for (auto index = std::size_t{}; index < buckets_.size(); ++index) {
            output += std::format("liquid_admission_review_seconds_bucket{{le=\"{}\"}} {}\n",
                                  bucket_names_[index],
                                  buckets_[index]);
        }
        output += std::format("liquid_admission_review_seconds_bucket{{le=\"+Inf\"}} {}\n",
                              review_seconds_count_);
        output += std::format("liquid_admission_review_seconds_sum {}\n", review_seconds_sum_);
        output += std::format("liquid_admission_review_seconds_count {}\n", review_seconds_count_);
        return output;
    }

private:
    auto set_reload_error(error failure, std::string rejected = {}) -> void {
        auto lock = std::scoped_lock{mutex_};
        reload_error_ = std::move(failure);
        rejected_fingerprint_ = std::move(rejected);
    }

    admission_options options_;
    mutable std::mutex mutex_;
    std::map<std::string, service_snapshot> policies_;
    std::string fingerprint_;
    std::string rejected_fingerprint_;
    std::optional<error> reload_error_;

    mutable std::mutex decision_mutex_;
    std::deque<admission_decision> decisions_;
    std::uint64_t next_sequence_{};

    static constexpr auto bucket_limits_ = std::array{0.01, 0.1, 1.0};
    static constexpr auto bucket_names_ = std::array{"0.01", "0.1", "1"};
    mutable std::mutex metrics_mutex_;
    std::map<std::pair<std::string, std::string>, std::uint64_t> reviews_;
    std::map<std::string, std::uint64_t> images_;
    std::array<std::uint64_t, bucket_limits_.size()> buckets_{};
    double review_seconds_sum_{};
    std::uint64_t review_seconds_count_{};
};

auto images_from(const json::value& review) -> result<std::vector<std::string>> {
    const auto* request = object_field(review, "request");
    const auto* object = request ? object_field(*request, "object") : nullptr;
    const auto* spec = object ? object_field(*object, "spec") : nullptr;
    const auto* kind = object ? string_field(*object, "kind") : nullptr;
    if (!request || !object || !spec || !kind || *kind != "Pod") {
        return std::unexpected(
            admission_error(errc::invalid_argument, "AdmissionReview request Pod is incomplete"));
    }
    auto output = std::vector<std::string>{};
    for (const auto name : {"containers", "initContainers", "ephemeralContainers"}) {
        const auto* child = field(*spec, name);
        if (!child) {
            continue;
        }
        const auto* array = std::get_if<json::value::array>(&child->data);
        if (!array) {
            return std::unexpected(admission_error(
                errc::invalid_argument, "Pod container field must be an array", name));
        }
        for (const auto& container : *array) {
            const auto* image = string_field(container, "image");
            if (!image) {
                return std::unexpected(admission_error(
                    errc::invalid_argument, "Pod container image is missing", name));
            }
            output.push_back(*image);
        }
    }
    return output;
}

auto namespace_from(const json::value& review) -> std::string {
    const auto* request = object_field(review, "request");
    if (!request) {
        return {};
    }
    if (const auto* value = string_field(*request, "namespace")) {
        return *value;
    }
    const auto* object = object_field(*request, "object");
    const auto* metadata = object ? object_field(*object, "metadata") : nullptr;
    const auto* value = metadata ? string_field(*metadata, "namespace") : nullptr;
    return value ? *value : std::string{};
}

auto route_parameter(const boost::http::route_params& params, std::string_view name)
    -> std::string_view {
    for (const auto& [key, value] : params.params) {
        if (key == name) {
            return value;
        }
    }
    return {};
}

auto subject_from_image(std::string_view image) -> std::optional<digest> {
    const auto separator = image.rfind('@');
    if (separator == std::string_view::npos || separator + 1 == image.size()) {
        return std::nullopt;
    }
    auto subject = parse_digest(image.substr(separator + 1));
    if (!subject || subject->alg != algorithm::sha256) {
        return std::nullopt;
    }
    return *subject;
}

auto decision_json_value(const admission_decision& decision) -> result<json::value> {
    auto encoded = encode_admission_decision_json(decision);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return json::parse(*encoded);
}

auto review_response(std::string uid, std::span<const admission_decision> decisions)
    -> result<std::string> {
    const auto denied =
        std::ranges::find_if(decisions, [](const auto& decision) { return !decision.allowed; });
    const auto allowed = denied == decisions.end();
    auto records = json::value::array{};
    records.reserve(decisions.size());
    for (const auto& decision : decisions) {
        auto record = decision_json_value(decision);
        if (!record) {
            return std::unexpected(record.error());
        }
        records.push_back(std::move(*record));
    }
    auto message = std::string{"admitted"};
    if (!allowed) {
        message = denied->image + ": " + denied->reason;
    }
    auto response = json::value::object{
        {"uid", json::value{std::move(uid)}},
        {"allowed", json::value{allowed}},
        {"status",
         json::value{json::value::object{
             {"code", number(allowed ? 200 : 403)},
             {"message", json::value{std::move(message)}},
             {"decisions", json::value{std::move(records)}},
         }}},
    };
    return json::write(json::value{json::value::object{
        {"apiVersion", json::value{std::string{"admission.k8s.io/v1"}}},
        {"kind", json::value{std::string{"AdmissionReview"}}},
        {"response", json::value{std::move(response)}},
    }});
}

auto review_pod(std::shared_ptr<service_state> state,
                std::string policy_name,
                const json::value& document) -> runtime::task<std::vector<admission_decision>> {
    auto images = images_from(document);
    if (!images) {
        co_return std::unexpected(images.error());
    }
    auto snapshot = state->snapshot(policy_name);
    if (!snapshot) {
        co_return std::unexpected(snapshot.error());
    }
    const auto pod_namespace = namespace_from(document);
    const auto namespace_exempt =
        std::ranges::find(snapshot->policy.exempt_namespaces, pod_namespace) !=
        snapshot->policy.exempt_namespaces.end();
    auto output = std::vector<admission_decision>{};
    output.reserve(images->size());
    for (const auto& image : *images) {
        auto decision = admission_decision{};
        if (namespace_exempt) {
            decision = {.policy = policy_name,
                        .allowed = true,
                        .image = image,
                        .subject = subject_from_image(image),
                        .reason = "exempt",
                        .reasons = {{.rule_id = "policy.namespace_exempt",
                                     .message = "namespace matched an exemption"}}};
        } else {
            auto reviewed =
                co_await review_image(snapshot->policy, image, *snapshot->source, *snapshot->log);
            if (!reviewed) {
                co_return std::unexpected(reviewed.error());
            }
            decision = std::move(*reviewed);
            decision.policy = policy_name;
        }
        state->record_image(decision.allowed);
        output.push_back(state->publish(std::move(decision)));
    }
    co_return output;
}

auto bad_request(boost::http::route_params& params, const error& failure)
    -> boost::http::route_task {
    auto body = json::write(json::value{json::value::object{
        {"error", json::value{failure.message}},
        {"context", json::value{failure.context}},
    }});
    const auto status = failure.code == errc::not_found ? boost::http::status::not_found
                                                        : boost::http::status::bad_request;
    co_return co_await http1::send(params, status, "application/json", body.value_or("{}"));
}

auto handle_admission(std::shared_ptr<service_state> state,
                      std::string policy_name,
                      boost::http::route_params& params) -> boost::http::route_task {
    const auto started = std::chrono::steady_clock::now();
    auto body = co_await http1::read_body(params);
    if (!body) {
        co_return boost::http::route_error(body.error());
    }
    auto document = json::parse(text(*body));
    const auto* version = document ? string_field(*document, "apiVersion") : nullptr;
    const auto* kind = document ? string_field(*document, "kind") : nullptr;
    const auto* request = document ? object_field(*document, "request") : nullptr;
    const auto* uid = request ? string_field(*request, "uid") : nullptr;
    if (!document || !version || *version != "admission.k8s.io/v1" || !kind ||
        *kind != "AdmissionReview" || !uid) {
        const auto failure = document
                                 ? admission_error(errc::invalid_argument,
                                                   "AdmissionReview v1 kind and uid are required")
                                 : document.error();
        co_return co_await bad_request(params, failure);
    }
    auto decisions = co_await review_pod(state, std::move(policy_name), *document);
    if (!decisions) {
        co_return co_await bad_request(params, decisions.error());
    }
    const auto denied =
        std::ranges::find_if(*decisions, [](const auto& decision) { return !decision.allowed; });
    const auto allowed = denied == decisions->end();
    auto reason = allowed || denied->reason.empty() ? std::string{} : denied->reason;
    if (allowed && !decisions->empty() && std::ranges::all_of(*decisions, [](const auto& decision) {
            return decision.reason == "exempt";
        })) {
        reason = "exempt";
    }
    const auto elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    state->record_review(allowed, reason, elapsed);
    auto response = review_response(*uid, *decisions);
    if (!response) {
        co_return co_await bad_request(params, response.error());
    }
    co_return co_await http1::send(
        params, boost::http::status::ok, "application/json", std::move(*response));
}

auto admission_router(std::shared_ptr<service_state> state) -> http1::router {
    auto routes = http1::router{boost::http::router_options{}.case_sensitive(true)};
    routes.add(boost::http::method::post,
               "/admit/:policy",
               [state](boost::http::route_params& params) -> boost::http::route_task {
                   return handle_admission(
                       state, std::string{route_parameter(params, "policy")}, params);
               });
    routes.add(boost::http::method::post,
               "/admission/review",
               [state](boost::http::route_params& params) -> boost::http::route_task {
                   return handle_admission(state, "default", params);
               });
    return routes;
}

auto openapi() -> std::string {
    return R"({
  "openapi": "3.1.0",
  "info": {"title": "liquid admission", "version": "v1"},
  "paths": {
    "/admit/{policy}": {
      "post": {
        "parameters": [
          {"name": "policy", "in": "path", "required": true, "schema": {"type": "string"}}
        ],
        "responses": {
          "200": {"description": "AdmissionReview response with structured decisions"},
          "404": {"description": "unknown policy"}
        }
      }
    },
    "/admission/review": {
      "post": {
        "deprecated": true,
        "responses": {"200": {"description": "default-policy AdmissionReview response"}}
      }
    },
    "/healthz": {"get": {"responses": {"200": {"description": "healthy"}}}},
    "/readyz": {
      "get": {
        "responses": {
          "200": {"description": "ready"},
          "503": {"description": "not ready"}
        }
      }
    },
    "/metrics": {"get": {"responses": {"200": {"description": "Prometheus metrics"}}}},
    "/v1": {"get": {"responses": {"200": {"description": "capabilities"}}}},
    "/v1/policies": {
      "get": {"responses": {"200": {"description": "policy names and reload status"}}}
    },
    "/v1/policy": {
      "get": {
        "parameters": [
          {"name": "name", "in": "query", "schema": {"type": "string", "default": "default"}}
        ],
        "responses": {
          "200": {
            "description": "effective named policy",
            "content": {
              "application/json": {"schema": {"$ref": "#/components/schemas/AdmissionPolicy"}}
            }
          }
        }
      }
    },
    "/v1/policy/reload": {
      "post": {
        "responses": {
          "200": {"description": "policy sets atomically reloaded"},
          "400": {"description": "invalid policy; previous sets retained"}
        }
      }
    },
    "/v1/verify": {
      "get": {
        "responses": {
          "200": {
            "description": "image decision with transparency verification outcome",
            "content": {
              "application/json": {"schema": {"$ref": "#/components/schemas/AdmissionDecision"}}
            }
          }
        }
      }
    },
    "/v1/decisions": {
      "get": {
        "responses": {"200": {"description": "server-sent decision stream with bounded replay"}}
      }
    },
    "/v1/openapi.json": {
      "get": {"responses": {"200": {"description": "OpenAPI document"}}}
    },
    "/mcp": {"post": {"responses": {"200": {"description": "MCP JSON-RPC response"}}}}
  },
  "components": {
    "schemas": {
      "LogIdentity": {
        "type": "object",
        "properties": {
          "taap_id": {"type": "string", "pattern": "^taap:1:ed25519:"},
          "public_key": {"type": "string", "pattern": "^[0-9a-f]{64}$"}
        },
        "required": ["taap_id", "public_key"]
      },
      "LogWitness": {
        "type": "object",
        "properties": {
          "endpoint": {"type": "string"},
          "public_key": {"type": "string", "pattern": "^[0-9a-f]{64}$"}
        },
        "required": ["endpoint", "public_key"]
      },
      "LogTrust": {
        "type": "object",
        "properties": {
          "identity": {"$ref": "#/components/schemas/LogIdentity"},
          "witness_threshold": {"type": "integer", "minimum": 0},
          "witnesses": {
            "type": "array",
            "items": {"$ref": "#/components/schemas/LogWitness"}
          }
        },
        "required": ["identity", "witness_threshold", "witnesses"]
      },
      "AdmissionLogPolicy": {
        "type": "object",
        "properties": {
          "mode": {"type": "string", "enum": ["deny", "verify"]},
          "lake": {"type": "string"},
          "trust": {"$ref": "#/components/schemas/LogTrust"},
          "max_checkpoint_age_seconds": {"type": "integer", "minimum": 0}
        },
        "required": ["mode", "lake", "trust", "max_checkpoint_age_seconds"]
      },
      "AdmissionPolicy": {
        "type": "object",
        "properties": {
          "v": {"const": 1},
          "mode": {"type": "string", "enum": ["off", "attestation", "attestation+log"]},
          "issuers": {"type": "array", "items": {"type": "string"}},
          "profiles": {"type": "array", "items": {"type": "string"}},
          "exempt": {"type": "array", "items": {"type": "string"}},
          "exempt_namespaces": {"type": "array", "items": {"type": "string"}},
          "attestation_source": {"type": "string"},
          "log": {"$ref": "#/components/schemas/AdmissionLogPolicy"}
        },
        "required": [
          "v",
          "mode",
          "issuers",
          "profiles",
          "exempt",
          "exempt_namespaces",
          "attestation_source",
          "log"
        ]
      },
      "LogVerification": {
        "type": ["object", "null"],
        "properties": {
          "outcome": {
            "type": "string",
            "enum": ["included", "not_included", "unavailable", "denied"]
          },
          "checkpoint": {"type": ["string", "null"]},
          "checkpoint_sequence": {"type": "integer", "minimum": 0},
          "tree_size": {"type": "integer", "minimum": 0},
          "reason": {"type": "string"}
        },
        "required": ["outcome", "checkpoint", "checkpoint_sequence", "tree_size", "reason"]
      },
      "Evidence": {
        "type": "object",
        "properties": {
          "attestation": {"type": "string"},
          "profile": {"type": "string"},
          "issuer": {"type": "string"},
          "verified": {"type": "boolean"},
          "rule_id": {"type": "string"},
          "reason": {"type": "string"},
          "log": {"$ref": "#/components/schemas/LogVerification"}
        },
        "required": ["attestation", "profile", "issuer", "verified", "rule_id", "reason", "log"]
      },
      "AdmissionDecision": {
        "type": "object",
        "properties": {
          "v": {"const": 1},
          "kind": {"const": "admission-decision"},
          "sequence": {"type": "integer", "minimum": 0},
          "policy": {"type": "string"},
          "subject": {"type": ["string", "null"]},
          "image": {"type": "string"},
          "outcome": {"type": "string", "enum": ["allowed", "denied"]},
          "reasons": {"type": "array", "items": {"type": "object"}},
          "evidence": {"type": "array", "items": {"$ref": "#/components/schemas/Evidence"}}
        },
        "required": ["v", "kind", "sequence", "policy", "subject", "image", "outcome", "reasons", "evidence"]
      }
    }
  }
})";
}

auto mcp_id(const json::value& request) -> json::value {
    const auto* id = field(request, "id");
    return id ? *id : json::value{json::null{}};
}

auto mcp_result(const json::value& request, json::value payload) -> result<std::string> {
    return json::write(json::value{json::value::object{
        {"jsonrpc", json::value{std::string{"2.0"}}},
        {"id", mcp_id(request)},
        {"result", std::move(payload)},
    }});
}

auto mcp_content(std::string value) -> json::value {
    return json::value{json::value::object{
        {"content",
         json::value{json::value::array{
             json::value{json::value::object{
                 {"type", json::value{std::string{"text"}}},
                 {"text", json::value{std::move(value)}},
             }},
         }}},
    }};
}

auto verify_json(std::shared_ptr<service_state> state, std::string policy_name, std::string image)
    -> runtime::task<std::string> {
    auto snapshot = state->snapshot(policy_name);
    if (!snapshot) {
        co_return std::unexpected(snapshot.error());
    }
    auto decision = co_await review_image(
        snapshot->policy, std::move(image), *snapshot->source, *snapshot->log);
    if (!decision) {
        co_return std::unexpected(decision.error());
    }
    decision->policy = std::move(policy_name);
    auto structured = decision_json_value(*decision);
    if (!structured) {
        co_return std::unexpected(structured.error());
    }
    auto* fields = std::get_if<json::value::object>(&structured->data);
    if (!fields) {
        co_return std::unexpected(
            admission_error(errc::corrupt, "admission decision is not an object"));
    }
    const auto* evidence_field = field(*structured, "evidence");
    const auto evidence = evidence_field ? *evidence_field : json::value{json::value::array{}};
    fields->emplace_back("allowed", json::value{decision->allowed});
    fields->emplace_back("reason", json::value{decision->reason});
    fields->emplace_back("attestation",
                         decision->attestation ? json::value{to_string(*decision->attestation)}
                                               : json::value{json::null{}});
    fields->emplace_back("considered", evidence);
    co_return json::write(*structured);
}

auto policies_json(const policy_status& status) -> result<std::string> {
    auto names = json::value::array{};
    for (const auto& name : status.names) {
        names.emplace_back(name);
    }
    auto failure = json::value{json::null{}};
    if (status.reload_error) {
        failure = json::value{json::value::object{
            {"message", json::value{status.reload_error->message}},
            {"context", json::value{status.reload_error->context}},
        }};
    }
    return json::write(json::value{json::value::object{
        {"policies", json::value{std::move(names)}},
        {"last_reload_error", std::move(failure)},
    }});
}

auto decisions_json(std::span<const admission_decision> decisions) -> result<std::string> {
    auto records = json::value::array{};
    records.reserve(decisions.size());
    for (const auto& decision : decisions) {
        auto record = decision_json_value(decision);
        if (!record) {
            return std::unexpected(record.error());
        }
        records.push_back(std::move(*record));
    }
    return json::write(json::value{json::value::object{
        {"decisions", json::value{std::move(records)}},
    }});
}

auto stream_write(boost::http::route_params& params, std::string_view body, bool final)
    -> boost::capy::io_task<> {
    auto offset = std::size_t{};
    do {
        auto buffers = std::array<boost::capy::mutable_buffer, 16>{};
        const auto output = params.res_body.prepare(buffers);
        const auto remaining = body.substr(offset);
        const auto copied = boost::capy::buffer_copy(
            output, boost::capy::const_buffer(remaining.data(), remaining.size()));
        offset += copied;
        if (offset == body.size()) {
            if (final) {
                co_return co_await params.res_body.commit_eof(copied);
            }
            co_return co_await params.res_body.commit(copied);
        }
        auto [write_error] = co_await params.res_body.commit(copied);
        if (write_error) {
            co_return write_error;
        }
    } while (offset < body.size());
    co_return std::error_code{};
}

auto decision_event(const admission_decision& decision) -> result<std::string> {
    auto encoded = encode_admission_decision_json(decision);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    auto output = std::format("id: {}\nevent: decision\n", decision.sequence);
    auto remaining = std::string_view{*encoded};
    while (!remaining.empty()) {
        const auto newline = remaining.find('\n');
        const auto line = remaining.substr(0, newline);
        output += "data: ";
        output.append(line);
        output.push_back('\n');
        if (newline == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(newline + 1);
    }
    output.push_back('\n');
    return output;
}

auto decision_stream(std::shared_ptr<service_state> state, boost::http::route_params& params)
    -> boost::http::route_task {
    const auto encoded_query = params.url.encoded_query();
    const auto query = std::string_view{encoded_query.data(), encoded_query.size()};
    auto after = std::uint64_t{};
    if (const auto value = optional_query_parameter(query, "after")) {
        auto parsed = unsigned_value(*value, "after");
        if (!parsed) {
            co_return co_await bad_request(params, parsed.error());
        }
        after = *parsed;
    }
    auto limit = std::optional<std::uint64_t>{};
    if (const auto value = optional_query_parameter(query, "limit")) {
        auto parsed = unsigned_value(*value, "limit");
        if (!parsed || *parsed == 0) {
            const auto failure =
                parsed ? admission_error(errc::invalid_argument, "limit must be positive", "limit")
                       : parsed.error();
            co_return co_await bad_request(params, failure);
        }
        limit = *parsed;
    }
    params.res.set_status(boost::http::status::ok);
    params.res.set_keep_alive(false);
    params.res.set(boost::http::field::content_type, "text/event-stream");
    params.res.set(boost::http::field::cache_control, "no-cache");
    params.res.set_chunked(true);
    auto sent = std::uint64_t{};
    for (;;) {
        for (const auto& decision : state->decisions_after(after)) {
            auto event = decision_event(decision);
            if (!event) {
                co_return boost::http::route_error(
                    std::make_error_code(std::errc::invalid_argument));
            }
            after = decision.sequence;
            ++sent;
            const auto final = limit && sent == *limit;
            auto [write_error] = co_await stream_write(params, *event, final);
            if (write_error) {
                co_return boost::http::route_error(write_error);
            }
            if (final) {
                co_return boost::http::route_done;
            }
        }
        auto [delay_error] = co_await runtime::delay(std::chrono::milliseconds{50});
        if (delay_error) {
            co_return boost::http::route_error(delay_error);
        }
    }
}

auto control_router(std::shared_ptr<service_state> state) -> http1::router {
    auto routes = http1::router{};
    routes.add(boost::http::method::get,
               "/healthz",
               [](boost::http::route_params& params) -> boost::http::route_task {
                   co_return co_await http1::send(
                       params, boost::http::status::ok, "text/plain", "ok\n");
               });
    routes.add(boost::http::method::get,
               "/readyz",
               [state](boost::http::route_params& params) -> boost::http::route_task {
                   const auto ready = state->ready();
                   co_return co_await http1::send(params,
                                                  ready ? boost::http::status::ok
                                                        : boost::http::status::service_unavailable,
                                                  "text/plain",
                                                  ready ? "ready\n" : "not ready\n");
               });
    routes.add(boost::http::method::get,
               "/metrics",
               [state](boost::http::route_params& params) -> boost::http::route_task {
                   co_return co_await http1::send(params,
                                                  boost::http::status::ok,
                                                  "text/plain; version=0.0.4",
                                                  state->metrics());
               });
    routes.add(
        boost::http::method::get,
        "/v1",
        [](boost::http::route_params& params) -> boost::http::route_task {
            co_return co_await http1::send(
                params,
                boost::http::status::ok,
                "application/json",
                R"({"component":"admission","version":"v1","capabilities":["policies","verify","decisions","openapi","mcp"]})");
        });
    routes.add(boost::http::method::get,
               "/v1/policies",
               [state](boost::http::route_params& params) -> boost::http::route_task {
                   auto encoded = policies_json(state->status());
                   if (!encoded) {
                       co_return co_await bad_request(params, encoded.error());
                   }
                   co_return co_await http1::send(
                       params, boost::http::status::ok, "application/json", std::move(*encoded));
               });
    routes.add(boost::http::method::get,
               "/v1/policy",
               [state](boost::http::route_params& params) -> boost::http::route_task {
                   const auto encoded_query = params.url.encoded_query();
                   const auto query = std::string_view{encoded_query.data(), encoded_query.size()};
                   auto name = optional_query_parameter(query, "name").value_or("default");
                   auto snapshot = state->snapshot(name);
                   if (!snapshot) {
                       co_return co_await bad_request(params, snapshot.error());
                   }
                   auto encoded = encode_admission_policy(snapshot->policy);
                   if (!encoded) {
                       co_return co_await bad_request(params, encoded.error());
                   }
                   co_return co_await http1::send(
                       params, boost::http::status::ok, "application/json", std::move(*encoded));
               });
    routes.add(boost::http::method::post,
               "/v1/policy/reload",
               [state](boost::http::route_params& params) -> boost::http::route_task {
                   auto reloaded = co_await state->reload(true);
                   if (!reloaded) {
                       co_return co_await bad_request(params, reloaded.error());
                   }
                   const auto body = *reloaded ? R"({"reloaded":true})" : R"({"reloaded":false})";
                   co_return co_await http1::send(
                       params, boost::http::status::ok, "application/json", body);
               });
    routes.add(boost::http::method::get,
               "/v1/verify",
               [state](boost::http::route_params& params) -> boost::http::route_task {
                   const auto query = params.url.encoded_query();
                   auto image = query_parameter({query.data(), query.size()}, "image");
                   if (!image) {
                       co_return co_await bad_request(params, image.error());
                   }
                   auto policy = optional_query_parameter({query.data(), query.size()}, "policy")
                                     .value_or("default");
                   auto verified =
                       co_await verify_json(state, std::move(policy), std::move(*image));
                   if (!verified) {
                       co_return co_await bad_request(params, verified.error());
                   }
                   co_return co_await http1::send(
                       params, boost::http::status::ok, "application/json", std::move(*verified));
               });
    routes.add(boost::http::method::get,
               "/v1/decisions",
               [state](boost::http::route_params& params) -> boost::http::route_task {
                   return decision_stream(state, params);
               });
    routes.add(boost::http::method::get,
               "/v1/openapi.json",
               [](boost::http::route_params& params) -> boost::http::route_task {
                   co_return co_await http1::send(
                       params, boost::http::status::ok, "application/json", openapi());
               });
    routes.add(
        boost::http::method::post,
        "/mcp",
        [state](boost::http::route_params& params) -> boost::http::route_task {
            auto body = co_await http1::read_body(params);
            if (!body) {
                co_return boost::http::route_error(body.error());
            }
            auto request = json::parse(text(*body));
            const auto* method = request ? string_field(*request, "method") : nullptr;
            if (!request || !method) {
                const auto failure =
                    request ? admission_error(errc::invalid_argument, "MCP method is missing")
                            : request.error();
                co_return co_await bad_request(params, failure);
            }
            auto response_result = result<std::string>{};
            if (*method == "initialize") {
                response_result =
                    mcp_result(*request,
                               json::value{json::value::object{
                                   {"protocolVersion", json::value{std::string{"2025-06-18"}}},
                                   {"capabilities", json::value{json::value::object{}}},
                                   {"serverInfo",
                                    json::value{json::value::object{
                                        {"name", json::value{std::string{"liquid-admission"}}},
                                        {"version", json::value{std::string{"v1"}}},
                                    }}},
                               }});
            } else if (*method == "tools/list") {
                response_result = mcp_result(
                    *request,
                    json::value{json::value::object{
                        {"tools",
                         json::value{json::value::array{
                             json::value{json::value::object{
                                 {"name", json::value{std::string{"admission_policies"}}},
                             }},
                             json::value{json::value::object{
                                 {"name", json::value{std::string{"admission_policy"}}},
                             }},
                             json::value{json::value::object{
                                 {"name", json::value{std::string{"admission_verify"}}},
                                 {"description",
                                  json::value{std::string{
                                      "Verify an image; evidence.log reports checkpoint, tile, "
                                      "and inclusion verification."}}},
                             }},
                             json::value{json::value::object{
                                 {"name", json::value{std::string{"admission_decisions"}}},
                                 {"description",
                                  json::value{std::string{
                                      "List retained decisions including transparency outcomes."}}},
                             }},
                         }}},
                    }});
            } else if (*method == "tools/call") {
                const auto* parameters = object_field(*request, "params");
                const auto* name = parameters ? string_field(*parameters, "name") : nullptr;
                const auto* arguments =
                    parameters ? object_field(*parameters, "arguments") : nullptr;
                if (name && *name == "admission_policies") {
                    auto encoded = policies_json(state->status());
                    response_result = encoded
                                          ? mcp_result(*request, mcp_content(*encoded))
                                          : result<std::string>{std::unexpected(encoded.error())};
                } else if (name && *name == "admission_policy") {
                    const auto* selected = arguments ? string_field(*arguments, "policy") : nullptr;
                    auto snapshot = state->snapshot(selected ? *selected : "default");
                    if (!snapshot) {
                        response_result = std::unexpected(snapshot.error());
                    } else {
                        auto encoded = encode_admission_policy(snapshot->policy);
                        response_result =
                            encoded ? mcp_result(*request, mcp_content(*encoded))
                                    : result<std::string>{std::unexpected(encoded.error())};
                    }
                } else if (name && *name == "admission_decisions") {
                    auto encoded = decisions_json(state->decisions_after(0));
                    response_result = encoded
                                          ? mcp_result(*request, mcp_content(*encoded))
                                          : result<std::string>{std::unexpected(encoded.error())};
                } else if (name && *name == "admission_verify") {
                    const auto* image = arguments ? string_field(*arguments, "image") : nullptr;
                    const auto* policy = arguments ? string_field(*arguments, "policy") : nullptr;
                    if (!image) {
                        response_result = std::unexpected(admission_error(
                            errc::invalid_argument, "admission_verify requires an image argument"));
                    } else {
                        auto verified =
                            co_await verify_json(state, policy ? *policy : "default", *image);
                        response_result =
                            verified ? mcp_result(*request, mcp_content(*verified))
                                     : result<std::string>{std::unexpected(verified.error())};
                    }
                } else {
                    response_result = std::unexpected(
                        admission_error(errc::invalid_argument, "unknown admission tool"));
                }
            } else {
                response_result = json::write(json::value{json::value::object{
                    {"jsonrpc", json::value{std::string{"2.0"}}},
                    {"id", mcp_id(*request)},
                    {"error",
                     json::value{json::value::object{
                         {"code", json::value{json::number{"-32601"}}},
                         {"message", json::value{std::string{"method not found"}}},
                     }}},
                }});
            }
            if (!response_result) {
                co_return co_await bad_request(params, response_result.error());
            }
            co_return co_await http1::send(
                params, boost::http::status::ok, "application/json", std::move(*response_result));
        });
    return routes;
}

auto detached_control(std::shared_ptr<service_state> state, std::string listen)
    -> boost::capy::task<> {
    auto outcome = co_await http1::serve(http1::server_options{.listen = std::move(listen)},
                                         control_router(std::move(state)));
    if (!outcome) {
        std::println(stderr,
                     "admission control listener error: {} ({})",
                     outcome.error().message,
                     outcome.error().context);
    }
    co_return;
}

auto detached_reload(std::shared_ptr<service_state> state) -> boost::capy::task<> {
    for (;;) {
        auto [delay_error] = co_await runtime::delay(std::chrono::milliseconds{100});
        if (delay_error) {
            co_return;
        }
        auto reloaded = co_await state->reload(false);
        if (!reloaded) {
            std::println(stderr,
                         "admission policy reload rejected: {} ({})",
                         reloaded.error().message,
                         reloaded.error().context);
        }
    }
}

} // namespace

runtime::task<void> serve_admission(admission_options options) {
    if (options.policy.empty() == options.policies.empty()) {
        co_return std::unexpected(admission_error(
            errc::invalid_argument, "exactly one admission policy file or directory is required"));
    }
    if (options.store.empty() == false && options.attestation_source.empty() == false) {
        co_return std::unexpected(admission_error(
            errc::invalid_argument, "store and attestation source are mutually exclusive"));
    }
    if (!options.insecure_http && (options.tls_certificate.empty() || options.tls_key.empty())) {
        co_return std::unexpected(
            admission_error(errc::invalid_argument, "TLS certificate and key are required"));
    }
    if (options.decision_replay_capacity == 0) {
        co_return std::unexpected(
            admission_error(errc::invalid_argument, "decision replay capacity must be positive"));
    }
    auto state = std::make_shared<service_state>(options);
    auto loaded = co_await state->reload(true);
    if (!loaded) {
        co_return std::unexpected(loaded.error());
    }
    const auto executor = co_await boost::capy::this_coro::executor;
    boost::capy::run_async(executor)(detached_control(state, options.control_listen));
    boost::capy::run_async(executor)(detached_reload(state));
    auto server = http1::server_options{.listen = std::move(options.listen)};
    if (!options.insecure_http) {
        server.tls_certificate = std::move(options.tls_certificate);
        server.tls_key = std::move(options.tls_key);
    }
    co_return co_await http1::serve(std::move(server), admission_router(std::move(state)));
}

} // namespace liquid
