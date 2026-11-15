#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <boost/capy/buffers.hpp>
#include <boost/capy/buffers/buffer_copy.hpp>
#include <boost/capy/buffers/vector_dynamic_buffer.hpp>
#include <boost/capy/read.hpp>
#include <boost/corosio.hpp>
#include <boost/corosio/openssl_stream.hpp>
#include <boost/corosio/tls_context.hpp>
#include <boost/http.hpp>
#include <boost/http/server/http_worker.hpp>

#include "liquid/admission.hpp"
#include "liquid/error.hpp"
#include "liquid/listener.hpp"
#include "liquid/reapi.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "liquid/sync.hpp"

namespace liquid_cli {
namespace {

constexpr auto sync_content_type = "application/vnd.liquid.sync-v1";
constexpr auto error_content_type = "application/vnd.liquid.error-v1";
constexpr auto maximum_body_size = std::size_t{64} * 1024 * 1024;

enum class post_operation { push, pull, find_missing, chunks };

auto fail(const liquid::error& error) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(error.code), error.message, error.context);
    return 1;
}

auto usage() -> int {
    std::println(stderr,
                 "error: usage: serve sync <path> [--listen <address>] [--port <port>] "
                 "[--allow-ref <glob>]... [--tls-cert <path> --tls-key <path>] "
                 "[--token-file <path>] [--client-ca <path>] [--upload-quota <bytes>]");
    return 2;
}

auto usage(std::string_view message) -> int {
    std::println(stderr, "error: usage: {} ()", message);
    return 2;
}

auto status_for(const liquid::error& error) -> boost::http::status {
    switch (error.code) {
    case liquid::errc::invalid_argument:
    case liquid::errc::corrupt:
        return boost::http::status::bad_request;
    case liquid::errc::unauthorized:
        return boost::http::status::unauthorized;
    case liquid::errc::not_found:
        return boost::http::status::not_found;
    case liquid::errc::already_exists:
    case liquid::errc::conflict:
        return boost::http::status::conflict;
    case liquid::errc::locked:
        return boost::http::status::locked;
    case liquid::errc::busy:
        return boost::http::status::too_many_requests;
    case liquid::errc::unavailable:
        return boost::http::status::service_unavailable;
    default:
        return boost::http::status::internal_server_error;
    }
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

auto send_error(boost::http::route_params& params, const liquid::error& error)
    -> boost::http::route_task {
    auto encoded = liquid::sync_server::encode_error(error);
    if (!encoded) {
        co_return co_await send_bytes(
            params, boost::http::status::internal_server_error, error_content_type, {});
    }
    co_return co_await send_bytes(params, status_for(error), error_content_type, *encoded);
}

auto request_body(boost::http::route_params& params)
    -> boost::capy::task<std::expected<std::vector<unsigned char>, std::error_code>> {
    auto body = std::vector<unsigned char>{};
    auto [error, size] = co_await boost::capy::read(
        params.req_body, boost::capy::dynamic_buffer(body, maximum_body_size));
    static_cast<void>(size);
    if (error) {
        co_return std::unexpected(error);
    }
    co_return body;
}

auto as_bytes(const std::vector<unsigned char>& value) -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

auto handle_post(liquid::sync_server& server,
                 boost::http::route_params& params,
                 post_operation operation) -> boost::http::route_task {
    auto body = co_await request_body(params);
    if (!body) {
        co_return boost::http::route_error(body.error());
    }
    if (operation == post_operation::chunks) {
        auto handled = co_await server.handle_chunks(as_bytes(*body));
        if (!handled) {
            co_return co_await send_error(params, handled.error());
        }
        co_return co_await send_bytes(params, boost::http::status::ok, sync_content_type, {});
    }
    auto pending = operation == post_operation::push ? server.handle_push(as_bytes(*body))
                   : operation == post_operation::pull
                       ? server.handle_pull(as_bytes(*body))
                       : server.handle_find_missing(as_bytes(*body));
    auto handled = co_await std::move(pending);
    if (!handled) {
        co_return co_await send_error(params, handled.error());
    }
    co_return co_await send_bytes(params, boost::http::status::ok, sync_content_type, *handled);
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

auto parse_number(std::string_view value) -> std::optional<std::uint64_t> {
    auto output = std::uint64_t{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), output);
    if (value.empty() || result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return output;
}

auto handle_range(liquid::sync_server& server, boost::http::route_params& params)
    -> boost::http::route_task {
    auto id = liquid::parse_digest(route_parameter(params, "digest"));
    const auto encoded_query = params.url.encoded_query();
    const auto query = std::string_view{encoded_query.data(), encoded_query.size()};
    const auto offset = parse_number(query_parameter(query, "offset"));
    const auto length = parse_number(query_parameter(query, "length"));
    if (!id || !offset || !length || *length > maximum_body_size ||
        *length > std::numeric_limits<std::size_t>::max()) {
        co_return co_await send_error(
            params,
            liquid::error{liquid::errc::invalid_argument, "invalid chunk range", "sync server"});
    }
    auto body = std::vector<std::byte>(static_cast<std::size_t>(*length));
    auto read = co_await server.handle_chunks(*id, *offset, body);
    if (!read) {
        co_return co_await send_error(params, read.error());
    }
    body.resize(static_cast<std::size_t>(*read));
    co_return co_await send_bytes(params, boost::http::status::ok, sync_content_type, body);
}

// True when no bearer token is configured, or the request carries exactly one `Authorization`
// header holding it. The comparison is constant-time (liquid#403).
auto authorized(const std::string& token, const boost::http::route_params& params) -> bool {
    if (token.empty()) {
        return true;
    }
    if (params.req.count(boost::http::field::authorization) != 1) {
        return false;
    }
    const auto value = params.req.value_or(boost::http::field::authorization, "");
    return liquid::bearer_authorized(token, std::string_view{value.data(), value.size()});
}

// Refuses an unauthenticated request before its body is read.
auto send_unauthorized(boost::http::route_params& params) -> boost::http::route_task {
    co_return co_await send_error(params,
                                  liquid::error{liquid::errc::unauthorized,
                                                "repository authentication required",
                                                "Authorization"});
}

auto make_router(liquid::sync_server& server, const std::string& token)
    -> boost::http::router<boost::http::route_params> {
    auto router = boost::http::router<boost::http::route_params>{};
    const auto* required = &token;
    router.add(boost::http::method::post,
               "/sync/push",
               [&server, required](boost::http::route_params& params) -> boost::http::route_task {
                   if (!authorized(*required, params)) {
                       co_return co_await send_unauthorized(params);
                   }
                   co_return co_await handle_post(server, params, post_operation::push);
               });
    router.add(boost::http::method::post,
               "/sync/pull",
               [&server, required](boost::http::route_params& params) -> boost::http::route_task {
                   if (!authorized(*required, params)) {
                       co_return co_await send_unauthorized(params);
                   }
                   co_return co_await handle_post(server, params, post_operation::pull);
               });
    router.add(boost::http::method::post,
               "/sync/find-missing",
               [&server, required](boost::http::route_params& params) -> boost::http::route_task {
                   if (!authorized(*required, params)) {
                       co_return co_await send_unauthorized(params);
                   }
                   co_return co_await handle_post(server, params, post_operation::find_missing);
               });
    router.add(boost::http::method::post,
               "/sync/chunks",
               [&server, required](boost::http::route_params& params) -> boost::http::route_task {
                   if (!authorized(*required, params)) {
                       co_return co_await send_unauthorized(params);
                   }
                   co_return co_await handle_post(server, params, post_operation::chunks);
               });
    router.add(boost::http::method::get,
               "/sync/chunks/:digest",
               [&server, required](boost::http::route_params& params) -> boost::http::route_task {
                   if (!authorized(*required, params)) {
                       co_return co_await send_unauthorized(params);
                   }
                   co_return co_await handle_range(server, params);
               });
    return router;
}

class worker_socket {
protected:
    explicit worker_socket(boost::corosio::io_context& context)
        : context_(context), socket_(context) {}

    boost::corosio::io_context& context_;
    boost::corosio::tcp_socket socket_;
};

// One accepted connection at a time. With TLS the socket is wrapped in Corosio's OpenSSL stream;
// a failed handshake, including a missing or untrusted client certificate, closes the connection.
class sync_worker final : private worker_socket, public boost::corosio::tcp_server::worker_base {
public:
    sync_worker(boost::corosio::io_context& context,
                boost::http::router<boost::http::route_params> router,
                boost::http::shared_parser_config parser,
                boost::http::shared_serializer_config serializer,
                std::shared_ptr<boost::corosio::tls_context> tls)
        : worker_socket(context), router_(std::move(router)), parser_(std::move(parser)),
          serializer_(std::move(serializer)), tls_(std::move(tls)) {}

    auto socket() -> boost::corosio::tcp_socket& override {
        return socket_;
    }

    void run(boost::corosio::tcp_server::launcher launch) override {
        launch(context_.get_executor(), session());
    }

private:
    auto session() -> boost::capy::task<void> {
        if (!tls_) {
            auto worker = boost::http::http_worker{socket_, router_, parser_, serializer_};
            co_await worker.do_http_session();
        } else {
            auto secure = boost::corosio::openssl_stream{&socket_, *tls_};
            auto [handshake_error] =
                co_await secure.handshake(boost::corosio::openssl_stream::server);
            if (!handshake_error) {
                auto worker = boost::http::http_worker{secure, router_, parser_, serializer_};
                co_await worker.do_http_session();
            }
        }
        socket_.close();
    }

    boost::http::router<boost::http::route_params> router_;
    boost::http::shared_parser_config parser_;
    boost::http::shared_serializer_config serializer_;
    std::shared_ptr<boost::corosio::tls_context> tls_;
};

auto make_workers(boost::corosio::io_context& context,
                  boost::http::router<boost::http::route_params> router,
                  const std::shared_ptr<boost::corosio::tls_context>& tls)
    -> std::vector<std::unique_ptr<boost::corosio::tcp_server::worker_base>> {
    auto parser_options = boost::http::parser_config{true};
    parser_options.body_limit = maximum_body_size;
    auto parser = boost::http::make_parser_config(parser_options);
    auto serializer = boost::http::make_serializer_config(boost::http::serializer_config{});
    auto workers = std::vector<std::unique_ptr<boost::corosio::tcp_server::worker_base>>{};
    for (auto count = 0; count < 32; ++count) {
        workers.push_back(std::make_unique<sync_worker>(context, router, parser, serializer, tls));
    }
    return workers;
}

class sync_http_server final : public boost::corosio::tcp_server {
public:
    sync_http_server(boost::corosio::io_context& context,
                     boost::http::router<boost::http::route_params> router,
                     const std::shared_ptr<boost::corosio::tls_context>& tls)
        : tcp_server(context, context.get_executor()) {
        set_workers(make_workers(context, std::move(router), tls));
    }
};

// The TLS context of the sync listener: its certificate chain and key, and, with a client CA, a
// required client certificate that CA signs (mutual TLS). Null when TLS is not configured.
auto sync_tls_context(const liquid::listener_security& security)
    -> liquid::result<std::shared_ptr<boost::corosio::tls_context>> {
    if (!security.tls_certificate || !security.tls_key) {
        return std::shared_ptr<boost::corosio::tls_context>{};
    }
    const auto refused = [](std::error_code code, std::string context) {
        return std::unexpected(
            liquid::error{liquid::errc::invalid_argument, code.message(), std::move(context)});
    };
    auto tls = std::make_shared<boost::corosio::tls_context>();
    const auto certificate = security.tls_certificate->string();
    if (const auto code = tls->use_certificate_chain_file(certificate)) {
        return refused(code, certificate);
    }
    const auto key = security.tls_key->string();
    if (const auto code = tls->use_private_key_file(key, boost::corosio::tls_file_format::pem)) {
        return refused(code, key);
    }
    if (const auto code = tls->set_alpn({"http/1.1"})) {
        return refused(code, "sync TLS ALPN");
    }
    if (security.client_certificate_authority) {
        const auto authority = security.client_certificate_authority->string();
        if (const auto code = tls->load_verify_file(authority)) {
            return refused(code, authority);
        }
        if (const auto code = tls->set_verify_mode(boost::corosio::tls_verify_mode::require_peer)) {
            return refused(code, "sync client TLS");
        }
    }
    return tls;
}

struct serve_options {
    std::filesystem::path path;
    std::string address{"127.0.0.1"};
    std::uint16_t port{8080};
    std::vector<std::string> allowed_refs;
    liquid::listener_security security;
    std::optional<std::filesystem::path> token_file;
    std::optional<std::uint64_t> upload_quota;
};

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

auto reference_authorizer(std::vector<std::string> patterns) -> liquid::sync_authorizer {
    return [patterns = std::move(patterns)](const liquid::sync_item& item,
                                            std::span<const liquid::sync_ref_update> updates)
               -> liquid::runtime::task<std::vector<liquid::sync_ref_verdict>> {
        auto verdicts = std::vector<liquid::sync_ref_verdict>{};
        verdicts.reserve(updates.size());
        for (const auto& update : updates) {
            auto verdict = liquid::sync_ref_verdict{update.name};
            const auto allowed = item.kind != liquid::sync_item_kind::reference ||
                                 std::ranges::any_of(patterns, [&](const auto& pattern) {
                                     return glob_matches(pattern, update.name);
                                 });
            if (!allowed) {
                verdict.verdict = liquid::sync_ref_verdict_kind::refused;
                verdict.reason = "reference is not allowed";
            }
            verdicts.push_back(std::move(verdict));
        }
        co_return verdicts;
    };
}

auto parse_options(int argc, char** argv) -> std::optional<serve_options> {
    if (argc < 2 || std::string_view{argv[0]} != "sync") {
        return std::nullopt;
    }
    auto options = serve_options{.path = argv[1]};
    for (auto index = 2; index < argc; index += 2) {
        if (index + 1 >= argc) {
            return std::nullopt;
        }
        const auto name = std::string_view{argv[index]};
        const auto value = std::string_view{argv[index + 1]};
        if (name == "--listen" && !value.empty()) {
            options.address = value;
            continue;
        }
        if (name == "--allow-ref" && !value.empty()) {
            options.allowed_refs.emplace_back(value);
            continue;
        }
        if (name == "--tls-cert" && !value.empty()) {
            options.security.tls_certificate = std::filesystem::path{value};
            continue;
        }
        if (name == "--tls-key" && !value.empty()) {
            options.security.tls_key = std::filesystem::path{value};
            continue;
        }
        if (name == "--client-ca" && !value.empty()) {
            options.security.client_certificate_authority = std::filesystem::path{value};
            continue;
        }
        if (name == "--token-file" && !value.empty()) {
            options.token_file = std::filesystem::path{value};
            continue;
        }
        if (name == "--upload-quota") {
            const auto quota = parse_number(value);
            if (!quota || *quota == 0) {
                return std::nullopt;
            }
            options.upload_quota = *quota;
            continue;
        }
        if (name != "--port") {
            return std::nullopt;
        }
        auto port = std::uint64_t{};
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), port);
        if (value.empty() || parsed.ec != std::errc{} ||
            parsed.ptr != value.data() + value.size() || port == 0 ||
            port > std::numeric_limits<std::uint16_t>::max()) {
            return std::nullopt;
        }
        options.port = static_cast<std::uint16_t>(port);
    }
    return options;
}

// The host of a `host:port` listen address: `[v6]:port`, `v4:port`, or the whole value.
auto listen_host(std::string_view listen) -> std::string_view {
    if (listen.starts_with('[')) {
        const auto close = listen.find(']');
        return close == std::string_view::npos ? listen : listen.substr(0, close + 1);
    }
    const auto colon = listen.rfind(':');
    return colon == std::string_view::npos ? listen : listen.substr(0, colon);
}

// Applies one transport-security option shared by the REAPI and sync listeners. Returns nullopt
// when `name` is not one; otherwise the exit status on failure, or 0. The bearer token is read
// from an owner-only file, never from argv or the environment.
auto security_option(liquid::listener_security& security,
                     std::string_view name,
                     std::string_view value) -> std::optional<int> {
    if (name == "--tls-cert") {
        security.tls_certificate = std::filesystem::path{value};
        return 0;
    }
    if (name == "--tls-key") {
        security.tls_key = std::filesystem::path{value};
        return 0;
    }
    if (name == "--client-ca") {
        security.client_certificate_authority = std::filesystem::path{value};
        return 0;
    }
    if (name == "--token-file") {
        auto token = liquid::read_token_file(std::filesystem::path{value});
        if (!token) {
            return fail(token.error());
        }
        security.bearer_token = std::move(*token);
        return 0;
    }
    return std::nullopt;
}

auto serve_reapi(int argc, char** argv) -> int {
    auto options = liquid::reapi_serve_options{.store_directory = std::filesystem::current_path() /
                                                                  ".liquid-store"};
    for (auto index = 1; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--listen") {
            if (++index == argc) {
                return usage("serve reapi --listen requires an address");
            }
            options.listen = argv[index];
            continue;
        }
        if (argument == "--instance") {
            if (++index == argc) {
                return usage("serve reapi --instance requires a name");
            }
            options.instance_name = argv[index];
            continue;
        }
        if (argument == "--tls-cert" || argument == "--tls-key" || argument == "--client-ca" ||
            argument == "--token-file") {
            if (++index == argc) {
                return usage("serve reapi security options require a path");
            }
            const auto applied = security_option(options.security, argument, argv[index]);
            if (applied && *applied != 0) {
                return *applied;
            }
            continue;
        }
        return usage("serve reapi --listen <address> [--instance <name>] [--tls-cert <path> "
                     "--tls-key <path>] [--token-file <path>] [--client-ca <path>]");
    }
    const auto secured = liquid::validate_listener_security(
        liquid::is_loopback_address(listen_host(options.listen)), options.security);
    if (!secured) {
        return fail(secured.error());
    }
    if (!std::filesystem::exists(options.store_directory)) {
        return usage("serve reapi must run from a liquid repository");
    }
    std::println("REAPI listening on {}", options.listen);
    auto served = liquid::runtime::block_on(liquid::serve_reapi(std::move(options)));
    return served ? 0 : fail(served.error());
}

auto environment(std::string_view name) -> std::string {
    const auto* value = std::getenv(std::string{name}.c_str());
    return value ? value : std::string{};
}

auto serve_admission(int argc, char** argv) -> int {
    auto options = liquid::admission_options{};
    options.policy = environment("LIQUID_ADMISSION_POLICY");
    options.policies = environment("LIQUID_ADMISSION_POLICIES");
    if (auto value = environment("LIQUID_ADMISSION_LISTEN"); !value.empty()) {
        options.listen = std::move(value);
    }
    if (auto value = environment("LIQUID_CONTROL_LISTEN"); !value.empty()) {
        options.control_listen = std::move(value);
    }
    options.tls_certificate = environment("LIQUID_ADMISSION_TLS_CERT");
    options.tls_key = environment("LIQUID_ADMISSION_TLS_KEY");
    options.attestation_source = environment("LIQUID_ADMISSION_ATTESTATION_SOURCE");
    options.log_cache = environment("LIQUID_ADMISSION_LOG_CACHE");
    if (auto value = environment("LIQUID_ADMISSION_DECISION_REPLAY"); !value.empty()) {
        auto parsed = parse_number(value);
        if (!parsed || *parsed == 0 || *parsed > std::numeric_limits<std::size_t>::max()) {
            return usage("LIQUID_ADMISSION_DECISION_REPLAY must be a positive integer");
        }
        options.decision_replay_capacity = static_cast<std::size_t>(*parsed);
    }
    auto source_flag = false;
    auto store_flag = false;
    for (auto index = 1; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--insecure-http") {
            options.insecure_http = true;
            continue;
        }
        if (++index == argc) {
            return usage("serve admission received an incomplete option");
        }
        if (argument == "--policy") {
            options.policy = argv[index];
            options.policies.clear();
        } else if (argument == "--policies") {
            options.policies = argv[index];
            options.policy.clear();
        } else if (argument == "--listen") {
            options.listen = argv[index];
        } else if (argument == "--control-listen") {
            options.control_listen = argv[index];
        } else if (argument == "--tls-cert") {
            options.tls_certificate = argv[index];
        } else if (argument == "--tls-key") {
            options.tls_key = argv[index];
        } else if (argument == "--attestation-source") {
            options.attestation_source = argv[index];
            source_flag = true;
        } else if (argument == "--store") {
            options.store = argv[index];
            store_flag = true;
        } else if (argument == "--log-cache") {
            options.log_cache = argv[index];
        } else if (argument == "--decision-replay") {
            auto parsed = parse_number(argv[index]);
            if (!parsed || *parsed == 0 || *parsed > std::numeric_limits<std::size_t>::max()) {
                return usage("serve admission --decision-replay requires a positive integer");
            }
            options.decision_replay_capacity = static_cast<std::size_t>(*parsed);
        } else {
            return usage("serve admission received an unknown option");
        }
    }
    if (options.policy.empty() == options.policies.empty()) {
        return usage(
            "serve admission requires exactly one of --policy <file> and --policies <dir>");
    }
    if (source_flag && store_flag) {
        return usage("serve admission accepts only one of --store and --attestation-source");
    }
    if (store_flag) {
        options.attestation_source.clear();
    }
    if (!options.insecure_http && (options.tls_certificate.empty() || options.tls_key.empty())) {
        return usage("serve admission requires --tls-cert and --tls-key or --insecure-http");
    }
    if (options.insecure_http) {
        std::println(stderr, "warning: admission insecure HTTP is enabled");
    }
    std::println("admission listening on {}", options.listen);
    std::println("admission control listening on {}", options.control_listen);
    auto served = liquid::runtime::block_on(liquid::serve_admission(std::move(options)));
    return served ? 0 : fail(served.error());
}

} // namespace

auto serve(int argc, char** argv) -> int {
    if (argc >= 1 && std::string_view{argv[0]} == "reapi") {
        return serve_reapi(argc, argv);
    }
    if (argc >= 1 && std::string_view{argv[0]} == "admission") {
        return serve_admission(argc, argv);
    }
    auto options = parse_options(argc, argv);
    if (!options) {
        return usage();
    }
    // The bearer token comes from an owner-only file, never argv (liquid#403).
    if (options->token_file) {
        auto token = liquid::read_token_file(*options->token_file);
        if (!token) {
            return fail(token.error());
        }
        options->security.bearer_token = std::move(*token);
    }
    // Plaintext and unauthenticated service only on loopback.
    const auto secured = liquid::validate_listener_security(
        liquid::is_loopback_address(options->address), options->security);
    if (!secured) {
        return fail(secured.error());
    }
    auto tls = sync_tls_context(options->security);
    if (!tls) {
        return fail(tls.error());
    }
    auto opened = liquid::runtime::block_on(liquid::repository::open(options->path));
    if (!opened) {
        return fail(opened.error());
    }
    try {
        auto server_options = liquid::sync_server_options{};
        if (!options->allowed_refs.empty()) {
            server_options.authorize = reference_authorizer(std::move(options->allowed_refs));
        }
        if (options->upload_quota) {
            server_options.upload_quota_bytes = *options->upload_quota;
        }
        auto handler = liquid::sync_server{*opened, std::move(server_options)};
        auto context = boost::corosio::io_context{};
        auto server =
            sync_http_server{context, make_router(handler, options->security.bearer_token), *tls};
        auto address = boost::corosio::endpoint{options->address};
        auto bound = server.bind(boost::corosio::endpoint{address, options->port});
        if (bound) {
            return fail(liquid::error{liquid::errc::io, bound.message(), "serve sync listener"});
        }
        std::println(
            "listening={}://{}:{}", *tls ? "https" : "http", options->address, options->port);
        server.start();
        context.run();
        return 0;
    } catch (const std::exception& exception) {
        return fail(liquid::error{liquid::errc::invalid_argument, exception.what(), "serve sync"});
    }
}

} // namespace liquid_cli
