#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <print>
#include <ranges>
#include <string_view>
#include <thread>

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
    for (const auto& conflict : status.conflicts) {
        std::println("conflict={} values={}", conflict.path, conflict.refs.size());
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

auto watch(const std::filesystem::path& folder) -> int {
    auto ready = ensure_daemon(folder);
    if (!ready) {
        return fail(ready.error());
    }
    auto attached = liquid::runtime::block_on(liquid::daemon::attach(folder));
    if (!attached) {
        return fail(attached.error());
    }
    interrupted = 0;
    auto previous = std::signal(SIGINT, handle_interrupt);
    std::println("watching {}", folder.string());
    while (interrupted == 0) {
        auto status = liquid::runtime::block_on(attached->status());
        if (!status) {
            std::signal(SIGINT, previous);
            return fail(status.error());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    std::signal(SIGINT, previous);
    return 0;
}

} // namespace

auto drive(int argc, char** argv) -> int {
    if (argc < 1) {
        return usage("drive <sync|status> ...");
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
