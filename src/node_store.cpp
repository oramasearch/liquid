#include "liquid/node_store.hpp"

#include <filesystem>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <utility>

#include "node_store/service.hpp"
#include "serve/http.hpp"

namespace liquid {

runtime::task<void> serve_node_store(node_store_options options) {
    auto opened = co_await node_store_detail::service::open(std::move(options));
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    auto service = std::move(*opened);
    const auto& listen = service->options().listen;
    constexpr auto unix_prefix = std::string_view{"unix://"};
    if (!std::string_view{listen}.starts_with(unix_prefix) || listen.size() == unix_prefix.size()) {
        co_return std::unexpected(
            error{errc::invalid_argument, "node-store listen address must use unix://", listen});
    }
    runtime::spawn(serve::serve_tcp(service->options().control_listen, service->control_routes()),
                   [service](result<void> outcome) {
                       if (!outcome) {
                           service->note_error(outcome.error());
                           std::println(stderr,
                                        "node-store control listener error: {} ({})",
                                        outcome.error().message,
                                        outcome.error().context);
                       }
                   });
    const auto socket = std::filesystem::path{listen.substr(unix_prefix.size())};
    co_return co_await serve::serve_unix(
        socket, service->unix_routes(), [service] { service->socket_bound(); });
}

} // namespace liquid
