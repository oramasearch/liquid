#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <print>
#include <string>
#include <string_view>

#include "liquid/liquid.hpp"

namespace liquid_cli {

auto set_executable(std::filesystem::path path) -> void;

auto repo_init(int argc, char** argv) -> int;
auto repo_status(int argc, char** argv) -> int;
auto repo_log(int argc, char** argv) -> int;
auto repo_cut(int argc, char** argv) -> int;
auto repo_refs(int argc, char** argv) -> int;
auto tree_move(int argc, char** argv) -> int;
auto tree_remove(int argc, char** argv) -> int;
auto compose_nest(int argc, char** argv) -> int;
auto compose_split(int argc, char** argv) -> int;
auto compose_combine(int argc, char** argv) -> int;
auto capture_pause(int argc, char** argv) -> int;
auto capture_resume(int argc, char** argv) -> int;
auto capture_watch(int argc, char** argv) -> int;
auto history_undo(int argc, char** argv) -> int;
auto history_operations(int argc, char** argv) -> int;
auto locks_lock(int argc, char** argv) -> int;
auto locks_unlock(int argc, char** argv) -> int;
auto identity(int argc, char** argv) -> int;
auto vfs_mount(int argc, char** argv) -> int;
auto vfs_materialise(int argc, char** argv) -> int;
auto garbage_collect(int argc, char** argv) -> int;
auto retain(int argc, char** argv) -> int;
auto attestation(int argc, char** argv) -> int;
auto verify(int argc, char** argv) -> int;
auto replay(int argc, char** argv) -> int;
auto provenance(int argc, char** argv) -> int;
auto policy(int argc, char** argv) -> int;
auto sbom(int argc, char** argv) -> int;
auto wrap(int argc, char** argv) -> int;
auto why(int argc, char** argv) -> int;
auto cache(int argc, char** argv) -> int;
auto git_import(int argc, char** argv) -> int;
auto git_export(int argc, char** argv) -> int;
auto build(int argc, char** argv) -> int;
auto why_slow(int argc, char** argv) -> int;
auto facts(int argc, char** argv) -> int;
auto delta(int argc, char** argv) -> int;
auto sync_push(int argc, char** argv) -> int;
auto sync_pull(int argc, char** argv) -> int;
auto serve(int argc, char** argv) -> int;
auto daemon_command(int argc, char** argv) -> int;

} // namespace liquid_cli

namespace {

using command_handler = auto (*)(int argc, char** argv) -> int;

struct command {
    std::string_view name;
    std::string_view usage;
    std::string_view summary;
    std::string_view availability;
    command_handler handler;
};

constexpr auto commands = std::to_array<command>({
    {"version", "version [--json]", "print the version", "available", nullptr},
    {"init",
     "init [path]",
     "create a repository and import its regular files",
     "available",
     liquid_cli::repo_init},
    {"status", "status [--json]", "show repository status", "available", liquid_cli::repo_status},
    {"daemon",
     "daemon [--shutdown]",
     "start or stop the checkout daemon",
     "available on macOS and Linux",
     liquid_cli::daemon_command},
    {"log", "log [--json]", "show change history", "available", liquid_cli::repo_log},
    {"cut", "cut <message>", "cut the working change", "available", liquid_cli::repo_cut},
    {"refs", "refs [--json]", "list repository references", "available", liquid_cli::repo_refs},
    {"push",
     "push <url>",
     "push repository changes and objects",
     "available",
     liquid_cli::sync_push},
    {"pull",
     "pull <url>",
     "pull repository changes and objects",
     "available",
     liquid_cli::sync_pull},
    {"serve",
     "serve sync <path> [--listen <address>] [--port <port>]",
     "serve a repository over the sync protocol",
     "available",
     liquid_cli::serve},
    {"move", "move <from> <to>", "record a path move", "available", liquid_cli::tree_move},
    {"rm", "rm <path>", "record a path removal", "available", liquid_cli::tree_remove},
    {"nest",
     "nest <repository> <path>",
     "nest another repository",
     "available",
     liquid_cli::compose_nest},
    {"split",
     "split <path> <into>",
     "split a subtree into a repository",
     "available",
     liquid_cli::compose_split},
    {"combine",
     "combine <repository> [--at <path>]",
     "combine another repository",
     "available",
     liquid_cli::compose_combine},
    {"ops",
     "ops [--json]",
     "list recorded operations",
     "available",
     liquid_cli::history_operations},
    {"undo",
     "undo [op-id]",
     "undo the newest or named operation",
     "available",
     liquid_cli::history_undo},
    {"lock", "lock <path>", "advisory-lock a path", "available", liquid_cli::locks_lock},
    {"unlock", "unlock <path>", "remove an advisory lock", "available", liquid_cli::locks_unlock},
    {"mount",
     "mount <mountpoint> [--read-only]",
     "mount a lazy working tree",
     "available",
     liquid_cli::vfs_mount},
    {"materialise",
     "materialise <target> [path]",
     "write a full working tree",
     "available",
     liquid_cli::vfs_materialise},
    {"gc",
     "gc [--dry-run] [--json] [--explain] [--compact] [--compact-below <fraction>] [repository]",
     "checkpoint and collect a repository",
     "available",
     liquid_cli::garbage_collect},
    {"import",
     "import git <path>",
     "import a local Git repository",
     "available",
     liquid_cli::git_import},
    {"export",
     "export git <path> [ref]",
     "export as a Git repository",
     "available",
     liquid_cli::git_export},
    {"retain",
     "retain explain <digest>",
     "explain retention for an object",
     "available",
     liquid_cli::retain},
    {"identity",
     "identity [show|rotate] [--json]",
     "show or rotate the device identity",
     "available",
     liquid_cli::identity},
    {"attest",
     "attest <show|verify> <digest>",
     "show or verify an attestation",
     "available",
     liquid_cli::attestation},
    {"delta",
     "delta <create|apply|fetch> ...",
     "create, apply, or fetch an artifact delta",
     "available",
     liquid_cli::delta},
    {"verify",
     "verify <action|--all|--changed> [--explain]",
     "re-execute and compare cached actions",
     "available on Linux with FUSE and on macOS with macFUSE",
     liquid_cli::verify},
    {"replay",
     "replay <action|result> [--json] [--record]",
     "reproduce and compare a recorded action",
     "available on Linux with FUSE and on macOS with macFUSE",
     liquid_cli::replay},
    {"provenance",
     "provenance [--dsse] <result|path>",
     "print and verify build provenance",
     "available",
     liquid_cli::provenance},
    {"policy",
     "policy <check|explain> [target]",
     "check or explain build-input policy",
     "available",
     liquid_cli::policy},
    {"sbom",
     "sbom <target> <--spdx|--cyclonedx>",
     "print a derived or explicitly inferred SBOM",
     "available",
     liquid_cli::sbom},
    {"wrap",
     "wrap [--jobs <count>] [--target <triple>] [--remote <endpoint>] [remote options] "
     "[--no-cache] [--no-verify] [--explain] <command> "
     "[args...]",
     "run an existing build as cached actions",
     "available on Linux with FUSE and on macOS with macFUSE",
     liquid_cli::wrap},
    {"build",
     "build --profile [--remote <endpoint>] [remote options] <command> [args...] | build trace "
     "[--chrome]",
     "profile a build or export its trace",
     "available on Linux with FUSE and on macOS with macFUSE",
     liquid_cli::build},
    {"why",
     "why <target> [--json]",
     "explain the latest cache lookup",
     "available",
     liquid_cli::why},
    {"why-slow",
     "why-slow <target>",
     "show the critical path to a build target",
     "available",
     liquid_cli::why_slow},
    {"cache",
     "cache stats [--json]",
     "show build-cache statistics",
     "available",
     liquid_cli::cache},
    {"facts",
     "facts <list|show|export> [digest]",
     "inspect or export configure facts",
     "available",
     liquid_cli::facts},
    {"serve",
     "serve reapi --listen <address> [--instance <name>]",
     "serve the repository CAS and action cache over REAPI",
     "available",
     liquid_cli::serve},
    {"pause", "pause", "pause working-tree capture", "available", liquid_cli::capture_pause},
    {"resume", "resume", "resume working-tree capture", "available", liquid_cli::capture_resume},
    {"watch", "watch", "capture changes until Ctrl-C", "available", liquid_cli::capture_watch},
});

auto print_usage() -> void {
    std::println("liquid — a version control system, build system, package manager,");
    std::println("artifact service and runtime in one program.");
    std::println("");
    std::println("usage: liquid <command> [options]");
    std::println("");
    std::println("commands:");
    for (const auto& command : commands) {
        const auto suffix = command.availability == "available"
                                ? std::string{}
                                : std::format(" ({})", command.availability);
        std::println("  {:{}} {}{}", command.name, 16, command.summary, suffix);
    }
}

auto usage_error(std::string_view message) -> int {
    std::println(stderr, "error: usage: {} ()", message);
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    liquid_cli::set_executable(argv[0]);
    if (argc < 2) {
        print_usage();
        return 2;
    }
    const auto command_name = std::string_view{argv[1]};
    if (command_name == "--help" || command_name == "help") {
        print_usage();
        return 0;
    }
    if (command_name == "--version" || command_name == "version") {
        if (argc != 2 && !(argc == 3 && std::string_view{argv[2]} == "--json")) {
            return usage_error("version accepts only --json");
        }
        if (argc == 3) {
            std::println("{{\"version\":\"{}\"}}", liquid::version_string());
            return 0;
        }
        std::println("liquid {}", liquid::version_string());
        return 0;
    }
    for (const auto& command : commands) {
        if (command.name != command_name) {
            continue;
        }
        if (!command.handler) {
            return usage_error(std::format("{} {}", command.name, command.availability));
        }
        return command.handler(argc - 2, argv + 2);
    }
    return usage_error(std::format("unknown command '{}'", command_name));
}
