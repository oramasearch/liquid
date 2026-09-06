#include "forge_double.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <boost/capy/buffers.hpp>
#include <boost/capy/buffers/buffer_copy.hpp>
#include <boost/capy/buffers/vector_dynamic_buffer.hpp>
#include <boost/capy/read.hpp>
#include <boost/corosio.hpp>
#include <boost/http.hpp>
#include <boost/http/server/http_worker.hpp>
#include <nlohmann/json.hpp>

#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "liquid/sync.hpp"

namespace liquid::test {
namespace {

using nlohmann::json;

constexpr auto maximum_body_size = std::size_t{64} * 1024 * 1024;
constexpr auto repository_id = std::string_view{"test"};

struct failure {
    std::string code;
    std::string message;
    std::string path;
};

struct state {
    std::mutex mutex;
    std::vector<json> reviews;
    std::vector<json> comments;
    std::vector<json> protections;
    std::vector<json> deployments;
    std::optional<failure> next_failure;
    std::string bearer_token;
    std::string last_authorization;
    std::uint64_t next_id{1};
    std::uint64_t clock{};
};

auto timestamp(std::uint64_t tick) -> std::string {
    const auto point = std::chrono::sys_seconds{std::chrono::seconds{1'788'537'600 + tick}};
    return std::format("{:%FT%T}Z", point);
}

auto record(state& shared, std::string kind, json value) -> json {
    const auto now = timestamp(++shared.clock);
    return {{"id", kind + '-' + std::to_string(shared.next_id++)},
            {"repository_id", repository_id},
            {"kind", std::move(kind)},
            {"value", std::move(value)},
            {"created_at", now},
            {"updated_at", now}};
}

auto status_for(std::string_view code) -> boost::http::status {
    if (code == "invalid_input") {
        return boost::http::status::bad_request;
    }
    if (code == "unauthorized" || code == "mfa_required") {
        return boost::http::status::forbidden;
    }
    if (code == "not_found") {
        return boost::http::status::not_found;
    }
    if (code == "conflict") {
        return boost::http::status::conflict;
    }
    if (code == "store_unavailable") {
        return boost::http::status::service_unavailable;
    }
    return boost::http::status::internal_server_error;
}

auto send_bytes(boost::http::route_params& params,
                boost::http::status status,
                std::string_view content_type,
                std::span<const std::byte> body) -> boost::http::route_task {
    params.res.set_status(status);
    params.res.set_keep_alive(false);
    params.res.set(boost::http::field::content_type, content_type);
    params.res.set_payload_size(body.size());
    auto offset = std::size_t{};
    for (;;) {
        auto buffers = std::array<boost::capy::mutable_buffer, 16>{};
        const auto output = params.res_body.prepare(buffers);
        const auto remaining = body.subspan(offset);
        const auto copied = boost::capy::buffer_copy(
            output, boost::capy::const_buffer(remaining.data(), remaining.size()));
        offset += copied;
        if (offset == body.size()) {
            auto [error] = co_await params.res_body.commit_eof(copied);
            if (error) {
                co_return boost::http::route_error(error);
            }
            co_return boost::http::route_done;
        }
        auto [error] = co_await params.res_body.commit(copied);
        if (error) {
            co_return boost::http::route_error(error);
        }
    }
}

auto send_json(boost::http::route_params& params, boost::http::status status, json value)
    -> boost::http::route_task {
    const auto encoded = value.dump();
    const auto* first = reinterpret_cast<const std::byte*>(encoded.data());
    co_return co_await send_bytes(
        params, status, "application/json", std::span{first, encoded.size()});
}

auto success(boost::http::route_params& params, json value) -> boost::http::route_task {
    co_return co_await send_json(params,
                                 boost::http::status::ok,
                                 {{"ok", true}, {"value", std::move(value)}, {"duration_ms", 0}});
}

auto error(boost::http::route_params& params,
           std::string code,
           std::string message,
           std::string path = {}) -> boost::http::route_task {
    const auto status = status_for(code);
    co_return co_await send_json(
        params,
        status,
        {{"ok", false},
         {"error",
          {{"code", std::move(code)}, {"message", std::move(message)}, {"path", std::move(path)}}},
         {"duration_ms", 0}});
}

auto request_body(boost::http::route_params& params)
    -> boost::capy::task<std::expected<json, std::error_code>> {
    auto body = std::vector<unsigned char>{};
    auto [read_error, size] = co_await boost::capy::read(
        params.req_body, boost::capy::dynamic_buffer(body, maximum_body_size));
    static_cast<void>(size);
    if (read_error) {
        co_return std::unexpected(read_error);
    }
    try {
        co_return json::parse(body.begin(), body.end());
    } catch (const json::exception&) {
        co_return json::object();
    }
}

auto authorize(state& shared, boost::http::route_params& params) -> std::optional<failure> {
    auto lock = std::unique_lock{shared.mutex};
    const auto header = params.req.find(boost::http::field::authorization);
    shared.last_authorization =
        header == params.req.end() ? std::string{} : std::string{header->value};
    if (!shared.bearer_token.empty() &&
        shared.last_authorization != "Bearer " + shared.bearer_token) {
        return failure{"unauthorized", "invalid bearer credential", "/authorization"};
    }
    return std::exchange(shared.next_failure, std::nullopt);
}

auto with_authorization(state& shared, boost::http::route_params& params, auto operation)
    -> boost::http::route_task {
    if (auto denied = authorize(shared, params)) {
        co_return co_await error(
            params, std::move(denied->code), std::move(denied->message), std::move(denied->path));
    }
    co_return co_await operation();
}

auto list_records(state& shared,
                  boost::http::route_params& params,
                  std::vector<json> state::* records) -> boost::http::route_task {
    co_return co_await with_authorization(shared, params, [&]() -> boost::http::route_task {
        auto lock = std::unique_lock{shared.mutex};
        auto items = json::array();
        for (auto item = (shared.*records).rbegin(); item != (shared.*records).rend(); ++item) {
            items.push_back(*item);
        }
        lock.unlock();
        co_return co_await success(params, {{"items", std::move(items)}});
    });
}

auto create_review(state& shared, boost::http::route_params& params) -> boost::http::route_task {
    co_return co_await with_authorization(shared, params, [&]() -> boost::http::route_task {
        auto input = co_await request_body(params);
        if (!input || !input->contains("change_id")) {
            co_return co_await error(
                params, "invalid_input", "change_id is required", "/change_id");
        }
        auto lock = std::unique_lock{shared.mutex};
        auto value = json{{"change_id", input->at("change_id")},
                          {"reviewers", input->value("reviewers", json::array())},
                          {"state", "open"}};
        shared.reviews.push_back(record(shared, "review", std::move(value)));
        const auto output = shared.reviews.back();
        lock.unlock();
        co_return co_await success(params, output);
    });
}

auto update_review(state& shared, boost::http::route_params& params) -> boost::http::route_task {
    co_return co_await with_authorization(shared, params, [&]() -> boost::http::route_task {
        auto input = co_await request_body(params);
        if (!input || !input->contains("review_id") || !input->contains("state")) {
            co_return co_await error(
                params, "invalid_input", "review_id and state are required", "/review_id");
        }
        const auto requested = input->at("state").get<std::string>();
        if (requested != "open" && requested != "approved" && requested != "closed") {
            co_return co_await error(params, "invalid_input", "invalid review state", "/state");
        }
        auto lock = std::unique_lock{shared.mutex};
        const auto found = std::ranges::find(
            shared.reviews, input->at("review_id").get<std::string>(), [](const json& review) {
                return review.at("id").get<std::string>();
            });
        if (found == shared.reviews.end()) {
            lock.unlock();
            co_return co_await error(params, "not_found", "review was not found");
        }
        (*found)["value"]["state"] = requested;
        (*found)["updated_at"] = timestamp(++shared.clock);
        const auto output = *found;
        lock.unlock();
        co_return co_await success(params, output);
    });
}

auto create_comment(state& shared, boost::http::route_params& params) -> boost::http::route_task {
    co_return co_await with_authorization(shared, params, [&]() -> boost::http::route_task {
        auto input = co_await request_body(params);
        if (!input || !input->contains("review_id") || !input->contains("body") ||
            !input->contains("anchor")) {
            co_return co_await error(
                params, "invalid_input", "comment fields are required", "/anchor");
        }
        auto lock = std::unique_lock{shared.mutex};
        const auto review_id = input->at("review_id").get<std::string>();
        const auto review = std::ranges::find(shared.reviews, review_id, [](const json& item) {
            return item.at("id").get<std::string>();
        });
        if (review == shared.reviews.end()) {
            lock.unlock();
            co_return co_await error(params, "not_found", "review was not found");
        }
        auto value = *input;
        value.erase("repository_id");
        shared.comments.push_back(record(shared, "review_comment", std::move(value)));
        const auto output = shared.comments.back();
        lock.unlock();
        co_return co_await success(params, output);
    });
}

auto create_deployment(state& shared, boost::http::route_params& params)
    -> boost::http::route_task {
    co_return co_await with_authorization(shared, params, [&]() -> boost::http::route_task {
        auto input = co_await request_body(params);
        if (!input || !input->contains("repository") || !input->contains("environment") ||
            !input->contains("change") || !input->contains("image")) {
            co_return co_await error(params, "invalid_input", "deployment fields are required");
        }
        auto lock = std::unique_lock{shared.mutex};
        auto value = json{{"id", "deployment-" + std::to_string(shared.next_id++)},
                          {"repository", input->at("repository")},
                          {"environment", input->at("environment")},
                          {"change", input->at("change")},
                          {"image", input->at("image")},
                          {"result", input->value("result", json(nullptr))},
                          {"rollback_of", nullptr}};
        shared.deployments.push_back(value);
        lock.unlock();
        co_return co_await success(params, value);
    });
}

auto list_deployments(state& shared, boost::http::route_params& params) -> boost::http::route_task {
    co_return co_await with_authorization(shared, params, [&]() -> boost::http::route_task {
        auto lock = std::unique_lock{shared.mutex};
        auto items = json::array();
        for (auto item = shared.deployments.rbegin(); item != shared.deployments.rend(); ++item) {
            items.push_back(*item);
        }
        lock.unlock();
        co_return co_await success(params, {{"items", std::move(items)}});
    });
}

auto rollback_deployment(state& shared, boost::http::route_params& params)
    -> boost::http::route_task {
    co_return co_await with_authorization(shared, params, [&]() -> boost::http::route_task {
        auto input = co_await request_body(params);
        if (!input || !input->contains("repository") || !input->contains("environment") ||
            !input->contains("to")) {
            co_return co_await error(params, "invalid_input", "rollback fields are required");
        }
        auto lock = std::unique_lock{shared.mutex};
        const auto prior = std::ranges::find_if(shared.deployments, [&](const json& item) {
            return item.at("repository") == input->at("repository") &&
                   item.at("environment") == input->at("environment") &&
                   (item.at("id") == input->at("to") || item.at("change") == input->at("to"));
        });
        if (prior == shared.deployments.end()) {
            lock.unlock();
            co_return co_await error(params, "not_found", "rollback target was not deployed");
        }
        auto value = *prior;
        value["id"] = "deployment-" + std::to_string(shared.next_id++);
        value["rollback_of"] = prior->at("id");
        shared.deployments.push_back(value);
        lock.unlock();
        co_return co_await success(params, value);
    });
}

auto deployment_status(state& shared, boost::http::route_params& params)
    -> boost::http::route_task {
    co_return co_await with_authorization(shared, params, [&]() -> boost::http::route_task {
        auto lock = std::unique_lock{shared.mutex};
        if (shared.deployments.empty()) {
            lock.unlock();
            co_return co_await error(params, "not_found", "environment was not deployed");
        }
        const auto& deployment = shared.deployments.back();
        const auto environment = deployment.at("environment").get<std::string>();
        lock.unlock();
        co_return co_await success(params,
                                   {{"phase", "ready"},
                                    {"url", "https://" + environment + ".example.test"},
                                    {"ready", true},
                                    {"message", "ready"}});
    });
}

auto map_anchor(json anchor, const json& hunks, const std::string& target) -> json {
    const auto line = anchor.value("line", std::int64_t{});
    auto shift = std::int64_t{};
    for (const auto& hunk : hunks) {
        const auto old_line = hunk.value("old_line", std::int64_t{});
        const auto old_count = hunk.value("old_count", std::int64_t{});
        const auto new_line = hunk.value("new_line", old_line);
        const auto new_count = hunk.value("new_count", std::int64_t{});
        if (line < old_line) {
            break;
        }
        if (old_count == 0 || line >= old_line + old_count) {
            shift += new_count - old_count;
            continue;
        }
        anchor["line"] =
            new_count == 0 ? new_line : new_line + std::min(line - old_line, new_count - 1);
        anchor["change"] = target;
        anchor["disposition"] = new_count == 0 ? "deleted" : "approximate";
        return anchor;
    }
    anchor["line"] = std::max<std::int64_t>(0, line + shift);
    anchor["change"] = target;
    anchor["disposition"] = shift == 0 ? "same" : "shifted";
    return anchor;
}

auto map_anchors(state& shared, boost::http::route_params& params) -> boost::http::route_task {
    co_return co_await with_authorization(shared, params, [&]() -> boost::http::route_task {
        auto input = co_await request_body(params);
        if (!input) {
            co_return boost::http::route_error(input.error());
        }
        auto lock = std::unique_lock{shared.mutex};
        auto items = json::array();
        for (auto& comment : shared.comments) {
            auto& value = comment["value"];
            if (value["anchor"].value("change", "") != input->value("source_change", "") ||
                value["anchor"].value("path", "") != input->value("path", "")) {
                continue;
            }
            value["anchor"] =
                map_anchor(value["anchor"], input->at("hunks"), input->at("target_change"));
            comment["updated_at"] = timestamp(++shared.clock);
            items.push_back(comment);
        }
        lock.unlock();
        co_return co_await success(params, {{"items", std::move(items)}});
    });
}

auto make_forge_router(std::shared_ptr<state> shared, liquid::sync_server& sync)
    -> boost::http::router<boost::http::route_params>;

class worker_socket {
protected:
    explicit worker_socket(boost::corosio::io_context& context)
        : context_(context), socket_(context) {}

    boost::corosio::io_context& context_;
    boost::corosio::tcp_socket socket_;
};

class forge_worker final : private worker_socket,
                           public boost::corosio::tcp_server::worker_base,
                           public boost::http::http_worker {
public:
    forge_worker(boost::corosio::io_context& context,
                 boost::http::router<boost::http::route_params> router,
                 boost::http::shared_parser_config parser,
                 boost::http::shared_serializer_config serializer)
        : worker_socket(context),
          http_worker(socket_, std::move(router), std::move(parser), std::move(serializer)) {}

    auto socket() -> boost::corosio::tcp_socket& override {
        return socket_;
    }

    void run(boost::corosio::tcp_server::launcher launch) override {
        launch(context_.get_executor(), session());
    }

private:
    auto session() -> boost::capy::task<void> {
        co_await do_http_session();
        socket_.close();
    }
};

class tls_worker_socket {
protected:
    tls_worker_socket(boost::corosio::io_context& context, boost::corosio::tls_context& tls_context)
        : context_(context), socket_(context), secure_(&socket_, tls_context) {}

    boost::corosio::io_context& context_;
    boost::corosio::tcp_socket socket_;
    boost::corosio::openssl_stream secure_;
};

class forge_tls_worker final : private tls_worker_socket,
                               public boost::corosio::tcp_server::worker_base,
                               public boost::http::http_worker {
public:
    forge_tls_worker(boost::corosio::io_context& context,
                     boost::corosio::tls_context& tls_context,
                     boost::http::router<boost::http::route_params> router,
                     boost::http::shared_parser_config parser,
                     boost::http::shared_serializer_config serializer)
        : tls_worker_socket(context, tls_context),
          http_worker(secure_, std::move(router), std::move(parser), std::move(serializer)) {}

    auto socket() -> boost::corosio::tcp_socket& override {
        return socket_;
    }

    void run(boost::corosio::tcp_server::launcher launch) override {
        launch(context_.get_executor(), session());
    }

private:
    auto session() -> boost::capy::task<void> {
        const auto [handshake_error] =
            co_await secure_.handshake(boost::corosio::tls_stream::server);
        if (handshake_error) {
            socket_.close();
            co_return;
        }
        co_await do_http_session();
        static_cast<void>(co_await secure_.shutdown());
        socket_.close();
    }
};

auto workers(boost::corosio::io_context& context,
             boost::http::router<boost::http::route_params> router,
             boost::corosio::tls_context* tls_context)
    -> std::vector<std::unique_ptr<boost::corosio::tcp_server::worker_base>> {
    auto parser_options = boost::http::parser_config{true};
    parser_options.body_limit = maximum_body_size;
    auto parser = boost::http::make_parser_config(parser_options);
    auto serializer = boost::http::make_serializer_config(boost::http::serializer_config{});
    auto output = std::vector<std::unique_ptr<boost::corosio::tcp_server::worker_base>>{};
    for (auto count = 0; count < 16; ++count) {
        if (tls_context == nullptr) {
            output.push_back(std::make_unique<forge_worker>(context, router, parser, serializer));
        } else {
            output.push_back(std::make_unique<forge_tls_worker>(
                context, *tls_context, router, parser, serializer));
        }
    }
    return output;
}

class forge_http_server final : public boost::corosio::tcp_server {
public:
    forge_http_server(boost::corosio::io_context& context,
                      boost::http::router<boost::http::route_params> router,
                      boost::corosio::tls_context* tls_context)
        : tcp_server(context, context.get_executor()) {
        set_workers(workers(context, std::move(router), tls_context));
    }
};

auto bytes(const std::vector<unsigned char>& value) -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
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

auto query_parameter(std::string_view query, std::string_view name) -> std::string_view {
    while (!query.empty()) {
        const auto separator = query.find('&');
        const auto field = query.substr(0, separator);
        const auto equals = field.find('=');
        if (equals != std::string_view::npos && field.substr(0, equals) == name) {
            return field.substr(equals + 1);
        }
        if (separator == std::string_view::npos) {
            break;
        }
        query.remove_prefix(separator + 1);
    }
    return {};
}

auto number(std::string_view value) -> std::optional<std::uint64_t> {
    auto output = std::uint64_t{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), output);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return output;
}

auto sync_range(liquid::sync_server& sync, boost::http::route_params& params)
    -> boost::http::route_task {
    auto digest = liquid::parse_digest(route_parameter(params, "digest"));
    const auto encoded_query = params.url.encoded_query();
    const auto query = std::string_view{encoded_query.data(), encoded_query.size()};
    const auto offset = number(query_parameter(query, "offset"));
    const auto length = number(query_parameter(query, "length"));
    if (!digest || !offset || !length || *length > maximum_body_size) {
        co_return co_await send_bytes(
            params, boost::http::status::bad_request, "application/vnd.liquid.error-v1", {});
    }
    auto output = std::vector<std::byte>(static_cast<std::size_t>(*length));
    auto read = co_await sync.handle_chunks(*digest, *offset, output);
    if (!read) {
        auto encoded = liquid::sync_server::encode_error(read.error());
        co_return co_await send_bytes(params,
                                      boost::http::status::bad_request,
                                      "application/vnd.liquid.error-v1",
                                      encoded.value_or(std::vector<std::byte>{}));
    }
    output.resize(static_cast<std::size_t>(*read));
    co_return co_await send_bytes(
        params, boost::http::status::ok, "application/vnd.liquid.sync-v1", output);
}

auto sync_post(liquid::sync_server& sync,
               boost::http::route_params& params,
               std::string_view operation) -> boost::http::route_task {
    auto body = std::vector<unsigned char>{};
    auto [read_error, size] = co_await boost::capy::read(
        params.req_body, boost::capy::dynamic_buffer(body, maximum_body_size));
    static_cast<void>(size);
    if (read_error) {
        co_return boost::http::route_error(read_error);
    }
    if (operation == "chunks") {
        auto handled = co_await sync.handle_chunks(bytes(body));
        if (!handled) {
            auto encoded = liquid::sync_server::encode_error(handled.error());
            co_return co_await send_bytes(params,
                                          boost::http::status::bad_request,
                                          "application/vnd.liquid.error-v1",
                                          encoded.value_or(std::vector<std::byte>{}));
        }
        co_return co_await send_bytes(
            params, boost::http::status::ok, "application/vnd.liquid.sync-v1", {});
    }
    auto handled = operation == "pull"   ? sync.handle_pull(bytes(body))
                   : operation == "push" ? sync.handle_push(bytes(body))
                                         : sync.handle_find_missing(bytes(body));
    auto result = co_await std::move(handled);
    if (!result) {
        auto encoded = liquid::sync_server::encode_error(result.error());
        co_return co_await send_bytes(params,
                                      boost::http::status::bad_request,
                                      "application/vnd.liquid.error-v1",
                                      encoded.value_or(std::vector<std::byte>{}));
    }
    co_return co_await send_bytes(
        params, boost::http::status::ok, "application/vnd.liquid.sync-v1", *result);
}

auto make_forge_router(std::shared_ptr<state> shared, liquid::sync_server& sync)
    -> boost::http::router<boost::http::route_params> {
    auto router = boost::http::router<boost::http::route_params>{};
    router.add(boost::http::method::get,
               "/v1/list_reviews",
               [shared](boost::http::route_params& params) -> boost::http::route_task {
                   co_return co_await list_records(*shared, params, &state::reviews);
               });
    router.add(boost::http::method::post,
               "/v1/create_review",
               [shared](boost::http::route_params& params) -> boost::http::route_task {
                   co_return co_await create_review(*shared, params);
               });
    router.add(boost::http::method::post,
               "/v1/update_review",
               [shared](boost::http::route_params& params) -> boost::http::route_task {
                   co_return co_await update_review(*shared, params);
               });
    router.add(boost::http::method::get,
               "/v1/list_review_comments",
               [shared](boost::http::route_params& params) -> boost::http::route_task {
                   co_return co_await list_records(*shared, params, &state::comments);
               });
    router.add(boost::http::method::post,
               "/v1/create_review_comment",
               [shared](boost::http::route_params& params) -> boost::http::route_task {
                   co_return co_await create_comment(*shared, params);
               });
    router.add(boost::http::method::post,
               "/v1/map_review_anchors",
               [shared](boost::http::route_params& params) -> boost::http::route_task {
                   co_return co_await map_anchors(*shared, params);
               });
    router.add(boost::http::method::get,
               "/v1/list_branch_protections",
               [shared](boost::http::route_params& params) -> boost::http::route_task {
                   co_return co_await list_records(*shared, params, &state::protections);
               });
    router.add(boost::http::method::post,
               "/v1/create_deployment",
               [shared](boost::http::route_params& params) -> boost::http::route_task {
                   co_return co_await create_deployment(*shared, params);
               });
    router.add(boost::http::method::get,
               "/v1/list_deployments",
               [shared](boost::http::route_params& params) -> boost::http::route_task {
                   co_return co_await list_deployments(*shared, params);
               });
    router.add(boost::http::method::post,
               "/v1/rollback_deployment",
               [shared](boost::http::route_params& params) -> boost::http::route_task {
                   co_return co_await rollback_deployment(*shared, params);
               });
    router.add(boost::http::method::get,
               "/v1/deployment_status",
               [shared](boost::http::route_params& params) -> boost::http::route_task {
                   co_return co_await deployment_status(*shared, params);
               });
    for (const auto operation : {"pull", "push", "find-missing", "chunks"}) {
        router.add(
            boost::http::method::post,
            "/v1/repositories/test/sync/" + std::string{operation},
            [&sync, operation](boost::http::route_params& params) -> boost::http::route_task {
                const auto name = std::string_view{operation} == "find-missing"
                                      ? std::string_view{"missing"}
                                      : std::string_view{operation};
                co_return co_await sync_post(sync, params, name);
            });
    }
    router.add(boost::http::method::get,
               "/v1/repositories/test/sync/chunks/:digest",
               [&sync](boost::http::route_params& params) -> boost::http::route_task {
                   co_return co_await sync_range(sync, params);
               });
    return router;
}

auto temporary_path() -> std::filesystem::path {
    auto random = std::random_device{};
    return std::filesystem::temp_directory_path() /
           ("liquid-forge-double-" + std::to_string(random()) + '-' + std::to_string(random()));
}

} // namespace

struct forge_double::impl {
    std::shared_ptr<state> shared{std::make_shared<state>()};
    std::filesystem::path path{temporary_path()};
    liquid::repository repository;
    std::jthread thread;
    std::string address{"127.0.0.1"};
    std::uint16_t requested_port{};
    std::uint16_t port{};
    bool tls{};
};

forge_double::forge_double(std::string bearer_token,
                           std::string listen,
                           std::string certificate,
                           std::string private_key)
    : impl_(std::make_unique<impl>()) {
    const auto colon = listen.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == listen.size()) {
        throw std::invalid_argument("listen address must be <address>:<port>");
    }
    auto port = std::uint64_t{};
    const auto port_text = std::string_view{listen}.substr(colon + 1);
    const auto parsed =
        std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    if (parsed.ec != std::errc{} || parsed.ptr != port_text.data() + port_text.size() ||
        port > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument("listen port must be between 0 and 65535");
    }
    impl_->address = listen.substr(0, colon);
    impl_->requested_port = static_cast<std::uint16_t>(port);
    if (certificate.empty() != private_key.empty()) {
        throw std::invalid_argument("certificate and private key must be provided together");
    }
    impl_->tls = !certificate.empty();
    impl_->shared->bearer_token = std::move(bearer_token);
    auto repository = liquid::runtime::block_on(liquid::repository::create(impl_->path));
    if (!repository) {
        throw std::runtime_error(repository.error().message);
    }
    impl_->repository = std::move(*repository);
    auto written = liquid::runtime::block_on(impl_->repository.write("README.md", {}));
    if (!written) {
        throw std::runtime_error(written.error().message);
    }
    auto cut = liquid::runtime::block_on(impl_->repository.cut("forge double initial head"));
    if (!cut) {
        throw std::runtime_error(cut.error().message);
    }
    auto ready_port = std::promise<std::uint16_t>{};
    auto ready = ready_port.get_future();
    impl_->thread =
        std::jthread([this,
                      ready_port = std::move(ready_port),
                      certificate = std::move(certificate),
                      private_key = std::move(private_key)](std::stop_token stop) mutable {
            try {
                auto sync = liquid::sync_server{impl_->repository};
                auto context = boost::corosio::io_context{};
                auto tls_context = std::optional<boost::corosio::tls_context>{};
                if (!certificate.empty()) {
                    tls_context.emplace();
                    if (const auto error = tls_context->use_certificate_file(
                            certificate, boost::corosio::tls_file_format::pem)) {
                        throw std::runtime_error(error.message());
                    }
                    if (const auto error = tls_context->use_private_key_file(
                            private_key, boost::corosio::tls_file_format::pem)) {
                        throw std::runtime_error(error.message());
                    }
                }
                auto server = forge_http_server{context,
                                                make_forge_router(impl_->shared, sync),
                                                tls_context ? &*tls_context : nullptr};
                auto address = boost::corosio::endpoint{impl_->address};
                const auto code =
                    server.bind(boost::corosio::endpoint{address, impl_->requested_port});
                if (code) {
                    ready_port.set_exception(
                        std::make_exception_ptr(std::runtime_error(code.message())));
                    return;
                }
                ready_port.set_value(server.local_endpoint().port());
                server.start();
                while (!stop.stop_requested()) {
                    context.run_for(std::chrono::milliseconds{20});
                    context.restart();
                }
                server.stop();
                context.run();
                server.join();
            } catch (...) {
                try {
                    ready_port.set_exception(std::current_exception());
                } catch (...) {
                }
            }
        });
    impl_->port = ready.get();
}

forge_double::~forge_double() {
    if (!impl_) {
        return;
    }
    impl_->thread.request_stop();
    impl_->thread.join();
    const auto path = impl_->path;
    impl_.reset();
    auto ignored = std::error_code{};
    std::filesystem::remove_all(path, ignored);
}

forge_double::forge_double(forge_double&&) noexcept = default;
forge_double& forge_double::operator=(forge_double&&) noexcept = default;

auto forge_double::url() const -> std::string {
    const auto host =
        impl_->tls ? "localhost" : (impl_->address == "0.0.0.0" ? "127.0.0.1" : impl_->address);
    const auto scheme = impl_->tls ? "https://" : "http://";
    return scheme + host + ':' + std::to_string(impl_->port) + "/v1/repositories/test";
}

auto forge_double::last_authorization() const -> std::string {
    auto lock = std::unique_lock{impl_->shared->mutex};
    return impl_->shared->last_authorization;
}

auto forge_double::add_comment(std::string review, std::string body, text_anchor anchor)
    -> std::string {
    auto lock = std::unique_lock{impl_->shared->mutex};
    auto value = json{{"review_id", std::move(review)},
                      {"body", std::move(body)},
                      {"anchor",
                       {{"change", to_string(anchor.change.value)},
                        {"path", std::move(anchor.path)},
                        {"line", anchor.line},
                        {"column", anchor.column}}}};
    impl_->shared->comments.push_back(record(*impl_->shared, "review_comment", std::move(value)));
    return impl_->shared->comments.back().at("id").get<std::string>();
}

auto forge_double::advance_head(std::string path, std::string contents) -> result<change_id> {
    const auto* first = reinterpret_cast<const std::byte*>(contents.data());
    auto written = liquid::runtime::block_on(impl_->repository.write(
        std::move(path), std::vector<std::byte>{first, first + contents.size()}));
    if (!written) {
        return std::unexpected(written.error());
    }
    return liquid::runtime::block_on(impl_->repository.cut("forge double head move"));
}

void forge_double::add_branch_protection(forge_branch_protection rule) {
    auto lock = std::unique_lock{impl_->shared->mutex};
    auto value = json{{"ref", std::move(rule.pattern)},
                      {"required_review", rule.required_review},
                      {"required_verification", rule.required_verification},
                      {"fast_forward_only", rule.fast_forward_only},
                      {"allowed_pushers", std::move(rule.allowed_pushers)}};
    impl_->shared->protections.push_back(
        record(*impl_->shared, "branch_protection", std::move(value)));
}

void forge_double::fail_next(std::string code, std::string message, std::string path) {
    auto lock = std::unique_lock{impl_->shared->mutex};
    impl_->shared->next_failure = failure{std::move(code), std::move(message), std::move(path)};
}

} // namespace liquid::test
