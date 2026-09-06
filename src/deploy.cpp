#include "liquid/deploy.hpp"

#include <cctype>
#include <format>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <boost/http.hpp>
#include <nlohmann/json.hpp>

#include "liquid/repository.hpp"
#include "net/http_client.hpp"

namespace liquid::deploy {
namespace {

using nlohmann::json;

auto deployment_cache = std::unordered_map<std::string, deployment_record>{};
auto deployment_cache_mutex = std::mutex{};

auto failure(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto repository_name(const repository& source) -> result<std::string> {
    auto id = source.id();
    if (!id) {
        return std::unexpected(id.error());
    }
    auto output = std::string{};
    output.reserve(id->size() * 2);
    for (const auto byte : *id) {
        output += std::format("{:02x}", std::to_integer<unsigned char>(byte));
    }
    return output;
}

auto valid_image(std::string_view image) -> bool {
    const auto marker = image.rfind("@sha256:");
    if (marker == std::string_view::npos || marker == 0 || marker + 72 != image.size()) {
        return false;
    }
    return std::ranges::all_of(image.substr(marker + 8), [](unsigned char character) {
        return std::isxdigit(character) != 0;
    });
}

auto call(const net_detail::parsed_url& url,
          std::string_view token,
          boost::http::method method,
          std::string target,
          json input) -> runtime::task<json> {
    auto encoded = method == boost::http::method::get ? std::string{} : input.dump();
    const auto* first = reinterpret_cast<const std::byte*>(encoded.data());
    auto response = co_await net_detail::perform_request({
        .method = method == boost::http::method::get ? net_detail::http_method::get
                                                     : net_detail::http_method::post,
        .url = url,
        .target = std::move(target),
        .bearer = std::string{token},
        .content_type = "application/json",
        .body = {first, first + encoded.size()},
    });
    if (!response) {
        co_return std::unexpected(response.error());
    }
    try {
        const auto body = std::string_view{reinterpret_cast<const char*>(response->body.data()),
                                           response->body.size()};
        const auto envelope = json::parse(body);
        if (!envelope.is_object() || !envelope.contains("ok") || !envelope.at("ok").is_boolean()) {
            co_return std::unexpected(failure(errc::corrupt, "invalid control-plane envelope"));
        }
        if (envelope.at("ok").get<bool>()) {
            if (!envelope.contains("value")) {
                co_return std::unexpected(
                    failure(errc::corrupt, "successful control-plane envelope has no value"));
            }
            co_return envelope.at("value");
        }
        const auto& detail = envelope.value("error", json::object());
        const auto code_name = detail.value("code", "internal");
        auto code = errc::internal;
        if (code_name == "invalid_input") {
            code = errc::invalid_argument;
        } else if (code_name == "unauthorized") {
            code = errc::unauthorized;
        } else if (code_name == "not_found") {
            code = errc::not_found;
        } else if (code_name == "conflict") {
            code = errc::conflict;
        } else if (code_name == "unavailable") {
            code = errc::unavailable;
        }
        co_return std::unexpected(failure(code,
                                          detail.value("message", "control-plane request failed"),
                                          detail.value("path", std::string{})));
    } catch (const json::exception& exception) {
        co_return std::unexpected(
            failure(errc::corrupt, exception.what(), "control-plane response"));
    }
}

auto parse_change(const json& value) -> result<change_id> {
    auto parsed = parse_digest(value.get<std::string>());
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return change_id{*parsed};
}

auto parse_record(const json& value) -> result<deployment_record> {
    try {
        auto change = parse_change(value.at("change"));
        if (!change) {
            return std::unexpected(change.error());
        }
        auto output = deployment_record{.id = value.at("id").get<std::string>(),
                                        .repository = value.at("repository").get<std::string>(),
                                        .environment = value.at("environment").get<std::string>(),
                                        .change = *change,
                                        .image = value.at("image").get<std::string>()};
        if (value.contains("result") && !value.at("result").is_null()) {
            auto result = parse_digest(value.at("result").get<std::string>());
            if (!result) {
                return std::unexpected(result.error());
            }
            output.result = result_id{*result};
        }
        if (value.contains("rollback_of") && !value.at("rollback_of").is_null()) {
            output.rollback_of = value.at("rollback_of").get<std::string>();
        }
        return output;
    } catch (const json::exception& exception) {
        return std::unexpected(failure(errc::corrupt, exception.what(), "deployment record"));
    }
}

auto endpoint(std::string_view control_plane_url) -> result<net_detail::parsed_url> {
    return net_detail::parse_url(control_plane_url, "control-plane URL");
}

auto target(const net_detail::parsed_url&, std::string_view suffix) -> std::string {
    return std::string{suffix};
}

auto cache_key(std::string_view control_plane_url,
               std::string_view repository,
               std::string_view environment,
               const change_id& change,
               std::string_view image) -> std::string {
    return std::string{control_plane_url} + '\n' + std::string{repository} + '\n' +
           std::string{environment} + '\n' + to_string(change.value) + '\n' + std::string{image};
}

} // namespace

runtime::task<deployment_record> create(repository& source,
                                        std::string control_plane_url,
                                        deploy_request request,
                                        deploy_options options) {
    if (request.environment.empty() || !valid_image(request.image)) {
        co_return std::unexpected(failure(
            errc::invalid_argument, "environment and digest-pinned image are required", "deploy"));
    }
    auto remote = endpoint(control_plane_url);
    if (!remote) {
        co_return std::unexpected(remote.error());
    }
    auto repository = repository_name(source);
    if (!repository) {
        co_return std::unexpected(repository.error());
    }
    if (!request.change) {
        auto head = co_await source.head();
        if (!head) {
            co_return std::unexpected(head.error());
        }
        request.change = *head;
    }
    const auto key = cache_key(
        control_plane_url, *repository, request.environment, *request.change, request.image);
    {
        const auto lock = std::lock_guard{deployment_cache_mutex};
        if (const auto found = deployment_cache.find(key); found != deployment_cache.end()) {
            co_return found->second;
        }
    }
    auto input = json{{"repository", *repository},
                      {"environment", request.environment},
                      {"change", to_string(request.change->value)},
                      {"image", request.image},
                      {"secrets", request.secret_references}};
    if (request.result) {
        input["result"] = to_string(request.result->value);
    }
    auto value = co_await call(*remote,
                               options.bearer_token,
                               boost::http::method::post,
                               target(*remote, "/v1/create_deployment"),
                               std::move(input));
    if (!value) {
        co_return std::unexpected(value.error());
    }
    auto parsed = parse_record(*value);
    if (!parsed) {
        co_return std::unexpected(parsed.error());
    }
    {
        const auto lock = std::lock_guard{deployment_cache_mutex};
        deployment_cache.insert_or_assign(key, *parsed);
    }
    co_return std::move(*parsed);
}

runtime::task<std::vector<deployment_record>>
list(repository& source, std::string control_plane_url, deploy_options options) {
    auto remote = endpoint(control_plane_url);
    auto repository = repository_name(source);
    if (!remote || !repository) {
        co_return std::unexpected(remote ? repository.error() : remote.error());
    }
    auto value = co_await call(*remote,
                               options.bearer_token,
                               boost::http::method::get,
                               target(*remote, "/v1/list_deployments?repository=" + *repository),
                               json::object());
    if (!value) {
        co_return std::unexpected(value.error());
    }
    try {
        auto output = std::vector<deployment_record>{};
        for (const auto& item : value->at("items")) {
            auto parsed = parse_record(item);
            if (!parsed) {
                co_return std::unexpected(parsed.error());
            }
            output.push_back(std::move(*parsed));
        }
        co_return output;
    } catch (const json::exception& exception) {
        co_return std::unexpected(failure(errc::corrupt, exception.what(), "deployment list"));
    }
}

runtime::task<deployment_record> rollback(repository& source,
                                          std::string control_plane_url,
                                          std::string environment,
                                          std::string deployment_or_change,
                                          deploy_options options) {
    auto remote = endpoint(control_plane_url);
    auto repository = repository_name(source);
    if (!remote || !repository || environment.empty() || deployment_or_change.empty()) {
        co_return std::unexpected(
            !remote ? remote.error()
            : !repository
                ? repository.error()
                : failure(errc::invalid_argument, "environment and rollback target are required"));
    }
    auto value = co_await call(*remote,
                               options.bearer_token,
                               boost::http::method::post,
                               target(*remote, "/v1/rollback_deployment"),
                               {{"repository", *repository},
                                {"environment", std::move(environment)},
                                {"to", std::move(deployment_or_change)}});
    if (!value) {
        co_return std::unexpected(value.error());
    }
    co_return parse_record(*value);
}

runtime::task<environment_status> status(repository& source,
                                         std::string control_plane_url,
                                         std::string environment,
                                         deploy_options options) {
    auto remote = endpoint(control_plane_url);
    auto repository = repository_name(source);
    if (!remote || !repository || environment.empty()) {
        co_return std::unexpected(!remote ? remote.error()
                                  : !repository
                                      ? repository.error()
                                      : failure(errc::invalid_argument, "environment is required"));
    }
    auto value = co_await call(
        *remote,
        options.bearer_token,
        boost::http::method::get,
        target(*remote,
               "/v1/deployment_status?repository=" + *repository + "&environment=" + environment),
        json::object());
    if (!value) {
        co_return std::unexpected(value.error());
    }
    try {
        co_return environment_status{.phase = value->at("phase").get<std::string>(),
                                     .url = value->value("url", std::string{}),
                                     .ready = value->at("ready").get<bool>(),
                                     .message = value->value("message", std::string{})};
    } catch (const json::exception& exception) {
        co_return std::unexpected(failure(errc::corrupt, exception.what(), "environment status"));
    }
}

} // namespace liquid::deploy
