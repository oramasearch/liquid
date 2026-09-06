#include <cstdlib>
#include <filesystem>
#include <format>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/deploy.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace liquid_cli {
namespace {

struct options {
    std::string command{"create"};
    std::string control;
    std::string environment{"production"};
    std::string image;
    std::string rollback_target;
    bool json{};
    bool wait{};
};

auto fail(const liquid::error& error) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(error.code), error.message, error.context);
    return 1;
}

auto usage() -> int {
    std::println(stderr,
                 "error: usage: deploy [--control <url>] [--env <name>] [--image <ref@sha256:...>] "
                 "[--to <change|deployment>] [--wait] [--json] | deploy <status|list> "
                 "[--control <url>] [--env <name>] [--json]");
    return 2;
}

auto parse_options(int argc, char** argv) -> std::optional<options> {
    auto output = options{};
    auto index = 0;
    if (index < argc &&
        (std::string_view{argv[index]} == "status" || std::string_view{argv[index]} == "list")) {
        output.command = argv[index++];
    }
    for (; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--control" && index + 1 < argc && output.control.empty()) {
            output.control = argv[++index];
        } else if (argument == "--env" && index + 1 < argc) {
            output.environment = argv[++index];
        } else if (argument == "--image" && index + 1 < argc && output.image.empty()) {
            output.image = argv[++index];
        } else if (argument == "--to" && index + 1 < argc && output.rollback_target.empty()) {
            output.rollback_target = argv[++index];
        } else if (argument == "--wait" && !output.wait) {
            output.wait = true;
        } else if (argument == "--json" && !output.json) {
            output.json = true;
        } else {
            return std::nullopt;
        }
    }
    if (output.control.empty()) {
        if (const auto* configured = std::getenv("LIQUID_CONTROL_PLANE")) {
            output.control = configured;
        }
    }
    if (output.environment.empty() ||
        (output.command == "create" && output.rollback_target.empty() && output.image.empty()) ||
        (output.command != "create" &&
         (!output.image.empty() || !output.rollback_target.empty() || output.wait))) {
        return std::nullopt;
    }
    return output;
}

auto json_string(std::string_view value) -> std::string {
    auto output = std::string{"\""};
    for (const auto character : value) {
        if (character == '\\' || character == '\"') {
            output += '\\';
        }
        output += character;
    }
    output += '\"';
    return output;
}

auto record_json(const liquid::deployment_record& record) -> std::string {
    return std::format("{{\"id\":{},\"repository\":{},\"environment\":{},\"change\":{},"
                       "\"image\":{},\"result\":{},\"rollback_of\":{}}}",
                       json_string(record.id),
                       json_string(record.repository),
                       json_string(record.environment),
                       json_string(liquid::to_string(record.change.value)),
                       json_string(record.image),
                       record.result ? json_string(liquid::to_string(record.result->value))
                                     : "null",
                       record.rollback_of ? json_string(*record.rollback_of) : "null");
}

auto print_record(const liquid::deployment_record& record, bool json) -> void {
    if (json) {
        std::println("{}", record_json(record));
        return;
    }
    std::println("deployment={} environment={} change={} image={} rollback_of={}",
                 record.id,
                 record.environment,
                 liquid::to_string(record.change.value),
                 record.image,
                 record.rollback_of.value_or(""));
}

auto run(int argc, char** argv) -> int {
    auto parsed = parse_options(argc, argv);
    if (!parsed) {
        return usage();
    }
    auto opened =
        liquid::runtime::block_on(liquid::repository::open(std::filesystem::current_path()));
    if (!opened) {
        return fail(opened.error());
    }
    if (parsed->control.empty()) {
        parsed->control = opened->deploy_configuration().control_plane;
    }
    if (parsed->control.empty()) {
        return usage();
    }
    auto client_options = liquid::deploy_options{};
    if (const auto* token = std::getenv("LIQUID_TOKEN")) {
        client_options.bearer_token = token;
    }
    if (parsed->command == "list") {
        auto records = liquid::runtime::block_on(
            liquid::deploy::list(*opened, parsed->control, client_options));
        if (!records) {
            return fail(records.error());
        }
        if (parsed->json) {
            std::print("[");
            for (std::size_t index = 0; index < records->size(); ++index) {
                std::print("{}{}", index == 0 ? "" : ",", record_json((*records)[index]));
            }
            std::println("]");
        } else {
            for (const auto& record : *records) {
                print_record(record, false);
            }
        }
        return 0;
    }
    if (parsed->command == "status") {
        auto status = liquid::runtime::block_on(
            liquid::deploy::status(*opened, parsed->control, parsed->environment, client_options));
        if (!status) {
            return fail(status.error());
        }
        if (parsed->json) {
            std::println("{{\"phase\":{},\"url\":{},\"ready\":{},\"message\":{}}}",
                         json_string(status->phase),
                         json_string(status->url),
                         status->ready ? "true" : "false",
                         json_string(status->message));
        } else {
            std::println("phase={} ready={} url={} message={}",
                         status->phase,
                         status->ready,
                         status->url,
                         status->message);
        }
        return 0;
    }
    const auto configured = opened->deploy_configuration();
    const auto environment = configured.environments.find(parsed->environment);
    const auto secrets = environment == configured.environments.end() ? std::vector<std::string>{}
                                                                      : environment->second.secrets;
    auto deployed =
        parsed->rollback_target.empty()
            ? liquid::runtime::block_on(liquid::deploy::create(*opened,
                                                               parsed->control,
                                                               {.environment = parsed->environment,
                                                                .image = parsed->image,
                                                                .secret_references = secrets},
                                                               client_options))
            : liquid::runtime::block_on(liquid::deploy::rollback(*opened,
                                                                 parsed->control,
                                                                 parsed->environment,
                                                                 parsed->rollback_target,
                                                                 client_options));
    if (!deployed) {
        return fail(deployed.error());
    }
    print_record(*deployed, parsed->json);
    if (parsed->wait) {
        auto current = liquid::runtime::block_on(
            liquid::deploy::status(*opened, parsed->control, parsed->environment, client_options));
        if (!current) {
            return fail(current.error());
        }
        if (!current->ready) {
            return fail({liquid::errc::unavailable, current->message, current->phase});
        }
    }
    return 0;
}

} // namespace

auto deploy(int argc, char** argv) -> int {
    return run(argc, argv);
}

} // namespace liquid_cli
