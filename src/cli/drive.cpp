#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <print>
#include <ranges>
#include <string_view>
#include <utility>

#include "liquid/daemon.hpp"
#include "liquid/drive.hpp"
#include "liquid/runtime.hpp"

namespace liquid_cli {

[[nodiscard]] auto ensure_daemon(const std::filesystem::path& checkout) -> liquid::result<void>;

namespace {

volatile std::sig_atomic_t interrupted = 0;

auto fail(const liquid::error& error) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(error.code), error.message, error.context);
    return 1;
}

auto usage(std::string_view message) -> int {
    std::println(stderr, "error: usage: {} ()", message);
    return 2;
}

auto digest_text(const std::optional<liquid::digest>& value) -> std::string {
    return value ? liquid::to_string(*value) : "none";
}

auto kind_name(liquid::drive_reconciliation_kind kind) -> std::string_view {
    switch (kind) {
    case liquid::drive_reconciliation_kind::local_only:
        return "local-only";
    case liquid::drive_reconciliation_kind::remote_only:
        return "remote-only";
    case liquid::drive_reconciliation_kind::content_conflict:
        return "content-conflict";
    case liquid::drive_reconciliation_kind::moved:
        return "move";
    case liquid::drive_reconciliation_kind::delete_edit:
        return "delete-edit";
    case liquid::drive_reconciliation_kind::interrupted_transfer:
        return "interrupted-transfer";
    }
    return "unknown";
}

auto state_name(liquid::drive_path_state state) -> std::string_view {
    switch (state) {
    case liquid::drive_path_state::online_only:
        return "online-only";
    case liquid::drive_path_state::available_offline:
        return "available-offline";
    case liquid::drive_path_state::not_synced_here:
        return "not-synced-here";
    }
    return "online-only";
}

auto print_status(const liquid::drive_status& status) -> void {
    std::println("folder={}", status.folder.string());
    std::println("remote={}", status.remote);
    std::println("remote_tree={}", digest_text(status.trees.remote));
    std::println("local_tree={}", digest_text(status.trees.local));
    std::println("base_tree={}", digest_text(status.trees.base));
    std::println("remote_cursor={}", status.trees.remote_cursor.token);
    std::println("interrupted={}", status.trees.interrupted);
    std::println("files={}", status.files);
    std::println("state_bytes={}", status.state_bytes);
    std::println("state_memory_ceiling={}", status.state_memory_ceiling);
    std::println("full_walks={}", status.full_walks);
    std::println("hydration_size_threshold={}", status.settings.hydration_size_threshold);
    std::println("pin_folders={}", status.settings.pin_folders);
    std::println("pending_uploads={}", status.pending_uploads);
    std::println("hydration_debt={}", status.hydration_debt);
    std::println("offline_reason={}", status.offline_reason);
    for (const auto& path : status.paths) {
        std::println("path={} state={} size={} hydration_debt={} hydrated={} reason={}",
                     path.path,
                     state_name(path.state),
                     path.size,
                     path.hydration_debt,
                     path.hydrated,
                     path.reason);
    }
    for (const auto& conflict : status.conflicts) {
        std::println("conflict={} id={}", conflict.region.path, liquid::to_string(conflict.id));
    }
    for (const auto& reference : status.references) {
        const auto outcome =
            reference.outcome == liquid::sync_ref_outcome_kind::applied ? "applied" : "refused";
        std::println(
            "reference={} outcome={} reason={}", reference.name, outcome, reference.reason);
    }
}

auto handle_interrupt(int) -> void {
    interrupted = 1;
}

auto watch_task(std::filesystem::path folder) -> liquid::runtime::task<void> {
    auto attached = co_await liquid::daemon::attach(folder);
    if (!attached) {
        co_return std::unexpected(attached.error());
    }
    std::println("watching {}", folder.string());
    while (interrupted == 0) {
        auto status = co_await attached->status();
        if (!status) {
            co_return std::unexpected(status.error());
        }
        auto [delay_error] = co_await liquid::runtime::delay(std::chrono::milliseconds{50});
        if (delay_error) {
            co_return std::unexpected(
                liquid::error{liquid::errc::io, delay_error.message(), "drive watch"});
        }
    }
    co_return liquid::result<void>{};
}

auto watch(const std::filesystem::path& folder) -> int {
    auto ready = ensure_daemon(folder);
    if (!ready) {
        return fail(ready.error());
    }
    interrupted = 0;
    auto previous = std::signal(SIGINT, handle_interrupt);
    auto watched = liquid::runtime::block_on(watch_task(folder));
    std::signal(SIGINT, previous);
    return watched ? 0 : fail(watched.error());
}

auto drive_root_and_path(std::filesystem::path requested)
    -> liquid::runtime::task<std::pair<std::filesystem::path, std::string>> {
    auto located = co_await liquid::runtime::offload([requested = std::move(requested)] {
        auto code = std::error_code{};
        auto target = std::filesystem::absolute(requested, code).lexically_normal();
        if (code) {
            return liquid::result<std::pair<std::filesystem::path, std::string>>{std::unexpected(
                liquid::error{liquid::errc::io, code.message(), requested.string()})};
        }
        auto candidate =
            std::filesystem::is_directory(target, code) ? target : target.parent_path();
        code.clear();
        while (!candidate.empty()) {
            if (std::filesystem::exists(candidate / "LIQUID_REPO", code) && !code) {
                auto relative = target.lexically_relative(candidate).generic_string();
                if (relative == ".") {
                    relative.clear();
                }
                return liquid::result<std::pair<std::filesystem::path, std::string>>{
                    std::pair{candidate, std::move(relative)}};
            }
            if (code) {
                return liquid::result<std::pair<std::filesystem::path, std::string>>{
                    std::unexpected(
                        liquid::error{liquid::errc::io, code.message(), candidate.string()})};
            }
            const auto parent = candidate.parent_path();
            if (parent == candidate) {
                break;
            }
            candidate = parent;
        }
        return liquid::result<std::pair<std::filesystem::path, std::string>>{std::unexpected(
            liquid::error{liquid::errc::not_found, "drive repository not found", target.string()})};
    });
    if (!located) {
        co_return std::unexpected(located.error());
    }
    co_return std::move(*located);
}

auto change_pin(std::filesystem::path requested, bool pin)
    -> liquid::runtime::task<liquid::drive_path_status> {
    auto located = co_await drive_root_and_path(std::move(requested));
    if (!located) {
        co_return std::unexpected(located.error());
    }
    auto client = co_await liquid::drive_client::open(located->first);
    if (!client) {
        co_return std::unexpected(client.error());
    }
    auto changed =
        pin ? co_await client->pin(located->second) : co_await client->unpin(located->second);
    if (!changed) {
        co_return std::unexpected(changed.error());
    }
    co_return co_await client->path_status(located->second);
}

} // namespace

auto drive(int argc, char** argv) -> int {
    if (argc < 1) {
        return usage("drive <sync|status|pin|unpin> ...");
    }
    const auto command = std::string_view{argv[0]};
    if (command == "status") {
        if (argc > 2) {
            return usage("drive status [folder]");
        }
        const auto folder =
            argc == 2 ? std::filesystem::path{argv[1]} : std::filesystem::current_path();
        auto client = liquid::runtime::block_on(liquid::drive_client::open(folder));
        if (!client) {
            return fail(client.error());
        }
        auto status = liquid::runtime::block_on(client->status());
        if (!status) {
            return fail(status.error());
        }
        print_status(*status);
        return 0;
    }
    if (command == "pin" || command == "unpin") {
        if (argc != 2) {
            return usage("drive pin|unpin <path>");
        }
        auto changed =
            liquid::runtime::block_on(change_pin(std::filesystem::path{argv[1]}, command == "pin"));
        if (!changed) {
            return fail(changed.error());
        }
        std::println("path={} state={} reason={}",
                     changed->path,
                     state_name(changed->state),
                     changed->reason);
        return 0;
    }
    if (command != "sync" || argc < 3 || argc > 4) {
        return usage("drive sync <folder> <remote> [--watch]");
    }
    const auto watching = argc == 4 && std::string_view{argv[3]} == "--watch";
    if (argc == 4 && !watching) {
        return usage("drive sync <folder> <remote> [--watch]");
    }
    auto options = liquid::drive_options{};
    if (const auto* token = std::getenv("LIQUID_TOKEN")) {
        options.bearer_token = token;
    }
    const auto folder = std::filesystem::path{argv[1]};
    auto client = liquid::runtime::block_on(
        liquid::drive_client::open(folder, std::string{argv[2]}, std::move(options)));
    if (!client) {
        return fail(client.error());
    }
    auto report = liquid::runtime::block_on(client->sync());
    if (!report) {
        return fail(report.error());
    }
    print_status(report->status);
    for (const auto& path : report->paths) {
        std::println("path={} outcome={} old_path={}",
                     path.path,
                     kind_name(path.kind),
                     path.old_path.value_or(""));
    }
    const auto refused = std::ranges::any_of(report->status.references, [](const auto& reference) {
        return reference.outcome == liquid::sync_ref_outcome_kind::refused;
    });
    if (refused) {
        return 1;
    }
    return watching ? watch(report->status.folder) : 0;
}

} // namespace liquid_cli
