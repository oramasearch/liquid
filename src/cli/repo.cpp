#include <filesystem>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "liquid/daemon.hpp"
#include "liquid/drip.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace liquid_cli {

[[nodiscard]] auto ensure_daemon(const std::filesystem::path& checkout) -> liquid::result<void>;

namespace {

auto fail(const liquid::error& error) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(error.code), error.message, error.context);
    return 1;
}

auto usage(std::string_view message) -> int {
    std::println(stderr, "error: usage: {} ()", message);
    return 2;
}

auto json_string(std::string_view value) -> std::string {
    auto output = std::string{"\""};
    for (const auto character : value) {
        switch (character) {
        case '\\':
            output += "\\\\";
            break;
        case '\"':
            output += "\\\"";
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
                output += character;
            }
        }
    }
    output += '"';
    return output;
}

auto daemon_targets_json(const std::optional<liquid::daemon_status>& status) -> std::string {
    auto output = std::string{"["};
    if (status) {
        for (std::size_t index = 0; index != status->targets.size(); ++index) {
            const auto& target = status->targets[index];
            output += std::format("{}{{\"name\":{},\"cursor\":{},\"behind\":{},\"deliveries\":{},"
                                  "\"last_error\":{}}}",
                                  index == 0 ? "" : ",",
                                  json_string(target.name),
                                  json_string(target.cursor.token),
                                  target.behind,
                                  target.deliveries,
                                  target.last_error ? json_string(*target.last_error) : "null");
        }
    }
    output += ']';
    return output;
}

auto daemon_paths_json(const std::optional<liquid::daemon_status>& status) -> std::string {
    auto output = std::string{"["};
    if (status) {
        for (std::size_t index = 0; index != status->invalidated_paths.size(); ++index) {
            output += index == 0 ? "" : ",";
            output += json_string(status->invalidated_paths[index]);
        }
    }
    output += ']';
    return output;
}

auto join(int argc, char** argv) -> std::string {
    auto output = std::string{};
    for (auto index = 0; index < argc; ++index) {
        if (index != 0) {
            output += ' ';
        }
        output += argv[index];
    }
    return output;
}

auto open_repository() -> liquid::result<liquid::repository> {
    return liquid::runtime::block_on(liquid::repository::open(std::filesystem::current_path()));
}

} // namespace

auto repo_init(int argc, char** argv) -> int {
    if (argc > 1) {
        return usage("init accepts at most one path");
    }
    const auto path = argc == 1 ? std::filesystem::path{argv[0]} : std::filesystem::current_path();
    auto repository = liquid::runtime::block_on(liquid::repository::create(path));
    if (!repository) {
        return fail(repository.error());
    }
    auto options = liquid::import_options{};
    options.message = "initial import";
    options.ignore_patterns = {".liquid-store/**", "LIQUID_REPO", "LIQUID_JOURNAL"};
    auto imported = liquid::runtime::block_on(repository->import_tree(path, std::move(options)));
    if (!imported) {
        return fail(imported.error());
    }
    std::println("initialised {}", path.string());
    return 0;
}

auto repo_status(int argc, char** argv) -> int {
    auto json = false;
    if (argc == 1 && std::string_view{argv[0]} == "--json") {
        json = true;
    } else if (argc != 0) {
        return usage("status accepts only --json");
    }
    auto repository = open_repository();
    if (!repository) {
        return fail(repository.error());
    }
    auto current = repository->status();
    if (!current) {
        return fail(current.error());
    }
    auto daemon_current = std::optional<liquid::daemon_status>{};
    auto ready = ensure_daemon(std::filesystem::current_path());
    if (!ready && ready.error().code != liquid::errc::unsupported) {
        return fail(ready.error());
    }
    if (ready) {
        auto attached =
            liquid::runtime::block_on(liquid::daemon::attach(std::filesystem::current_path()));
        if (!attached) {
            return fail(attached.error());
        }
        auto status = liquid::runtime::block_on(attached->status());
        if (!status) {
            return fail(status.error());
        }
        daemon_current = std::move(*status);
    }
    const auto head = current->head ? liquid::to_string(current->head->value) : std::string{};
    if (json) {
        std::println(
            "{{\"pending_drips\":{},\"remote_bytes\":{},\"paused\":{},\"conflicts\":{},\"oversized_"
            "directories\":{},\"head\":{},\"capture\":{{\"state\":{},\"pending_drips\":{},"
            "\"overflow_count\":{},\"recrawl_count\":{},\"cursor\":{}}},"
            "\"daemon\":{{\"available\":{},\"running\":{},"
            "\"pid\":{},\"graph_keys\":{},\"graph_values\":{},\"graph_memory\":{},"
            "\"graph_ceiling\":{},\"last_drip\":{},\"invalidated_paths\":{},"
            "\"targets\":{}}}}}",
            current->pending_drips,
            current->remote_bytes,
            current->paused ? "true" : "false",
            current->conflicts.size(),
            current->oversized_directories.size(),
            current->head ? json_string(head) : "null",
            json_string(daemon_current && daemon_current->capture.state ==
                                              liquid::capture_status::state::paused
                            ? "paused"
                            : "running"),
            daemon_current ? daemon_current->capture.pending_drips : 0,
            daemon_current ? daemon_current->capture.overflow_count : 0,
            daemon_current ? daemon_current->capture.recrawl_count : 0,
            json_string(daemon_current ? daemon_current->capture.last_cursor.token : ""),
            daemon_current ? "true" : "false",
            daemon_current && daemon_current->running ? "true" : "false",
            daemon_current ? daemon_current->process_id : 0,
            daemon_current ? daemon_current->graph_keys : 0,
            daemon_current ? daemon_current->graph_values : 0,
            daemon_current ? daemon_current->graph_memory_bytes : 0,
            daemon_current ? daemon_current->graph_memory_ceiling : 0,
            daemon_current && daemon_current->last_drip
                ? json_string(liquid::to_string(daemon_current->last_drip->value))
                : "null",
            daemon_paths_json(daemon_current),
            daemon_targets_json(daemon_current));
    } else {
        std::println("pending_drips={}", current->pending_drips);
        std::println("remote_bytes={}", current->remote_bytes);
        std::println("paused={}", current->paused);
        std::println("conflicts={}", current->conflicts.size());
        std::println("oversized_directories={}", current->oversized_directories.size());
        std::println("head={}", head);
        std::println("daemon_available={}", daemon_current.has_value());
        std::println("capture_state={}",
                     daemon_current &&
                             daemon_current->capture.state == liquid::capture_status::state::paused
                         ? "paused"
                         : "running");
        std::println("capture_pending_drips={}",
                     daemon_current ? daemon_current->capture.pending_drips : 0);
        std::println("capture_overflow_count={}",
                     daemon_current ? daemon_current->capture.overflow_count : 0);
        std::println("capture_recrawl_count={}",
                     daemon_current ? daemon_current->capture.recrawl_count : 0);
        std::println("capture_cursor={}",
                     daemon_current ? daemon_current->capture.last_cursor.token : "");
        std::println("daemon_running={}", daemon_current && daemon_current->running);
        if (daemon_current) {
            std::println("daemon_pid={}", daemon_current->process_id);
            std::println("daemon_graph_keys={}", daemon_current->graph_keys);
            std::println("daemon_graph_values={}", daemon_current->graph_values);
            std::println("daemon_graph_memory={}", daemon_current->graph_memory_bytes);
            std::println("daemon_graph_ceiling={}", daemon_current->graph_memory_ceiling);
            std::println("daemon_last_drip={}",
                         daemon_current->last_drip
                             ? liquid::to_string(daemon_current->last_drip->value)
                             : "");
            for (const auto& target : daemon_current->targets) {
                std::println("daemon_target={} cursor={} behind={} deliveries={}",
                             target.name,
                             target.cursor.token,
                             target.behind,
                             target.deliveries);
            }
        }
    }
    return 0;
}

auto repo_log(int argc, char** argv) -> int {
    auto json = false;
    if (argc == 1 && std::string_view{argv[0]} == "--json") {
        json = true;
    } else if (argc != 0) {
        return usage("log accepts only --json");
    }
    auto repository = open_repository();
    if (!repository) {
        return fail(repository.error());
    }
    auto changes = liquid::runtime::block_on(repository->log());
    if (!changes) {
        return fail(changes.error());
    }
    if (json) {
        std::print("[");
        for (std::size_t index = 0; index < changes->size(); ++index) {
            const auto& change = (*changes)[index];
            std::print("{}{{\"id\":{},\"message\":{},\"author\":{},\"at\":{}}}",
                       index == 0 ? "" : ",",
                       json_string(liquid::to_string(change.id.value)),
                       json_string(change.message),
                       json_string(change.author.taap_id),
                       change.at.time_since_epoch().count());
        }
        std::println("]");
    } else {
        for (const auto& change : *changes) {
            std::println("{} {} {}",
                         liquid::to_string(change.id.value),
                         change.author.taap_id,
                         change.message);
        }
    }
    return 0;
}

auto repo_cut(int argc, char** argv) -> int {
    if (argc == 0) {
        return usage("cut requires a message");
    }
    auto repository = open_repository();
    if (!repository) {
        return fail(repository.error());
    }
    auto change = liquid::runtime::block_on(repository->cut(join(argc, argv)));
    if (!change) {
        return fail(change.error());
    }
    std::println("change={}", liquid::to_string(change->value));
    return 0;
}

auto repo_refs(int argc, char** argv) -> int {
    auto json = false;
    if (argc == 1 && std::string_view{argv[0]} == "--json") {
        json = true;
    } else if (argc != 0) {
        return usage("refs accepts only --json");
    }
    auto repository = open_repository();
    if (!repository) {
        return fail(repository.error());
    }
    auto values = liquid::runtime::block_on(repository->refs());
    if (!values) {
        return fail(values.error());
    }
    if (json) {
        std::print("[");
        for (std::size_t index = 0; index < values->size(); ++index) {
            const auto& value = (*values)[index];
            std::print("{}{{\"name\":{},\"id\":{}}}",
                       index == 0 ? "" : ",",
                       json_string(value.name),
                       json_string(liquid::to_string(value.id.value)));
        }
        std::println("]");
    } else {
        for (const auto& value : *values) {
            std::println("{}={}", value.name, liquid::to_string(value.id.value));
        }
    }
    return 0;
}

} // namespace liquid_cli
