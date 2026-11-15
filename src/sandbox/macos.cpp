// SPDX-License-Identifier: MIT

#include "sandbox/sandbox.hpp"

#if defined(__APPLE__)

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace liquid::sandbox_detail {
namespace {

auto escaped(std::string value) -> std::string {
    for (auto offset = std::size_t{}; (offset = value.find('"', offset)) != std::string::npos;) {
        value.insert(offset, "\\");
        offset += 2;
    }
    return value;
}

auto write_profile(const launch_request& request, const std::filesystem::path& path)
    -> result<void> {
    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
    if (!output)
        return std::unexpected(
            error{errc::io, "cannot create sandbox-exec profile", path.string()});
    output << "(version 1)\n(deny default)\n(allow process*)\n(deny network*)\n";
    output << "(allow file-read* (subpath \"" << escaped(request.work_root) << "\"))\n";
    output << "(allow file-read* (subpath \"" << escaped(request.toolchain_root) << "\"))\n";
    output << "(allow file-read* (subpath \"/System/Library\") (subpath \"/usr/lib\"))\n";
    output << "(allow file-write* (subpath \"" << escaped(request.output_root.string())
           << "\") (subpath \"" << escaped(request.temporary_root) << "\"))\n";
    output.close();
    if (!output)
        return std::unexpected(error{errc::io, "cannot write sandbox-exec profile", path.string()});
    return {};
}

auto write_wrap_profile(const wrap_launch_request& request, const std::filesystem::path& path)
    -> result<void> {
    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
    if (!output)
        return std::unexpected(
            error{errc::io, "cannot create wrap sandbox profile", path.string()});
    if (request.network_only) {
        output << "(version 1)\n(allow default)\n(deny network*)\n";
        output << "(allow network* (local unix-socket) (remote unix-socket))\n";
        output.close();
        if (!output) {
            return std::unexpected(
                error{errc::io, "cannot write wrap sandbox profile", path.string()});
        }
        return {};
    }
    output << "(version 1)\n(deny default)\n(allow process*)\n";
    output << (request.network ? "(allow network*)\n" : "(deny network*)\n");
    if (request.network) {
        output << "(allow sysctl-read)\n(allow ipc-posix-shm-read-data)\n"
                  "(allow mach-lookup)\n";
        output << "(allow file-read* (literal \"/\") (literal \"/etc\") "
                  "(literal \"/opt\") (literal \"/private\") "
                  "(subpath \"/private/etc\") (subpath \"/Library/Preferences\") "
                  "(literal \"/dev/dtracehelper\"))\n";
    }
    output << "(allow file-read* (subpath \"" << escaped(request.input_mount.string()) << "\"))\n";
    if (!request.toolchain_mount.empty()) {
        output << "(allow file-read* (subpath \"" << escaped(request.toolchain_mount.string())
               << "\"))\n";
    }
    output << "(allow file-read* (literal \"" << escaped(request.hook.string()) << "\") (literal \""
           << escaped(request.plan.string()) << "\"))\n";
    output << "(allow file-read* (subpath \"/System\") (subpath \"/usr\") "
              "(subpath \"/opt/homebrew\") (subpath \"/Library/Developer\"))\n";
    output << "(allow file-write* (subpath \"" << escaped(request.input_mount.string())
           << "\") (literal \"" << escaped(request.trace.string()) << "\")";
    if (!request.stdout_path.empty()) {
        output << " (literal \"" << escaped(request.stdout_path.string()) << "\")";
    }
    if (!request.stderr_path.empty()) {
        output << " (literal \"" << escaped(request.stderr_path.string()) << "\")";
    }
    output << " (subpath \"/tmp\") (subpath \"/private/tmp\"))\n";
    output.close();
    if (!output)
        return std::unexpected(error{errc::io, "cannot write wrap sandbox profile", path.string()});
    return {};
}

auto pointers(std::vector<std::string>& values) -> std::vector<char*> {
    auto output = std::vector<char*>{};
    output.reserve(values.size() + 1);
    for (auto& value : values)
        output.push_back(value.data());
    output.push_back(nullptr);
    return output;
}

auto exit_code(int status) -> int {
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 255;
}

} // namespace

auto launch(const launch_request& request) -> result<launch_result> {
    const auto profile = request.sandbox_root / "sandbox.sb";
    auto written = write_profile(request, profile);
    if (!written)
        return std::unexpected(written.error());
    return std::unexpected(
        error{errc::unsupported,
              "sandbox-exec actions require the fixed /liquid mount prepared by the executor",
              profile.string()});
}

auto launch_wrap(const wrap_launch_request& request) -> result<launch_result> {
    auto code = std::error_code{};
    std::filesystem::create_directories(request.sandbox_root, code);
    if (code)
        return std::unexpected(error{errc::io, code.message(), request.sandbox_root.string()});
    const auto profile = request.sandbox_root / "wrap.sb";
    auto written = write_wrap_profile(request, profile);
    if (!written)
        return std::unexpected(written.error());
    auto arguments =
        std::vector<std::string>{"sandbox-exec", "-f", profile.string(), "/usr/bin/env", "-i"};
    for (const auto& [name, value] : request.environment)
        arguments.push_back(name + "=" + value);
    arguments.insert(arguments.end(), request.command.begin(), request.command.end());
    auto argument_pointers = pointers(arguments);
    auto environment = std::vector<std::string>{};
    auto environment_pointers = pointers(environment);
    auto actions = posix_spawn_file_actions_t{};
    posix_spawn_file_actions_init(&actions);
    auto working_directory = request.input_mount;
    constexpr auto work_root = std::string_view{"/liquid/work"};
    if (request.cwd != work_root) {
        const auto prefix = std::string{work_root} + "/";
        if (!request.cwd.starts_with(prefix)) {
            posix_spawn_file_actions_destroy(&actions);
            return std::unexpected(error{errc::invalid_argument, "invalid wrap cwd", request.cwd});
        }
        working_directory /= request.cwd.substr(prefix.size());
    }
    posix_spawn_file_actions_addchdir_np(&actions, working_directory.c_str());
    auto stdout_descriptor = -1;
    auto stderr_descriptor = -1;
    if (!request.stdout_path.empty()) {
        stdout_descriptor = ::open(request.stdout_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
        if (stdout_descriptor < 0) {
            posix_spawn_file_actions_destroy(&actions);
            return std::unexpected(
                error{errc::io, std::strerror(errno), request.stdout_path.string()});
        }
        posix_spawn_file_actions_adddup2(&actions, stdout_descriptor, STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, stdout_descriptor);
    }
    if (!request.stderr_path.empty()) {
        stderr_descriptor = ::open(request.stderr_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
        if (stderr_descriptor < 0) {
            if (stdout_descriptor >= 0) {
                ::close(stdout_descriptor);
            }
            posix_spawn_file_actions_destroy(&actions);
            return std::unexpected(
                error{errc::io, std::strerror(errno), request.stderr_path.string()});
        }
        posix_spawn_file_actions_adddup2(&actions, stderr_descriptor, STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, stderr_descriptor);
    }
    auto pid = pid_t{};
    const auto spawned = posix_spawn(&pid,
                                     "/usr/bin/sandbox-exec",
                                     &actions,
                                     nullptr,
                                     argument_pointers.data(),
                                     environment_pointers.data());
    posix_spawn_file_actions_destroy(&actions);
    if (stdout_descriptor >= 0) {
        ::close(stdout_descriptor);
    }
    if (stderr_descriptor >= 0) {
        ::close(stderr_descriptor);
    }
    if (spawned != 0)
        return std::unexpected(error{errc::io, std::strerror(spawned), "posix_spawn wrap"});
    auto status = int{};
    if (::waitpid(pid, &status, 0) < 0)
        return std::unexpected(error{errc::io, std::strerror(errno), "waitpid wrap"});
    return launch_result{exit_code(status), false};
}

} // namespace liquid::sandbox_detail

#endif
