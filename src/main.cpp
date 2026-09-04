#include <array>
#include <cstddef>
#include <format>
#include <print>
#include <string>
#include <string_view>

#include "liquid/liquid.hpp"

namespace liquid_cli {

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
auto attestation(int argc, char** argv) -> int;

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
    {"log", "log [--json]", "show change history", "available", liquid_cli::repo_log},
    {"cut", "cut <message>", "cut the working change", "available", liquid_cli::repo_cut},
    {"refs", "refs [--json]", "list repository references", "available", liquid_cli::repo_refs},
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
