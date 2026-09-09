#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <print>
#include <span>
#include <string>
#include <string_view>

#include "liquid/build.hpp"
#include "liquid/error.hpp"
#include "liquid/hash.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "liquid/transparency.hpp"

#include "cli/verify.hpp"

namespace liquid_cli {
namespace {

auto fail(const liquid::error& value) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(value.code), value.message, value.context);
    return 1;
}

auto usage(std::string_view message) -> int {
    std::println(stderr, "error: usage: {} ()", message);
    return 2;
}

auto digest_text(const std::optional<liquid::digest>& value) -> std::string {
    return value ? liquid::to_string(*value) : "missing";
}

auto hex(std::span<const std::byte> value) -> std::string {
    auto output = std::string{};
    output.reserve(value.size() * 2);
    for (const auto byte : value) {
        output += std::format("{:02x}", std::to_integer<std::uint8_t>(byte));
    }
    return output;
}

auto json_string(std::string_view value) -> std::string {
    auto output = std::string{"\""};
    for (const auto character : value) {
        switch (character) {
        case '\\':
            output += "\\\\";
            break;
        case '"':
            output += "\\\"";
            break;
        case '\b':
            output += "\\b";
            break;
        case '\f':
            output += "\\f";
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

auto json_digest(const std::optional<liquid::digest>& value) -> std::string {
    return value ? json_string(liquid::to_string(*value)) : "null";
}

auto json_number(const auto& value) -> std::string {
    return value ? std::to_string(*value) : "null";
}

auto verify_log_inclusion(const std::string& url, const liquid::digest& identifier)
    -> liquid::result<void> {
    auto repository =
        liquid::runtime::block_on(liquid::repository::open(std::filesystem::current_path()));
    if (!repository) {
        return std::unexpected(repository.error());
    }
    liquid::attestation_filter filter;
    filter.id = identifier;
    auto records = liquid::runtime::block_on(repository->attestations(std::move(filter)));
    if (!records) {
        return std::unexpected(records.error());
    }
    if (records->records.size() != 1) {
        return std::unexpected(liquid::error{
            liquid::errc::not_found, "verification attestation is absent from repository", {}});
    }
    auto encoded = liquid::encode_attestation(records->records.front().value);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    auto source = liquid::transparency::http_log_source{url};
    auto client = liquid::transparency::log_client{source};
    auto checked =
        liquid::runtime::block_on(client.verify_inclusion(*repository, std::move(*encoded)));
    if (!checked) {
        return std::unexpected(checked.error());
    }
    return {};
}

void print_replay_json(const liquid::replay_report& report, bool recorded) {
    std::print("{{\"action\":{},\"result\":{},\"recorded_exit\":{},"
               "\"reexecuted_exit\":{},\"agreement\":{},\"recorded\":{},"
               "\"attestation\":{},\"differences\":[",
               json_string(liquid::to_string(report.action)),
               json_string(liquid::to_string(report.result)),
               report.recorded_exit,
               report.reexecuted_exit,
               report.agreement,
               recorded,
               json_digest(report.attestation));
    for (auto index = std::size_t{}; index < report.differences.size(); ++index) {
        const auto& difference = report.differences[index];
        if (index != 0) {
            std::print(",");
        }
        std::print("{{\"output\":{},\"recorded\":{},\"reexecuted\":{},\"byte_offset\":{},"
                   "\"recorded_byte\":{},\"reexecuted_byte\":{}}}",
                   json_string(difference.output),
                   json_digest(difference.cached),
                   json_digest(difference.reexecuted),
                   json_number(difference.byte_offset),
                   json_number(difference.cached_byte),
                   json_number(difference.reexecuted_byte));
    }
    std::println("]}}");
}

void print_replay_text(const liquid::replay_report& report, bool recorded) {
    std::println("action={} result={} agreement={} recorded_exit={} reexecuted_exit={} recorded={} "
                 "attestation={}",
                 liquid::to_string(report.action),
                 liquid::to_string(report.result),
                 report.agreement,
                 report.recorded_exit,
                 report.reexecuted_exit,
                 recorded,
                 digest_text(report.attestation));
    for (const auto& difference : report.differences) {
        std::println(stderr,
                     "difference: output={} recorded={} reexecuted={}",
                     difference.output,
                     digest_text(difference.cached),
                     digest_text(difference.reexecuted));
        if (difference.byte_offset) {
            const auto recorded_byte =
                difference.cached_byte ? std::format("{:02x}", *difference.cached_byte) : "missing";
            const auto reexecuted_byte = difference.reexecuted_byte
                                             ? std::format("{:02x}", *difference.reexecuted_byte)
                                             : "missing";
            std::println(stderr,
                         "diff: output={} offset={} recorded={} reexecuted={}",
                         difference.output,
                         *difference.byte_offset,
                         recorded_byte,
                         reexecuted_byte);
        }
    }
}

} // namespace

auto verify_detail::run(int argc, char** argv, const verification_runner& runner) -> int {
    auto options = liquid::verify_options{};
    auto log_url = std::optional<std::string>{};
    auto selector = false;
    auto graph_mode = false;
    for (auto index = 0; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--graph") {
            graph_mode = true;
            continue;
        }
        if (argument == "--explain") {
            options.explain = true;
            continue;
        }
        if (argument == "--log") {
            if (++index == argc || log_url) {
                return usage("verify accepts one --log <log-url>");
            }
            log_url = argv[index];
            continue;
        }
        if (selector) {
            return usage("verify accepts one of <action>|--all|--changed or --graph [--all]");
        }
        if (argument == "--all") {
            options.selection = liquid::verify_selection::all;
            selector = true;
            continue;
        }
        if (argument == "--changed") {
            options.selection = liquid::verify_selection::changed;
            selector = true;
            continue;
        }
        auto action = liquid::parse_digest(argument);
        if (!action) {
            return usage("verify requires an action digest, --all, or --changed");
        }
        options.selection = liquid::verify_selection::action;
        options.action = *action;
        selector = true;
    }
    if (graph_mode) {
        if (options.selection == liquid::verify_selection::action ||
            options.selection == liquid::verify_selection::changed || log_url) {
            return usage("verify --graph accepts only --all and --explain");
        }
        auto report = liquid::runtime::block_on(
            liquid::build::verify_graph(std::filesystem::current_path(),
                                        {.all = options.selection == liquid::verify_selection::all,
                                         .explain = options.explain}));
        if (!report) {
            return fail(report.error());
        }
        auto divergent = report->graph.divergences != 0;
        for (const auto& record : report->graph.records) {
            std::println("query={} arguments={} record={} agreement={} inputs={}",
                         record.query.kind,
                         hex(record.query.arguments),
                         liquid::to_string(record.id),
                         record.agreement,
                         record.inputs.size());
            if (record.agreement) {
                continue;
            }
            std::println(stderr,
                         "critical: query={} arguments={} incremental={} clean={}",
                         record.query.kind,
                         hex(record.query.arguments),
                         liquid::to_string(record.incremental),
                         liquid::to_string(record.clean));
            for (const auto& input : record.inputs) {
                if (const auto* key = std::get_if<liquid::graph_input_key>(&input.key)) {
                    std::println(stderr,
                                 "input: {} fingerprint={}",
                                 key->name,
                                 liquid::to_string(input.fingerprint));
                }
            }
        }
        for (const auto& action : report->actions.actions) {
            if (!action.agreement) {
                divergent = true;
            }
        }
        std::println("graph_considered={} graph_validated={} graph_divergent={} backlog={} "
                     "actions_verified={} elapsed_ms={}",
                     report->graph.considered,
                     report->graph.validated,
                     report->graph.divergences,
                     report->graph.backlog,
                     report->actions.actions.size(),
                     report->graph.elapsed.count());
        return divergent ? 1 : 0;
    }
    if (!selector) {
        return usage("verify requires an action digest, --all, or --changed");
    }
    auto report = runner(std::filesystem::current_path(), options);
    if (!report) {
        return fail(report.error());
    }
    auto divergent = false;
    for (const auto& action : report->actions) {
        std::println("action={} result={} attestation={} agreement={}",
                     liquid::to_string(action.action),
                     liquid::to_string(action.result),
                     liquid::to_string(action.attestation),
                     action.agreement);
        if (action.agreement) {
            if (log_url) {
                auto inclusion = verify_log_inclusion(*log_url, action.attestation);
                if (!inclusion) {
                    return fail(inclusion.error());
                }
            }
            continue;
        }
        divergent = true;
        for (const auto& difference : action.differences) {
            std::println(stderr,
                         "critical: action={} output={} cached={} reexecuted={}",
                         liquid::to_string(action.action),
                         difference.output,
                         digest_text(difference.cached),
                         digest_text(difference.reexecuted));
            if (options.explain && difference.byte_offset) {
                const auto cached = difference.cached_byte
                                        ? std::format("{:02x}", *difference.cached_byte)
                                        : "missing";
                const auto replayed = difference.reexecuted_byte
                                          ? std::format("{:02x}", *difference.reexecuted_byte)
                                          : "missing";
                std::println(stderr,
                             "diff: output={} offset={} cached={} reexecuted={}",
                             difference.output,
                             *difference.byte_offset,
                             cached,
                             replayed);
            }
        }
    }
    std::println("considered={} verified={} divergent={}",
                 report->considered,
                 report->actions.size(),
                 divergent);
    return divergent ? 1 : 0;
}

auto verify(int argc, char** argv) -> int {
    return verify_detail::run(
        argc, argv, [](std::filesystem::path working_tree, liquid::verify_options options) {
            return liquid::runtime::block_on(
                liquid::build::verify(std::move(working_tree), options));
        });
}

auto replay(int argc, char** argv) -> int {
    auto target = std::optional<liquid::digest>{};
    auto record = false;
    auto json = false;
    for (auto index = 0; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--record") {
            record = true;
            continue;
        }
        if (argument == "--json") {
            json = true;
            continue;
        }
        if (target) {
            return usage("replay accepts one <action|result> digest");
        }
        auto parsed = liquid::parse_digest(argument);
        if (!parsed) {
            return usage("replay requires an action or result digest [--json] [--record]");
        }
        target = *parsed;
    }
    if (!target) {
        return usage("replay requires an action or result digest [--json] [--record]");
    }
    auto report = liquid::runtime::block_on(
        liquid::build::replay(std::filesystem::current_path(), *target, {.record = record}));
    if (!report) {
        return fail(report.error());
    }
    if (json) {
        print_replay_json(*report, record);
    } else {
        print_replay_text(*report, record);
    }
    return report->agreement ? 0 : 1;
}

} // namespace liquid_cli
