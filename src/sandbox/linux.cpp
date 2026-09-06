#include "sandbox/sandbox.hpp"

#if defined(__linux__)

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

#ifndef LIQUID_READ_GUARD_PATH
#define LIQUID_READ_GUARD_PATH ""
#endif
#ifndef LIQUID_READ_GUARD_FILENAME
#define LIQUID_READ_GUARD_FILENAME "liquid-read-guard"
#endif

namespace liquid::sandbox_detail {
namespace {

auto quote(std::string_view value) -> std::string {
    auto output = std::string{"'"};
    for (const auto character : value) {
        if (character == '\'')
            output += "'\\''";
        else
            output += character;
    }
    output += '\'';
    return output;
}

auto ensure_directory(const std::filesystem::path& path) -> result<void> {
    auto code = std::error_code{};
    std::filesystem::create_directories(path, code);
    if (code)
        return std::unexpected(error{errc::io, code.message(), path.string()});
    return {};
}

auto create_file(const std::filesystem::path& path) -> result<void> {
    auto parent = ensure_directory(path.parent_path());
    if (!parent)
        return parent;
    auto stream = std::ofstream{path, std::ios::binary | std::ios::trunc};
    if (!stream)
        return std::unexpected(error{errc::io, "cannot create declared output", path.string()});
    return {};
}

auto read_guard_path() -> result<std::filesystem::path> {
    auto candidates = std::vector<std::filesystem::path>{};
    if (const auto* configured = std::getenv("LIQUID_READ_GUARD"))
        candidates.emplace_back(configured);
    candidates.emplace_back(LIQUID_READ_GUARD_PATH);
    auto code = std::error_code{};
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", code);
    if (!code) {
        candidates.push_back(executable.parent_path() / "../libexec/liquid" /
                             LIQUID_READ_GUARD_FILENAME);
    }
    for (const auto& candidate : candidates) {
        if (!candidate.empty() && std::filesystem::is_regular_file(candidate))
            return std::filesystem::canonical(candidate);
    }
    return std::unexpected(error{
        errc::not_found, "cannot locate the remote action read guard", LIQUID_READ_GUARD_FILENAME});
}

auto read_violation(const launch_request& request) -> std::optional<std::string> {
    if (!request.remote_eligible)
        return {};
    auto input = std::ifstream{request.sandbox_root / "read-violation"};
    auto path = std::string{};
    if (input && std::getline(input, path) && !path.empty())
        return path;
    return {};
}

auto write_bootstrap(const launch_request& request,
                     const std::filesystem::path& script,
                     const std::optional<std::filesystem::path>& read_guard) -> result<void> {
    auto stream = std::ofstream{script, std::ios::binary | std::ios::trunc};
    if (!stream)
        return std::unexpected(error{errc::io, "cannot create sandbox bootstrap", script.string()});
    const auto rooted = [&](std::string_view virtual_path) {
        return request.sandbox_root / std::filesystem::path{virtual_path}.relative_path();
    };
    stream << "set -eu\n";
    stream << "/usr/bin/mount --make-rprivate /\n";
    stream << "/usr/bin/mount --bind " << quote(request.input_mount.string()) << ' '
           << quote(rooted(request.work_root).string()) << "\n";
    stream << "/usr/bin/mount --bind " << quote(request.toolchain.string()) << ' '
           << quote(rooted(request.toolchain_root).string()) << "\n";
    stream << "/usr/bin/mount -o remount,bind,ro " << quote(rooted(request.toolchain_root).string())
           << "\n";
    for (const auto* alias : {"etc", "lib", "lib64"}) {
        const auto source = request.toolchain / alias;
        if (!std::filesystem::is_directory(source)) {
            continue;
        }
        const auto destination = request.sandbox_root / alias;
        auto created = ensure_directory(destination);
        if (!created) {
            return created;
        }
        stream << "/usr/bin/mount --bind " << quote(source.string()) << ' '
               << quote(destination.string()) << "\n";
        stream << "/usr/bin/mount -o remount,bind,ro " << quote(destination.string()) << "\n";
    }
    for (const auto& output : request.outputs) {
        const auto directory = output.ends_with('/');
        const auto relative = directory ? output.substr(0, output.size() - 1) : output;
        const auto source = request.output_root / relative;
        if (directory) {
            auto created = ensure_directory(source);
            if (!created)
                return created;
        } else {
            auto created = create_file(source);
            if (!created)
                return created;
        }
        stream << "/usr/bin/mount --bind " << quote(source.string()) << ' '
               << quote((rooted(request.work_root) / relative).string()) << "\n";
    }
    stream << "/usr/bin/mount -t proc proc " << quote((request.sandbox_root / "proc").string())
           << "\n";
    if (request.remote_eligible) {
        stream << "/usr/bin/mount --bind " << quote(read_guard->string()) << ' '
               << quote(rooted("/liquid/read-guard").string()) << "\n";
        stream << "/usr/bin/mount --bind "
               << quote((request.sandbox_root / "read-violation").string()) << ' '
               << quote(rooted("/liquid/read-violation").string()) << "\n";
    }
    stream << "unset PWD\n";
    stream << "exec /usr/bin/unshare --root=" << quote(request.sandbox_root.string())
           << " --wd=" << quote(request.cwd) << " -- ";
    if (request.remote_eligible) {
        stream << "/liquid/read-guard /liquid/read-violation " << quote(request.work_root) << ' '
               << quote(request.toolchain_root) << ' ';
    }
    for (const auto& argument : request.command)
        stream << quote(argument) << ' ';
    stream << "\n";
    stream.close();
    if (!stream)
        return std::unexpected(error{errc::io, "cannot write sandbox bootstrap", script.string()});
    return {};
}

auto write_wrap_bootstrap(const wrap_launch_request& request, const std::filesystem::path& script)
    -> result<void> {
    auto stream = std::ofstream{script, std::ios::binary | std::ios::trunc};
    if (!stream)
        return std::unexpected(error{errc::io, "cannot create wrap bootstrap", script.string()});
    const auto rooted = [&](std::string_view path) {
        return request.sandbox_root / std::filesystem::path{path}.relative_path();
    };
    stream << "set -eu\n";
    stream << "/usr/bin/mount --make-rprivate /\n";
    stream << "/usr/bin/mount --bind " << quote(request.input_mount.string()) << ' '
           << quote(rooted("/liquid/work").string()) << "\n";
    if (!request.toolchain_mount.empty()) {
        stream << "/usr/bin/mount --bind " << quote(request.toolchain_mount.string()) << ' '
               << quote(rooted("/liquid/toolchain").string()) << "\n";
    }
    for (const auto* path : {"/usr", "/usr/local", "/bin", "/lib", "/lib64", "/etc"}) {
        if (!std::filesystem::exists(path))
            continue;
        const auto destination = rooted(path);
        stream << "/usr/bin/mount --rbind " << quote(path) << ' ' << quote(destination.string())
               << "\n";
        stream << "/usr/bin/mount -o remount,bind,ro " << quote(destination.string()) << "\n";
    }
    stream << "/usr/bin/mount --rbind /dev " << quote(rooted("/dev").string()) << "\n";
    stream << "/usr/bin/mount --bind " << quote(request.hook.string()) << ' '
           << quote(rooted("/liquid/wrap-hook.so").string()) << "\n";
    stream << "/usr/bin/mount --bind " << quote(request.trace.string()) << ' '
           << quote(rooted("/liquid/trace").string()) << "\n";
    stream << "/usr/bin/mount --bind " << quote(request.plan.string()) << ' '
           << quote(rooted("/liquid/plan").string()) << "\n";
    stream << "/usr/bin/mount -t tmpfs tmpfs " << quote(rooted("/tmp").string()) << "\n";
    stream << "/usr/bin/mkdir -p " << quote(rooted("/tmp/home").string()) << "\n";
    stream << "/usr/bin/mount --rbind /proc " << quote(rooted("/proc").string()) << "\n";
    stream << "exec /usr/bin/unshare --root=" << quote(request.sandbox_root.string())
           << " --wd=" << quote(request.cwd) << " -- /usr/bin/env -i";
    for (const auto& [name, value] : request.environment)
        stream << ' ' << quote(name + "=" + value);
    for (const auto& argument : request.command)
        stream << ' ' << quote(argument);
    stream << "\n";
    stream.close();
    if (!stream)
        return std::unexpected(error{errc::io, "cannot write wrap bootstrap", script.string()});
    return {};
}

auto environment_block(const std::map<std::string, std::string>& values)
    -> std::vector<std::string> {
    auto output = std::vector<std::string>{};
    output.reserve(values.size());
    for (const auto& [name, value] : values)
        output.push_back(name + "=" + value);
    return output;
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
    auto created = ensure_directory(request.sandbox_root / "proc");
    if (!created)
        return std::unexpected(created.error());
    for (const auto& path :
         {request.work_root, request.toolchain_root, request.home_root, request.temporary_root}) {
        created =
            ensure_directory(request.sandbox_root / std::filesystem::path{path}.relative_path());
        if (!created)
            return std::unexpected(created.error());
    }
    if (request.remote_eligible) {
        created = ensure_directory(request.sandbox_root / "liquid");
        if (!created)
            return std::unexpected(created.error());
        for (const auto& path : {request.sandbox_root / "liquid/read-guard",
                                 request.sandbox_root / "liquid/read-violation",
                                 request.sandbox_root / "read-violation"}) {
            auto file = create_file(path);
            if (!file)
                return std::unexpected(file.error());
        }
    }
    const auto script = request.sandbox_root / "bootstrap.sh";
    auto read_guard = std::optional<std::filesystem::path>{};
    if (request.remote_eligible) {
        auto located = read_guard_path();
        if (!located)
            return std::unexpected(located.error());
        read_guard = std::move(*located);
    }
    auto written = write_bootstrap(request, script, read_guard);
    if (!written)
        return std::unexpected(written.error());
    const auto stdout_descriptor =
        ::open(request.stdout_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (stdout_descriptor < 0)
        return std::unexpected(error{errc::io, std::strerror(errno), request.stdout_path.string()});
    const auto stderr_descriptor =
        ::open(request.stderr_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (stderr_descriptor < 0) {
        ::close(stdout_descriptor);
        return std::unexpected(error{errc::io, std::strerror(errno), request.stderr_path.string()});
    }
    auto arguments = std::vector<std::string>{"unshare",
                                              "--user",
                                              "--map-root-user",
                                              "--mount",
                                              "--pid",
                                              "--fork",
                                              "--net",
                                              "/bin/sh",
                                              script.string()};
    auto argument_pointers = pointers(arguments);
    auto environment = environment_block(request.environment);
    auto environment_pointers = pointers(environment);
    auto actions = posix_spawn_file_actions_t{};
    auto attributes = posix_spawnattr_t{};
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, stdout_descriptor, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, stderr_descriptor, STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, stdout_descriptor);
    posix_spawn_file_actions_addclose(&actions, stderr_descriptor);
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);
    auto pid = pid_t{};
    const auto spawned = posix_spawn(&pid,
                                     "/usr/bin/unshare",
                                     &actions,
                                     &attributes,
                                     argument_pointers.data(),
                                     environment_pointers.data());
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    ::close(stdout_descriptor);
    ::close(stderr_descriptor);
    if (spawned != 0)
        return std::unexpected(error{errc::io, std::strerror(spawned), "posix_spawn unshare"});
    auto stop_callback = std::stop_callback{request.stop, [pid] { ::kill(-pid, SIGKILL); }};
    const auto deadline = request.timeout
                              ? std::optional{std::chrono::steady_clock::now() + *request.timeout}
                              : std::nullopt;
    auto status = int{};
    for (;;) {
        const auto waited = ::waitpid(pid, &status, WNOHANG);
        if (waited == pid)
            return launch_result{
                exit_code(status), false, read_violation(request), request.stop.stop_requested()};
        if (waited < 0)
            return std::unexpected(error{errc::io, std::strerror(errno), "waitpid"});
        if (deadline && std::chrono::steady_clock::now() >= *deadline) {
            ::kill(-pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            return launch_result{128 + SIGKILL, true, read_violation(request), false};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
}

auto launch_wrap(const wrap_launch_request& request) -> result<launch_result> {
    auto created = ensure_directory(request.sandbox_root / "liquid/work");
    if (!created)
        return std::unexpected(created.error());
    if (!request.toolchain_mount.empty()) {
        created = ensure_directory(request.sandbox_root / "liquid/toolchain");
        if (!created)
            return std::unexpected(created.error());
    }
    for (const auto* path :
         {"usr", "usr/local", "bin", "lib", "lib64", "etc", "dev", "tmp", "proc"}) {
        created = ensure_directory(request.sandbox_root / path);
        if (!created)
            return std::unexpected(created.error());
    }
    for (const auto* path : {"liquid/wrap-hook.so", "liquid/trace", "liquid/plan"}) {
        auto file = create_file(request.sandbox_root / path);
        if (!file)
            return std::unexpected(file.error());
    }
    const auto script = request.sandbox_root / "wrap-bootstrap.sh";
    auto written = write_wrap_bootstrap(request, script);
    if (!written)
        return std::unexpected(written.error());
    auto arguments = std::vector<std::string>{"unshare", "--user", "--map-root-user", "--mount"};
    if (!request.network) {
        arguments.push_back("--net");
    }
    arguments.push_back("/bin/sh");
    arguments.push_back(script.string());
    auto argument_pointers = pointers(arguments);
    auto environment = environment_block({});
    auto environment_pointers = pointers(environment);
    auto actions = posix_spawn_file_actions_t{};
    auto attributes = posix_spawnattr_t{};
    posix_spawn_file_actions_init(&actions);
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
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);
    auto pid = pid_t{};
    const auto spawned = posix_spawn(&pid,
                                     "/usr/bin/unshare",
                                     &actions,
                                     &attributes,
                                     argument_pointers.data(),
                                     environment_pointers.data());
    posix_spawnattr_destroy(&attributes);
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
