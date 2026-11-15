// SPDX-License-Identifier: MIT

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "liquid/configuration.hpp"
#include "liquid/daemon.hpp"
#include "liquid/drip.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

#include "cli/signer.hpp"

namespace liquid_cli {

[[nodiscard]] auto ensure_daemon(const std::filesystem::path& checkout) -> liquid::result<void>;
auto history_operations(int argc, char** argv) -> int;
auto log_timestamps(int argc, char** argv) -> int;
auto verify_log_timestamps(int argc, char** argv) -> int;

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

auto hex(std::span<const std::byte> value) -> std::string {
    auto output = std::string{};
    output.reserve(value.size() * 2);
    for (const auto byte : value) {
        output += std::format("{:02x}", std::to_integer<std::uint8_t>(byte));
    }
    return output;
}

auto daemon_targets_json(const std::optional<liquid::daemon_status>& status) -> std::string {
    auto output = std::string{"["};
    if (status) {
        for (std::size_t index = 0; index != status->targets.size(); ++index) {
            const auto& target = status->targets[index];
            output += std::format("{}{{\"name\":{},\"cursor\":{},\"behind\":{},\"deliveries\":{},"
                                  "\"last_error\":{},\"detail\":{}}}",
                                  index == 0 ? "" : ",",
                                  json_string(target.name),
                                  json_string(target.cursor.token),
                                  target.behind,
                                  target.deliveries,
                                  target.last_error ? json_string(*target.last_error) : "null",
                                  target.detail ? json_string(*target.detail) : "null");
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

auto flow_scope_name(liquid::flow_scope scope) -> std::string_view {
    switch (scope) {
    case liquid::flow_scope::actor:
        return "actor";
    case liquid::flow_scope::document:
        return "document";
    case liquid::flow_scope::repository:
        return "repository";
    }
    return "repository";
}

auto flow_reason_name(liquid::flow_pause_reason reason) -> std::string_view {
    switch (reason) {
    case liquid::flow_pause_reason::none:
        return "none";
    case liquid::flow_pause_reason::persistence:
        return "persistence";
    case liquid::flow_pause_reason::consumers:
        return "consumers";
    }
    return "none";
}

auto flow_json(const std::optional<liquid::daemon_status>& status) -> std::string {
    auto buckets = std::string{"["};
    if (status) {
        for (std::size_t index = 0; index != status->flow.buckets.size(); ++index) {
            const auto& bucket = status->flow.buckets[index];
            buckets +=
                std::format("{}{{\"scope\":{},\"key\":{},\"tokens\":{},\"rate\":{},\"burst\":{},"
                            "\"rejected\":{}}}",
                            index == 0 ? "" : ",",
                            json_string(flow_scope_name(bucket.scope)),
                            json_string(bucket.key),
                            bucket.tokens,
                            bucket.rate,
                            bucket.burst,
                            bucket.rejected);
        }
    }
    buckets += ']';
    return std::format("{{\"paused\":{},\"reason\":{},\"persistence_lag\":{},\"consumer_lag\":{},"
                       "\"buckets\":{}}}",
                       status && status->flow.paused ? "true" : "false",
                       json_string(status ? flow_reason_name(status->flow.reason) : "none"),
                       status ? status->flow.persistence_lag : 0,
                       status ? status->flow.consumer_lag : 0,
                       buckets);
}

auto graph_divergence_json(const std::optional<liquid::daemon_status>& status) -> std::string {
    if (!status || !status->last_graph_divergence) {
        return "null";
    }
    const auto& divergence = *status->last_graph_divergence;
    auto inputs = std::string{"["};
    for (std::size_t index = 0; index != divergence.inputs.size(); ++index) {
        const auto& dependency = divergence.inputs[index];
        const auto name = std::visit(
            [](const auto& key) {
                using key_type = std::decay_t<decltype(key)>;
                if constexpr (std::is_same_v<key_type, liquid::graph_input_key>) {
                    return key.name;
                } else {
                    return key.kind;
                }
            },
            dependency.key);
        inputs += std::format("{}{{\"name\":{},\"fingerprint\":{}}}",
                              index == 0 ? "" : ",",
                              json_string(name),
                              json_string(liquid::to_string(dependency.fingerprint)));
    }
    inputs += ']';
    return std::format(
        "{{\"record\":{},\"query\":{},\"arguments\":{},\"inputs\":{},\"incremental\":{},"
        "\"clean\":{}}}",
        json_string(liquid::to_string(divergence.id)),
        json_string(divergence.query.kind),
        json_string(hex(divergence.query.arguments)),
        inputs,
        json_string(liquid::to_string(divergence.incremental)),
        json_string(liquid::to_string(divergence.clean)));
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

auto strings_json(const std::vector<std::string>& values) -> std::string {
    auto output = std::string{"["};
    for (std::size_t index = 0; index < values.size(); ++index) {
        output += index == 0 ? "" : ",";
        output += json_string(values[index]);
    }
    output += ']';
    return output;
}

auto conflicts_json(const std::vector<liquid::conflict_reference>& values) -> std::string {
    auto output = std::string{"["};
    for (auto index = std::size_t{}; index < values.size(); ++index) {
        if (index != 0) {
            output += ',';
        }
        output += std::format("{{\"path\":{},\"id\":{}}}",
                              json_string(values[index].path),
                              json_string(liquid::to_string(values[index].id)));
    }
    output += ']';
    return output;
}

auto data_branches_json(const std::vector<liquid::data_branch_record>& branches) -> std::string {
    auto output = std::string{"["};
    for (std::size_t index = 0; index < branches.size(); ++index) {
        const auto& branch = branches[index];
        output +=
            std::format("{}{{\"id\":{},\"session\":{},\"database\":{},\"url\":{},\"mechanism\":{},"
                        "\"created_at\":{}}}",
                        index == 0 ? "" : ",",
                        json_string(branch.id),
                        json_string(liquid::to_string(branch.session)),
                        json_string(branch.database),
                        json_string(branch.url),
                        json_string(liquid::to_string(branch.mechanism)),
                        branch.created_at.time_since_epoch().count());
    }
    output += ']';
    return output;
}

auto configured_locks(const std::filesystem::path& root) -> liquid::result<liquid::lock_config> {
    auto code = std::error_code{};
    if (!std::filesystem::exists(root, code)) {
        if (code) {
            return std::unexpected(
                liquid::error{liquid::errc::io, code.message(), root.generic_string()});
        }
        return liquid::lock_config{};
    }
    auto selected = root / "liquid.liquid";
    if (!std::filesystem::is_regular_file(selected)) {
        auto candidates = std::vector<std::filesystem::path>{};
        code.clear();
        for (std::filesystem::directory_iterator iterator{root, code};
             !code && iterator != std::filesystem::directory_iterator{};
             iterator.increment(code)) {
            if (iterator->is_regular_file() && iterator->path().extension() == ".liquid") {
                candidates.push_back(iterator->path());
            }
        }
        if (code) {
            return std::unexpected(
                liquid::error{liquid::errc::io, code.message(), root.generic_string()});
        }
        if (candidates.empty()) {
            return liquid::lock_config{};
        }
        std::ranges::sort(candidates);
        selected = candidates.front();
    }
    auto input = std::ifstream{selected, std::ios::binary};
    if (!input) {
        return std::unexpected(liquid::error{
            liquid::errc::io, "cannot read configuration", selected.generic_string()});
    }
    auto text =
        std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    auto document = liquid::parse_configuration(text, selected);
    if (!document) {
        return std::unexpected(document.error());
    }
    auto lowered = liquid::lower(*document);
    if (!lowered) {
        return std::unexpected(lowered.error());
    }
    return std::move(lowered->locks);
}

} // namespace

auto repo_init(int argc, char** argv) -> int {
    if (argc > 1) {
        return usage("init accepts at most one path");
    }
    const auto path = argc == 1 ? std::filesystem::path{argv[0]} : std::filesystem::current_path();
    auto locks = configured_locks(path);
    if (!locks) {
        return fail(locks.error());
    }
    auto repository =
        liquid::runtime::block_on(liquid::repository::create(path, {.locks = std::move(*locks)}));
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

auto repo_write(int argc, char** argv) -> int {
    if (argc < 5) {
        return usage("write requires <path>, --from <file>, and --base <change>[@<cursor>]");
    }
    auto path = std::string{argv[0]};
    auto source = std::optional<std::filesystem::path>{};
    auto options = liquid::write_options{};
    for (auto index = 1; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--from" && index + 1 < argc) {
            source = std::filesystem::path{argv[++index]};
        } else if (argument == "--base" && index + 1 < argc) {
            const auto value = std::string_view{argv[++index]};
            const auto separator = value.find('@');
            const auto change_text = value.substr(0, separator);
            auto change = liquid::parse_digest(change_text);
            if (!change) {
                return fail(change.error());
            }
            auto base = liquid::write_base{.change = liquid::change_id{*change}};
            if (separator != std::string_view::npos) {
                const auto cursor = value.substr(separator + 1);
                if (cursor.empty()) {
                    return usage("write base cursor must not be empty");
                }
                base.cursor = liquid::journal_cursor{std::string{cursor}};
            }
            options.base = std::move(base);
        } else if (argument == "--depends-on" && index + 1 < argc) {
            options.depends_on.emplace_back(argv[++index]);
        } else if (argument == "--on-stale" && index + 1 < argc) {
            const auto choice = std::string_view{argv[++index]};
            if (choice == "refuse") {
                options.on_stale = liquid::write_options::stale_action::refuse;
            } else if (choice == "disjoint") {
                options.on_stale = liquid::write_options::stale_action::accept_if_disjoint;
            } else if (choice == "force") {
                options.on_stale = liquid::write_options::stale_action::force;
            } else {
                return usage("write --on-stale accepts refuse, disjoint, or force");
            }
        } else {
            return usage("invalid write option");
        }
    }
    if (!source || !options.base) {
        return usage("write requires --from and --base");
    }
    auto repository = open_repository();
    if (!repository) {
        return fail(repository.error());
    }
    auto written = liquid::runtime::block_on(
        repository->write_file(std::move(path), std::move(*source), std::move(options)));
    if (!written) {
        return fail(written.error());
    }
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
            "directories\":{},\"head\":{},\"agents\":{{\"require_base\":{},\"actors\":{}}},"
            "\"data_branches\":{},\"store_recoveries\":{},"
            "\"capture\":{{\"state\":{},\"pending_drips\":{},"
            "\"overflow_count\":{},\"recrawl_count\":{},\"cursor\":{}}},"
            "\"flow\":{},"
            "\"daemon\":{{\"available\":{},\"running\":{},"
            "\"pid\":{},\"graph_keys\":{},\"graph_values\":{},\"graph_memory\":{},"
            "\"graph_ceiling\":{},\"graph_validation_backlog\":{},"
            "\"last_graph_divergence\":{},\"last_drip\":{},\"invalidated_paths\":{},"
            "\"targets\":{}}}}}",
            current->pending_drips,
            current->remote_bytes,
            current->paused ? "true" : "false",
            conflicts_json(current->conflicts),
            current->oversized_directories.size(),
            current->head ? json_string(head) : "null",
            current->agents.require_base ? "true" : "false",
            strings_json(current->agents.actors),
            data_branches_json(current->data_branches),
            strings_json(current->store_recoveries),
            json_string(daemon_current && daemon_current->capture.state ==
                                              liquid::capture_status::state::paused
                            ? "paused"
                            : "running"),
            daemon_current ? daemon_current->capture.pending_drips : 0,
            daemon_current ? daemon_current->capture.overflow_count : 0,
            daemon_current ? daemon_current->capture.recrawl_count : 0,
            json_string(daemon_current ? daemon_current->capture.last_cursor.token : ""),
            flow_json(daemon_current),
            daemon_current ? "true" : "false",
            daemon_current && daemon_current->running ? "true" : "false",
            daemon_current ? daemon_current->process_id : 0,
            daemon_current ? daemon_current->graph_keys : 0,
            daemon_current ? daemon_current->graph_values : 0,
            daemon_current ? daemon_current->graph_memory_bytes : 0,
            daemon_current ? daemon_current->graph_memory_ceiling : 0,
            daemon_current ? daemon_current->graph_validation_backlog : 0,
            graph_divergence_json(daemon_current),
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
        for (const auto& conflict : current->conflicts) {
            std::println("conflict={} {}", conflict.path, liquid::to_string(conflict.id));
        }
        std::println("oversized_directories={}", current->oversized_directories.size());
        std::println("head={}", head);
        std::println("agents_require_base={}", current->agents.require_base);
        for (const auto& actor : current->agents.actors) {
            std::println("agent_actor={}", actor);
        }
        for (const auto& branch : current->data_branches) {
            std::println("data_branch={} session={} database={} mechanism={} url={}",
                         branch.id,
                         liquid::to_string(branch.session),
                         branch.database,
                         liquid::to_string(branch.mechanism),
                         branch.url);
        }
        for (const auto& recovery : current->store_recoveries) {
            std::println("store_recovery={}", recovery);
        }
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
        std::println("flow paused={} reason={} persistence_lag={} consumer_lag={}",
                     daemon_current && daemon_current->flow.paused,
                     flow_reason_name(daemon_current ? daemon_current->flow.reason
                                                     : liquid::flow_pause_reason::none),
                     daemon_current ? daemon_current->flow.persistence_lag : 0,
                     daemon_current ? daemon_current->flow.consumer_lag : 0);
        std::println("daemon_running={}", daemon_current && daemon_current->running);
        if (daemon_current) {
            std::println("daemon_pid={}", daemon_current->process_id);
            std::println("daemon_graph_keys={}", daemon_current->graph_keys);
            std::println("daemon_graph_values={}", daemon_current->graph_values);
            std::println("daemon_graph_memory={}", daemon_current->graph_memory_bytes);
            std::println("daemon_graph_ceiling={}", daemon_current->graph_memory_ceiling);
            std::println("daemon_graph_validation_backlog={}",
                         daemon_current->graph_validation_backlog);
            if (daemon_current->last_graph_divergence) {
                const auto& divergence = *daemon_current->last_graph_divergence;
                std::println("daemon_last_graph_divergence={} query={} arguments={} incremental={} "
                             "clean={}",
                             liquid::to_string(divergence.id),
                             divergence.query.kind,
                             hex(divergence.query.arguments),
                             liquid::to_string(divergence.incremental),
                             liquid::to_string(divergence.clean));
                for (const auto& input : divergence.inputs) {
                    if (const auto* key = std::get_if<liquid::graph_input_key>(&input.key)) {
                        std::println("daemon_last_graph_divergence_input={} fingerprint={}",
                                     key->name,
                                     liquid::to_string(input.fingerprint));
                    }
                }
            }
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
                if (target.detail) {
                    std::println("daemon_target_detail={} {}", target.name, *target.detail);
                }
            }
        }
    }
    return 0;
}

auto repo_log(int argc, char** argv) -> int {
    if (argc >= 1 && std::string_view{argv[0]} == "timestamps") {
        return log_timestamps(argc - 1, argv + 1);
    }
    if (argc >= 1 && std::string_view{argv[0]} == "verify") {
        return verify_log_timestamps(argc - 1, argv + 1);
    }
    auto json = false;
    auto ops = false;
    for (auto index = 0; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--json") {
            json = true;
        } else if (argument == "--ops") {
            ops = true;
        } else {
            return usage("log accepts --ops, --json, timestamps <log-url>, or verify --timestamps");
        }
    }
    auto repository = open_repository();
    if (!repository) {
        return fail(repository.error());
    }
    if (ops) {
        auto drips = liquid::runtime::block_on(repository->drips());
        if (!drips) {
            return fail(drips.error());
        }
        if (json) {
            std::print("[");
            for (std::size_t index = 0; index < drips->size(); ++index) {
                const auto& drip = (*drips)[index];
                auto paths = std::vector<std::string>{};
                for (const auto& operation : drip.ops) {
                    paths.push_back(operation.path);
                }
                const auto reasoned_from =
                    drip.reasoned_from
                        ? json_string(liquid::to_string(drip.reasoned_from->change.value))
                        : "null";
                const auto cursor = drip.reasoned_from && drip.reasoned_from->cursor
                                        ? json_string(drip.reasoned_from->cursor->token)
                                        : "null";
                const auto revalidated =
                    !drip.revalidated
                        ? "null"
                        : json_string(*drip.revalidated == liquid::stale_revalidation::disjoint
                                          ? "disjoint"
                                          : "forced");
                const auto delegation =
                    drip.delegation ? json_string(liquid::to_string(*drip.delegation)) : "null";
                std::print("{}{{\"id\":{},\"base\":{},\"paths\":{},\"actor\":{},"
                           "\"identity_verified\":{},\"at\":{},\"reasoned_from\":{},\"cursor\":{},"
                           "\"revalidated\":{},"
                           "\"delegation\":{}}}",
                           index == 0 ? "" : ",",
                           json_string(liquid::to_string(drip.id.value)),
                           json_string(liquid::to_string(drip.base.value)),
                           strings_json(paths),
                           json_string(shown_signer(drip.by, drip.identity_verified)),
                           drip.identity_verified,
                           drip.at.time_since_epoch().count(),
                           reasoned_from,
                           cursor,
                           revalidated,
                           delegation);
            }
            std::println("]");
        } else {
            for (const auto& drip : *drips) {
                std::println("{} {} {}",
                             liquid::to_string(drip.id.value),
                             shown_signer(drip.by, drip.identity_verified),
                             drip.revalidated == liquid::stale_revalidation::disjoint ? "disjoint"
                             : drip.revalidated == liquid::stale_revalidation::forced ? "forced"
                                                                                      : "");
            }
        }
        return 0;
    }
    auto changes = liquid::runtime::block_on(repository->log());
    if (!changes) {
        return fail(changes.error());
    }
    if (json) {
        std::print("[");
        for (std::size_t index = 0; index < changes->size(); ++index) {
            const auto& change = (*changes)[index];
            std::print("{}{{\"id\":{},\"message\":{},\"author\":{},\"identity_verified\":{},"
                       "\"at\":{}}}",
                       index == 0 ? "" : ",",
                       json_string(liquid::to_string(change.id.value)),
                       json_string(change.message),
                       json_string(shown_signer(change.author, change.identity_verified)),
                       change.identity_verified,
                       change.at.time_since_epoch().count());
        }
        std::println("]");
    } else {
        for (const auto& change : *changes) {
            std::println("{} {} {}",
                         liquid::to_string(change.id.value),
                         shown_signer(change.author, change.identity_verified),
                         change.message);
        }
    }
    return 0;
}

auto repo_cut(int argc, char** argv) -> int {
    // `-m`/`--message` as in Git, or the remaining words as the message. A leading `-` is refused
    // so that a mistyped option is never recorded in history (liquid#528).
    auto message = std::string{};
    if (argc >= 1 &&
        (std::string_view{argv[0]} == "-m" || std::string_view{argv[0]} == "--message")) {
        if (argc != 2) {
            return usage("cut -m <message> takes one message; quote it");
        }
        message = argv[1];
    } else {
        message = join(argc, argv);
    }
    if (message.empty()) {
        return usage("cut requires a message: liquid cut -m <message>");
    }
    if (message.starts_with('-')) {
        return usage(std::format("cut refuses a message that starts with '-' ({}); use "
                                 "liquid cut -m <message>",
                                 message));
    }
    auto repository = open_repository();
    if (!repository) {
        return fail(repository.error());
    }
    auto change = liquid::runtime::block_on(repository->cut(message));
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
