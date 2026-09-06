#include "liquid/admission.hpp"

#include <algorithm>
#include <chrono>
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
#include <utility>
#include <vector>

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

class unavailable_log final : public log_verifier {
public:
    runtime::task<log_verdict> verify_inclusion(digest attestation) override {
        static_cast<void>(attestation);
        co_return log_verdict::unavailable;
    }
};

struct service_snapshot {
    admission_policy policy;
    std::shared_ptr<attestation_source> source;
    std::shared_ptr<log_verifier> log;
};

class service_state {
public:
    explicit service_state(admission_options options) : options_(std::move(options)) {}

    runtime::task<void> reload() {
        auto loaded =
            co_await runtime::offload([path = options_.policy] { return read_file(path); });
        if (!loaded) {
            co_return std::unexpected(loaded.error());
        }
        if (!*loaded) {
            co_return std::unexpected(loaded->error());
        }
        auto policy = decode_admission_policy(**loaded);
        if (!policy) {
            co_return std::unexpected(policy.error());
        }
        if (!options_.store.empty()) {
            policy->attestation_source = options_.store.string();
        } else if (!options_.attestation_source.empty()) {
            policy->attestation_source = options_.attestation_source;
        }
        auto source = co_await admission_detail::open_source(policy->attestation_source);
        auto source_ready = false;
        auto selected_source = std::shared_ptr<attestation_source>{};
        if (source) {
            selected_source = *source;
            if (policy->mode == admission_mode::off) {
                source_ready = true;
            } else {
                auto probe = co_await selected_source->by_subject(
                    digest{.alg = algorithm::sha256, .bytes = {}});
                source_ready = probe.has_value();
            }
        } else {
            selected_source = std::make_shared<unavailable_source>(source.error());
        }
        auto selected_log = admission_detail::make_log_verifier(policy->log, options_.log_cache);
        {
            auto lock = std::scoped_lock{mutex_};
            policy_ = std::move(*policy);
            source_ = std::move(selected_source);
            log_ = std::move(selected_log);
            loaded_ = true;
            source_ready_ = source_ready;
        }
        co_return result<void>{};
    }

    auto snapshot() const -> service_snapshot {
        auto lock = std::scoped_lock{mutex_};
        return {policy_, source_, log_};
    }

    auto ready() const -> bool {
        auto lock = std::scoped_lock{mutex_};
        return loaded_ && source_ready_;
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
    admission_options options_;
    mutable std::mutex mutex_;
    admission_policy policy_;
    std::shared_ptr<attestation_source> source_{std::make_shared<unavailable_source>(
        admission_error(errc::unavailable, "admission policy is not loaded"))};
    std::shared_ptr<log_verifier> log_{std::make_shared<unavailable_log>()};
    bool loaded_{};
    bool source_ready_{};

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

auto decision_json(const admission_decision& decision) -> json::value {
    auto considered = json::value::array{};
    for (const auto& statement : decision.considered) {
        considered.emplace_back(json::value::object{
            {"attestation", json::value{to_string(statement.attestation)}},
            {"profile", json::value{statement.profile}},
            {"issuer", json::value{statement.issuer}},
            {"verified", json::value{statement.verified}},
            {"reason", json::value{statement.reason}},
        });
    }
    auto output = json::value::object{
        {"allowed", json::value{decision.allowed}},
        {"image", json::value{decision.image}},
        {"reason", json::value{decision.reason}},
        {"considered", json::value{std::move(considered)}},
    };
    if (decision.attestation) {
        output.emplace_back("attestation", json::value{to_string(*decision.attestation)});
    } else {
        output.emplace_back("attestation", json::value{json::null{}});
    }
    return json::value{std::move(output)};
}

auto review_response(std::string uid, const admission_decision& decision) -> result<std::string> {
    auto response = json::value::object{
        {"uid", json::value{std::move(uid)}},
        {"allowed", json::value{decision.allowed}},
    };
    if (!decision.allowed) {
        response.emplace_back("status",
                              json::value{json::value::object{
                                  {"code", number(403)},
                                  {"message", json::value{decision.image + ": " + decision.reason}},
                              }});
    }
    return json::write(json::value{json::value::object{
        {"apiVersion", json::value{std::string{"admission.k8s.io/v1"}}},
        {"kind", json::value{std::string{"AdmissionReview"}}},
        {"response", json::value{std::move(response)}},
    }});
}

auto review_pod(std::shared_ptr<service_state> state, const json::value& document)
    -> runtime::task<admission_decision> {
    auto images = images_from(document);
    if (!images) {
        co_return std::unexpected(images.error());
    }
    auto snapshot = state->snapshot();
    const auto pod_namespace = namespace_from(document);
    const auto namespace_exempt =
        std::ranges::find(snapshot.policy.exempt_namespaces, pod_namespace) !=
        snapshot.policy.exempt_namespaces.end();
    auto first = admission_decision{.allowed = true, .reason = "exempt"};
    auto all_exempt = true;
    for (const auto& image : *images) {
        auto decision = admission_decision{};
        if (namespace_exempt) {
            decision = {.allowed = true, .image = image, .reason = "exempt"};
        } else {
            auto reviewed =
                co_await review_image(snapshot.policy, image, *snapshot.source, *snapshot.log);
            if (!reviewed) {
                co_return std::unexpected(reviewed.error());
            }
            decision = std::move(*reviewed);
        }
        state->record_image(decision.allowed);
        all_exempt = all_exempt && decision.reason == "exempt";
        if (!decision.allowed && first.allowed) {
            first = decision;
        }
    }
    if (first.allowed && !all_exempt) {
        first.reason.clear();
    }
    co_return first;
}

auto bad_request(boost::http::route_params& params, const error& failure)
    -> boost::http::route_task {
    auto body = json::write(json::value{json::value::object{
        {"error", json::value{failure.message}},
        {"context", json::value{failure.context}},
    }});
    co_return co_await http1::send(
        params, boost::http::status::bad_request, "application/json", body.value_or("{}"));
}

auto admission_router(std::shared_ptr<service_state> state) -> http1::router {
    auto routes = http1::router{};
    routes.add(
        boost::http::method::post,
        "/admission/review",
        [state](boost::http::route_params& params) -> boost::http::route_task {
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
                const auto failure =
                    document ? admission_error(errc::invalid_argument,
                                               "AdmissionReview v1 kind and uid are required")
                             : document.error();
                co_return co_await bad_request(params, failure);
            }
            auto decision = co_await review_pod(state, *document);
            if (!decision) {
                co_return co_await bad_request(params, decision.error());
            }
            const auto elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            state->record_review(decision->allowed, decision->reason, elapsed);
            auto response = review_response(*uid, *decision);
            if (!response) {
                co_return co_await bad_request(params, response.error());
            }
            co_return co_await http1::send(
                params, boost::http::status::ok, "application/json", std::move(*response));
        });
    return routes;
}

auto openapi() -> std::string {
    return R"({"openapi":"3.1.0","info":{"title":"liquid admission","version":"v1"},"paths":{"/admission/review":{"post":{"responses":{"200":{"description":"AdmissionReview response"}}}},"/healthz":{"get":{"responses":{"200":{"description":"healthy"}}}},"/readyz":{"get":{"responses":{"200":{"description":"ready"},"503":{"description":"not ready"}}}},"/metrics":{"get":{"responses":{"200":{"description":"Prometheus metrics"}}}},"/v1":{"get":{"responses":{"200":{"description":"capabilities"}}}},"/v1/policy":{"get":{"responses":{"200":{"description":"effective policy"}}}},"/v1/policy/reload":{"post":{"responses":{"200":{"description":"policy reloaded"}}}},"/v1/verify":{"get":{"responses":{"200":{"description":"image decision"}}}},"/v1/openapi.json":{"get":{"responses":{"200":{"description":"OpenAPI document"}}}},"/mcp":{"post":{"responses":{"200":{"description":"MCP JSON-RPC response"}}}}}})";
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

auto verify_json(std::shared_ptr<service_state> state, std::string image)
    -> runtime::task<std::string> {
    auto snapshot = state->snapshot();
    auto decision =
        co_await review_image(snapshot.policy, std::move(image), *snapshot.source, *snapshot.log);
    if (!decision) {
        co_return std::unexpected(decision.error());
    }
    co_return json::write(decision_json(*decision));
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
                R"({"component":"admission","version":"v1","capabilities":["policy","verify","openapi","mcp"]})");
        });
    routes.add(boost::http::method::get,
               "/v1/policy",
               [state](boost::http::route_params& params) -> boost::http::route_task {
                   auto encoded = encode_admission_policy(state->snapshot().policy);
                   if (!encoded) {
                       co_return co_await bad_request(params, encoded.error());
                   }
                   co_return co_await http1::send(
                       params, boost::http::status::ok, "application/json", std::move(*encoded));
               });
    routes.add(boost::http::method::post,
               "/v1/policy/reload",
               [state](boost::http::route_params& params) -> boost::http::route_task {
                   auto reloaded = co_await state->reload();
                   if (!reloaded) {
                       co_return co_await bad_request(params, reloaded.error());
                   }
                   co_return co_await http1::send(
                       params, boost::http::status::ok, "application/json", R"({"reloaded":true})");
               });
    routes.add(boost::http::method::get,
               "/v1/verify",
               [state](boost::http::route_params& params) -> boost::http::route_task {
                   const auto query = params.url.encoded_query();
                   auto image = query_parameter({query.data(), query.size()}, "image");
                   if (!image) {
                       co_return co_await bad_request(params, image.error());
                   }
                   auto verified = co_await verify_json(state, std::move(*image));
                   if (!verified) {
                       co_return co_await bad_request(params, verified.error());
                   }
                   co_return co_await http1::send(
                       params, boost::http::status::ok, "application/json", std::move(*verified));
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
                response_result =
                    mcp_result(*request,
                               json::value{json::value::object{
                                   {"tools",
                                    json::value{json::value::array{
                                        json::value{json::value::object{
                                            {"name", json::value{std::string{"admission_policy"}}},
                                        }},
                                        json::value{json::value::object{
                                            {"name", json::value{std::string{"admission_verify"}}},
                                        }},
                                    }}},
                               }});
            } else if (*method == "tools/call") {
                const auto* parameters = object_field(*request, "params");
                const auto* name = parameters ? string_field(*parameters, "name") : nullptr;
                if (name && *name == "admission_policy") {
                    auto encoded = encode_admission_policy(state->snapshot().policy);
                    response_result = encoded
                                          ? mcp_result(*request, mcp_content(*encoded))
                                          : result<std::string>{std::unexpected(encoded.error())};
                } else if (name && *name == "admission_verify") {
                    const auto* arguments = object_field(*parameters, "arguments");
                    const auto* image = arguments ? string_field(*arguments, "image") : nullptr;
                    if (!image) {
                        response_result = std::unexpected(admission_error(
                            errc::invalid_argument, "admission_verify requires an image argument"));
                    } else {
                        auto verified = co_await verify_json(state, *image);
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

} // namespace

runtime::task<void> serve_admission(admission_options options) {
    if (options.policy.empty()) {
        co_return std::unexpected(
            admission_error(errc::invalid_argument, "admission policy path is required"));
    }
    if (options.store.empty() == false && options.attestation_source.empty() == false) {
        co_return std::unexpected(admission_error(
            errc::invalid_argument, "store and attestation source are mutually exclusive"));
    }
    if (!options.insecure_http && (options.tls_certificate.empty() || options.tls_key.empty())) {
        co_return std::unexpected(
            admission_error(errc::invalid_argument, "TLS certificate and key are required"));
    }
    auto state = std::make_shared<service_state>(options);
    auto loaded = co_await state->reload();
    if (!loaded) {
        co_return std::unexpected(loaded.error());
    }
    const auto executor = co_await boost::capy::this_coro::executor;
    boost::capy::run_async(executor)(detached_control(state, options.control_listen));
    auto server = http1::server_options{.listen = std::move(options.listen)};
    if (!options.insecure_http) {
        server.tls_certificate = std::move(options.tls_certificate);
        server.tls_key = std::move(options.tls_key);
    }
    co_return co_await http1::serve(std::move(server), admission_router(std::move(state)));
}

} // namespace liquid
